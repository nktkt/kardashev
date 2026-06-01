#!/usr/bin/env bash
# v32 Phase 176 smoke test — user-defined effects + effect HANDLERS (algebraic
# effects, the tail-resumptive / dynamically-scoped subset).
#
#   effect E { fn op(a: A) -> R; … }          -- declare an effect + its ops
#   perform E::op(args)                        -- invoke an op (resumes here w/ R)
#   handle { body } with E { op(p) => hbody }  -- install handlers for body's extent
#
# A `perform` dispatches to the dynamically-current handler for E::op (set by an
# enclosing `handle … with E` anywhere up the call stack); the handler returns a
# value and control RESUMES at the perform site (tail-resumptive). `handle`
# DISCHARGES E from the body's effect row. Handler arms are desugared to closures
# that capture by REFERENCE, so multiple arms share live handle-scope state (a
# State effect's get/put see the same cell). `effect`/`handle`/`with`/`perform`
# are CONTEXTUAL keywords (a variable named `handle` still works). Differentially
# gated JIT vs AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" \
    "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n")
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n")
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || {
        echo "FAIL [$name]: expected error containing '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# 1. A logging effect: each `perform` calls the handler (which prints + returns),
#    control resumes; the handle discharges the effect so `main` needn't declare it.
diff_run logger $'1\n2\n42' '
effect Logger { fn log(msg: i64) -> i64; }
fn work() -> i64 ! { Logger } { perform Logger::log(1); perform Logger::log(2); 42 }
fn main() -> i64 ! { io } {
    let r = handle { work() } with Logger { log(m) => { print(m); m } };
    print(r);
    0
}
'

# 2. A State effect: get/put share a live mutable cell (the by-reference handler
#    capture). get=10, put 15, get=15 -> 15; the cell ends at 15.
diff_run state $'15\n15' '
effect State { fn get() -> i64; fn put(v: i64) -> i64; }
fn counter() -> i64 ! { State } {
    let a = perform State::get();
    perform State::put(a + 5);
    let b = perform State::get();
    b
}
fn main() -> i64 ! { io } {
    let mut cell = 10;
    let r = handle { counter() } with State { get() => cell, put(v) => { cell = v; 0 } };
    print(r);
    print(cell);
    0
}
'

# 3. The handler RESUMES with a computed value (a Reader/config effect): perform
#    returns the handler result into the computation.
diff_run reader $'30' '
effect Cfg { fn base() -> i64; }
fn compute() -> i64 ! { Cfg } { let b = perform Cfg::base(); b * 3 }
fn main() -> i64 ! { io } {
    print(handle { compute() } with Cfg { base() => 10 });   // 10*3
    0
}
'

# 4. The performing fn declares the effect; a DIFFERENT fn installs the handler
#    (dynamic scoping across a call boundary).
diff_run dynamic_scope $'99' '
effect Ask { fn ask() -> i64; }
fn middle() -> i64 ! { Ask } { perform Ask::ask() + 1 }
fn run() -> i64 { handle { middle() } with Ask { ask() => 98 } }
fn main() -> i64 ! { io } { print(run()); 0 }
'

# 5. `handle` / `with` as ordinary IDENTIFIERS still work (contextual keywords).
diff_run handle_identifier $'7' '
async fn f() -> i64 { 7 }
fn main() -> i64 ! { io } { let handle = spawn(f()); print(join(handle)); 0 }
'

# 6. NEGATIVE: performing an undeclared effect is a type error.
expect_err undeclared_effect 'unknown effect' '
fn main() -> i64 ! { io } { print(perform Ghost::boo()); 0 }
'

# 7. NEGATIVE: a fn that performs an effect without handling it must DECLARE it.
expect_err undeclared_row 'effect `Logger`' '
effect Logger { fn log(m: i64) -> i64; }
fn work() -> i64 { perform Logger::log(1) }
fn main() -> i64 ! { io } { print(handle { work() } with Logger { log(m) => m }); 0 }
'

# 8. No double-free / leak: a tight loop of handle+perform stays clean under
#    MALLOC_CHECK_=3 (handler closures + dynamic-handler save/restore).
cat > "$TMP/loop.kd" <<'EOF'
effect Add { fn add(a: i64, b: i64) -> i64; }
fn use_add(x: i64) -> i64 ! { Add } { perform Add::add(x, 1) }
fn main() -> i64 ! { io } {
    let mut i = 0;
    let mut acc = 0;
    while i < 50000 {
        acc = acc + handle { use_add(i) } with Add { add(a, b) => a + b };
        i = i + 1;
    }
    print(acc);   // sum(i+1, i=0..50000) = sum(1..50000) = 1250025000
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null 2>&1
got=$(MALLOC_CHECK_=3 "$TMP/loop" 2>&1)
[[ "$got" == "1250025000" ]] || { echo "FAIL [loop]: expected 1250025000 got: $got"; exit 1; }
echo "PASS: handle_perform_loop_clean (1250025000)"

echo "ALL PHASE 176 SMOKE TESTS PASSED"
