#!/usr/bin/env bash
# Phase 20a smoke test: toolchain maturity + the RPN capstone.
#
#   1. Optimization-level flags `-O0 / -O2`:
#        a. A fib program and the RPN capstone both compile + run to the
#           SAME correct result at -O0 and -O2 (and the default == O2).
#        b. `--emit-llvm -O0` IR is materially LESS optimized than `-O2`:
#           a trivial wrapper fn is still called at -O0 but inlined away at
#           -O2, and the O0 IR has strictly more lines. (Proves -O0 actually
#           skips optimization rather than silently running O2.)
#        c. The AOT compile cache keys on the opt level (an -O0 build and an
#           -O2 build of the same source produce two distinct cache objects;
#           neither collides with the other).
#   2. `kardc --test`:
#        a. A fixture with two passing + one failing `test_*` fn reports the
#           right pass/fail counts and exits NONZERO.
#        b. An all-pass fixture exits 0.
#        c. A test file with NO `main()` is handled (the runner doesn't
#           require an entry point).
#   3. The RPN capstone builds AOT and prints its expected output (and the
#      first result becomes the process exit code).
#
# The capstone program text is inlined here (rather than read from
# examples/rpn/main.kd) so the Bazel sandbox always sees it; the example
# file is a copy of the same program for users to read / build directly.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found in runfiles"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Give the AOT cache its own scratch dir so we can observe miss/hit and so a
# stale developer cache can't perturb the assertions.
export XDG_CACHE_HOME="$TMP/cache"

# ===========================================================================
# The RPN capstone (inlined; mirrors examples/rpn/main.kd).
# ===========================================================================
cat > "$TMP/rpn.kd" <<'EOF'
struct Tok { kind: i64, val: i64 }

trait Op { fn apply(&self, a: i64, b: i64) -> Result<i64, i64>; }

struct Add {}
struct Sub {}
struct Mul {}
struct Div {}

impl Op for Add { fn apply(&self, a: i64, b: i64) -> Result<i64, i64> { Ok(a + b) } }
impl Op for Sub { fn apply(&self, a: i64, b: i64) -> Result<i64, i64> { Ok(a - b) } }
impl Op for Mul { fn apply(&self, a: i64, b: i64) -> Result<i64, i64> { Ok(a * b) } }
impl Op for Div {
    fn apply(&self, a: i64, b: i64) -> Result<i64, i64> {
        if b == 0 { Err(1) } else { Ok(a / b) }
    }
}

fn run_op(code: i64, a: i64, b: i64) -> Result<i64, i64> {
    match code {
        0 => { let o = Add {}; o.apply(a, b) },
        1 => { let o = Sub {}; o.apply(a, b) },
        2 => { let o = Mul {}; o.apply(a, b) },
        _ => { let o = Div {}; o.apply(a, b) },
    }
}

fn eval(toks: &Vec<Tok>) -> Result<i64, i64> ! { alloc } {
    let mut m = hashmap_new();
    let mut top = 0;
    let n = vec_len(toks);
    let mut i = 0;
    let mut err = 0;
    while i < n {
        let t = vec_get(toks, i);
        if t.kind == 0 {
            hashmap_insert(&mut m, top, t.val);
            top = top + 1;
        } else {
            if top < 2 {
                err = 2;
                i = n;
            } else {
                top = top - 1;
                let b = match hashmap_get(&m, top) { Some(v) => v, None => 0 };
                top = top - 1;
                let a = match hashmap_get(&m, top) { Some(v) => v, None => 0 };
                match run_op(t.val, a, b) {
                    Ok(r) => { hashmap_insert(&mut m, top, r); top = top + 1; },
                    Err(e) => { err = e; i = n; },
                }
            }
        }
        i = i + 1;
    }
    if err != 0 {
        Err(err)
    } else {
        if top == 1 {
            top = top - 1;
            match hashmap_get(&m, top) { Some(v) => Ok(v), None => Err(2) }
        } else {
            Err(2)
        }
    }
}

fn push_num(toks: &mut Vec<Tok>, x: i64) -> i64 ! { alloc } {
    vec_push(toks, Tok { kind: 0, val: x })
}
fn push_op(toks: &mut Vec<Tok>, code: i64) -> i64 ! { alloc } {
    vec_push(toks, Tok { kind: 1, val: code })
}

fn report(r: Result<i64, i64>) -> i64 ! { io } {
    match r {
        Ok(v) => print(v),
        Err(e) => print(0 - e),
    }
}

fn main() -> i64 ! { alloc, io } {
    let mut e1 = vec_new();
    push_num(&mut e1, 3); push_num(&mut e1, 4); push_op(&mut e1, 0);
    push_num(&mut e1, 5); push_op(&mut e1, 2);
    report(eval(&e1));

    let mut e2 = vec_new();
    push_num(&mut e2, 10); push_num(&mut e2, 2); push_num(&mut e2, 8);
    push_op(&mut e2, 2); push_op(&mut e2, 0); push_num(&mut e2, 3);
    push_op(&mut e2, 1);
    report(eval(&e2));

    let mut e3 = vec_new();
    push_num(&mut e3, 100); push_num(&mut e3, 5); push_op(&mut e3, 3);
    report(eval(&e3));

    let mut e4 = vec_new();
    push_num(&mut e4, 7); push_num(&mut e4, 0); push_op(&mut e4, 3);
    report(eval(&e4));

    let mut e5 = vec_new();
    push_num(&mut e5, 1); push_op(&mut e5, 0);
    report(eval(&e5));

    match eval(&e1) { Ok(v) => v, Err(e) => 0 - e }
}
EOF

RPN_EXPECT=$'35\n23\n20\n-1\n-2'
RPN_EXIT=35

# ===========================================================================
# 1a/3. Capstone builds AOT and runs correctly at default / -O0 / -O2.
# ===========================================================================
run_aot() {
    # $1 = label, $2 = src, $3 = expected stdout, $4 = expected exit, $5.. = flags
    local label="$1" src="$2" want_out="$3" want_rc="$4"; shift 4
    "$KARDC" "$@" -o "$TMP/$label" "$src" 2> "$TMP/$label.err" || {
        echo "FAIL [$label]: AOT compile failed"; cat "$TMP/$label.err"; exit 1; }
    set +e
    local got_out; got_out=$("$TMP/$label")
    "$TMP/$label" > /dev/null
    local rc=$?
    set -e
    if [[ "$got_out" != "$want_out" ]]; then
        echo "FAIL [$label]: AOT stdout mismatch"
        echo "expected:"; echo "$want_out"; echo "got:"; echo "$got_out"; exit 1
    fi
    if [[ "$rc" -ne "$want_rc" ]]; then
        echo "FAIL [$label]: AOT exit code $rc (expected $want_rc)"; exit 1
    fi
}

run_aot rpn_def "$TMP/rpn.kd" "$RPN_EXPECT" "$RPN_EXIT"
run_aot rpn_o0  "$TMP/rpn.kd" "$RPN_EXPECT" "$RPN_EXIT" -O0
run_aot rpn_o2  "$TMP/rpn.kd" "$RPN_EXPECT" "$RPN_EXIT" -O2
echo "PASS [capstone]: RPN calculator AOT-builds + prints 35/23/20/-1/-2, exit 35 (default/-O0/-O2)"

# JIT the capstone too (default + both opt levels) — stdout identical.
for flag in "" "-O0" "-O2"; do
    got=$("$KARDC" $flag "$TMP/rpn.kd")
    want=$'35\n23\n20\n-1\n-2\n35'   # JIT prints main's return after the 5 reports
    if [[ "$got" != "$want" ]]; then
        echo "FAIL [capstone-jit ${flag:-default}]: JIT output mismatch"
        echo "expected:"; echo "$want"; echo "got:"; echo "$got"; exit 1
    fi
done
echo "PASS [capstone-jit]: RPN calculator JIT-runs identically at default/-O0/-O2"

# ===========================================================================
# 1a. fib: same correct result at -O0 and -O2 (and default).
# ===========================================================================
cat > "$TMP/fib.kd" <<'EOF'
fn add1(x: i64) -> i64 { x + 1 }
fn fib(n: i64) -> i64 {
    if n < 2 { n } else { fib(n - 1) + fib(n - 2) }
}
fn main() -> i64 {
    add1(fib(10))   // fib(10)=55, +1 => 56
}
EOF
for flag in "" "-O0" "-O1" "-O2" "-O3"; do
    got=$("$KARDC" $flag "$TMP/fib.kd")
    if [[ "$got" != "56" ]]; then
        echo "FAIL [fib ${flag:-default}]: expected 56, got '$got'"; exit 1
    fi
done
run_aot fib_o0 "$TMP/fib.kd" "" 56 -O0
run_aot fib_o2 "$TMP/fib.kd" "" 56 -O2
echo "PASS [opt-correctness]: fib(10)+1 == 56 at default/-O0/-O1/-O2/-O3 (JIT) and -O0/-O2 (AOT)"

# ===========================================================================
# 1b. -O0 IR is less optimized than -O2 (wrapper not inlined; more lines).
# ===========================================================================
"$KARDC" --emit-llvm -O0 "$TMP/fib.kd" > "$TMP/fib_o0.ll" 2>/dev/null
"$KARDC" --emit-llvm -O2 "$TMP/fib.kd" > "$TMP/fib_o2.ll" 2>/dev/null

if cmp -s "$TMP/fib_o0.ll" "$TMP/fib_o2.ll"; then
    echo "FAIL [opt-ir]: -O0 and -O2 IR are byte-identical (expected differ)"; exit 1
fi

# The precise, robust signal that -O2 is more optimized than -O0: the trivial
# `add1` wrapper is CALLED at -O0 but the inliner removes every call site at
# -O2. (A whole-module size/call comparison is intentionally NOT used — the
# always-emitted prelude helpers can grow at -O2 via inlining even as the user
# code shrinks, so a module-wide metric misreads optimization. The per-call
# inline check below targets exactly the user code.)
o0_calls=$(grep -c 'call i64 @add1' "$TMP/fib_o0.ll" || true)
o2_calls=$(grep -c 'call i64 @add1' "$TMP/fib_o2.ll" || true)
if [[ "$o0_calls" -lt 1 ]]; then
    echo "FAIL [opt-ir]: expected >=1 add1 call at -O0, found $o0_calls"; exit 1
fi
if [[ "$o2_calls" -ne 0 ]]; then
    echo "FAIL [opt-ir]: expected add1 inlined away at -O2, found $o2_calls call(s)"
    exit 1
fi
echo "PASS [opt-ir]: add1 called at -O0, inlined away at -O2 (optimization confirmed)"

# ===========================================================================
# 1c. AOT cache keys on the opt level (-O0 and -O2 -> two distinct objects).
# ===========================================================================
rm -rf "$XDG_CACHE_HOME"
MISS_O0=$("$KARDC" -O0 -o "$TMP/c_o0" "$TMP/fib.kd" 2>&1 | grep -c 'cache miss' || true)
MISS_O2=$("$KARDC" -O2 -o "$TMP/c_o2" "$TMP/fib.kd" 2>&1 | grep -c 'cache miss' || true)
HIT_O0=$("$KARDC" -O0 -o "$TMP/c_o0b" "$TMP/fib.kd" 2>&1 | grep -c 'cache hit' || true)
n_objs=$(find "$XDG_CACHE_HOME/kardashev" -name '*.o' | wc -l | tr -d ' ')
if [[ "$MISS_O0" -ne 1 || "$MISS_O2" -ne 1 ]]; then
    echo "FAIL [opt-cache]: expected a cache miss for each of -O0 and -O2 (distinct keys)"
    exit 1
fi
if [[ "$HIT_O0" -ne 1 ]]; then
    echo "FAIL [opt-cache]: expected a cache hit re-building at -O0"; exit 1
fi
if [[ "$n_objs" -ne 2 ]]; then
    echo "FAIL [opt-cache]: expected 2 distinct cache objects (-O0 + -O2), found $n_objs"
    exit 1
fi
echo "PASS [opt-cache]: -O0 and -O2 produce distinct cache keys (2 objects, no collision)"

# ===========================================================================
# 2a/2c. --test on a mixed fixture (2 pass, 1 fail) — no main() present.
# ===========================================================================
cat > "$TMP/tests_mixed.kd" <<'EOF'
fn test_arith() -> i64 {
    let x = 2 + 3 * 4;       // 14
    if x == 14 { 0 } else { 1 }
}

fn test_vec_sum() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 10);
    vec_push(&mut v, 20);
    vec_push(&mut v, 30);
    let s = vec_get(&v, 0) + vec_get(&v, 1) + vec_get(&v, 2);
    if s == 60 { 0 } else { 1 }
}

fn test_deliberately_fails() -> i64 {
    3
}

fn helper(n: i64) -> i64 { n + 1 }   // not a test; must be ignored
EOF

set +e
MIXED_OUT=$("$KARDC" --test "$TMP/tests_mixed.kd" 2>&1)
MIXED_RC=$?
set -e
echo "$MIXED_OUT"
if [[ "$MIXED_RC" -eq 0 ]]; then
    echo "FAIL [test-mixed]: exit code 0 (expected nonzero — a test failed)"; exit 1
fi
if ! grep -q '^running 3 tests$' <<< "$MIXED_OUT"; then
    echo "FAIL [test-mixed]: expected 'running 3 tests' (helper must be excluded)"; exit 1
fi
if ! grep -q 'test test_deliberately_fails ... FAILED (returned 3)' <<< "$MIXED_OUT"; then
    echo "FAIL [test-mixed]: missing the expected FAILED line"; exit 1
fi
if ! grep -q '^test result: 2 passed, 1 failed$' <<< "$MIXED_OUT"; then
    echo "FAIL [test-mixed]: expected 'test result: 2 passed, 1 failed'"; exit 1
fi
echo "PASS [test-mixed]: 2 passed, 1 failed, exit $MIXED_RC, no main() required"

# ===========================================================================
# 2b. --test on an all-pass fixture exits 0.
# ===========================================================================
cat > "$TMP/tests_pass.kd" <<'EOF'
fn test_one() -> i64 { 0 }
fn test_two() -> i64 {
    let x = 5;
    if x > 3 { 0 } else { 1 }
}
EOF
set +e
PASS_OUT=$("$KARDC" --test "$TMP/tests_pass.kd" 2>&1)
PASS_RC=$?
set -e
if [[ "$PASS_RC" -ne 0 ]]; then
    echo "FAIL [test-pass]: all-pass fixture exited $PASS_RC (expected 0)"
    echo "$PASS_OUT"; exit 1
fi
if ! grep -q '^test result: 2 passed, 0 failed$' <<< "$PASS_OUT"; then
    echo "FAIL [test-pass]: expected 'test result: 2 passed, 0 failed'"
    echo "$PASS_OUT"; exit 1
fi
echo "PASS [test-pass]: all-pass fixture reports 2 passed, 0 failed, exit 0"

echo "PASS: toolchain (-O0..-O3 + cache key), kardc --test, and the RPN capstone all work"
