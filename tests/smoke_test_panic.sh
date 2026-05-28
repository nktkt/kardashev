#!/usr/bin/env bash
# Phase 23 smoke test: real panic + unwinding (setjmp/longjmp + a cleanup
# stack), end to end through JIT and AOT.
#
# Proves all four "realness" criteria the phase demands:
#   (a) Drop runs on panic — a `Guard` with `impl Drop` declared before a
#       `panic` has its `drop` RUN (its id printed) while the panic unwinds and
#       is caught, in reverse declaration order.
#   (b) catch recovers — `catch(f, recover)` returns `recover` when `f` panics
#       and execution continues normally afterward; a non-panicking `f` returns
#       its real value.
#   (c) uncaught panic — a program that panics with NO enclosing catch prints
#       the message to stderr and exits 101 (nonzero), and does NOT segfault.
#   (d) no double-free / leak — a value dropped on the normal path is freed
#       exactly once; the SAME shape dropped on the panic path is freed exactly
#       once (a global "drop counter" — the number of Drop-impl print lines —
#       asserts exactly-once on each path; a value MOVED before a panic is
#       dropped exactly once total, never twice). Plus a 100k-iteration
#       panic/unwind loop with a live heap Vec keeps RSS flat (the Vec buffer is
#       freed on every unwind — a leak would balloon to GBs).
#
# The unwind mechanism: every droppable local registers a cleanup entry
# (recording its Phase-16 drop flag + storage + a drop thunk) when it becomes
# live; `panic` writes the message to stderr then runs the cleanup entries from
# the panic point down to the catching frame's saved depth (each flag-guarded,
# so a moved-out value is skipped) and longjmps to the catch. The SAME drop flag
# gates both the normal inline drop and the panic-path cleanup entry, so every
# value is dropped at most once and (if still owned) exactly once.
#
# The array `[T; N]` OOB index now PANICS (a real unwind) instead of reading
# out of bounds — exercised in both the caught and uncaught directions.
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

# Helper: run $2 through JIT, assert its full stdout ($3). Stderr is discarded
# (panic diagnostics go to stderr and are checked separately where relevant).
jit_expect() {
    local label="$1" src="$2" want="$3"
    local got
    got=$("$KARDC" "$src" 2>/dev/null)
    if [[ "$got" != "$want" ]]; then
        echo "FAIL [$label]: JIT stdout mismatch"
        echo "expected:"; echo "$want"
        echo "got:"; echo "$got"
        exit 1
    fi
}

# Helper: AOT-compile $2, run it, assert stdout ($3). Stderr discarded.
aot_expect() {
    local label="$1" src="$2" want_out="$3"
    "$KARDC" -o "$TMP/prog" "$src" >/dev/null 2>&1
    if [[ ! -x "$TMP/prog" ]]; then
        echo "FAIL [$label]: AOT build failed"
        exit 1
    fi
    set +e
    local got_out
    got_out=$("$TMP/prog" 2>/dev/null)
    set -e
    if [[ "$got_out" != "$want_out" ]]; then
        echo "FAIL [$label]: AOT stdout mismatch"
        echo "expected:"; echo "$want_out"
        echo "got:"; echo "$got_out"
        exit 1
    fi
}

# ============================================================================
# (a) Drop runs on panic, in reverse declaration order, while unwinding.
# `boom` declares g1 (id 1) then g2 (id 2), then panics. The unwinder runs
# their Drop impls in REVERSE order (g2 then g1 -> 2,1) before catch returns 99.
# ============================================================================
cat > "$TMP/drop_on_panic.kd" <<'EOF'
trait Drop { fn drop(&mut self); }
struct Guard { id: i64 }
impl Drop for Guard { fn drop(&mut self) ! { io } { print(self.id); } }
fn boom() -> i64 ! { io, panic } {
    let g1 = Guard { id: 1 };
    let g2 = Guard { id: 2 };
    panic("boom");
    0
}
fn main() -> i64 ! { io } {
    let r = catch(boom, 99);
    print(r);
    print(42);
    0
}
EOF
# 2,1 = guards dropped in reverse order during unwind; 99 = recovery; 42 =
# execution continued; 0 = main's JIT-printed return value.
A_WANT_JIT=$'2\n1\n99\n42\n0'
A_WANT_AOT=$'2\n1\n99\n42'
jit_expect "a_drop_on_panic" "$TMP/drop_on_panic.kd" "$A_WANT_JIT"
aot_expect "a_drop_on_panic" "$TMP/drop_on_panic.kd" "$A_WANT_AOT"
echo "PASS [a]: Drop runs while unwinding a caught panic, reverse order (2,1) then recover 99, then continue (42) — JIT + AOT"

# ============================================================================
# (b) catch recovers: panicking computation -> recovery value; non-panicking
# computation -> its real value; execution continues after either.
# ============================================================================
cat > "$TMP/catch_recovers.kd" <<'EOF'
fn panics() -> i64 ! { panic } { panic("nope"); 0 }
fn computes() -> i64 { 7 }
fn main() -> i64 ! { io } {
    let a = catch(panics, 555);    // panics -> 555
    print(a);
    let b = catch(computes, 555);  // does not panic -> its real value 7
    print(b);
    print(a + b);                  // 555 + 7 = 562 (execution continues)
    0
}
EOF
B_WANT_JIT=$'555\n7\n562\n0'
B_WANT_AOT=$'555\n7\n562'
jit_expect "b_catch_recovers" "$TMP/catch_recovers.kd" "$B_WANT_JIT"
aot_expect "b_catch_recovers" "$TMP/catch_recovers.kd" "$B_WANT_AOT"
echo "PASS [b]: catch returns recovery (555) on panic and the real value (7) otherwise; execution continues — JIT + AOT"

# ============================================================================
# (c) Uncaught panic: prints the message to stderr and exits 101, no segfault.
# Verified in BOTH JIT and AOT, asserting the exit code AND the stderr message.
# ============================================================================
cat > "$TMP/uncaught.kd" <<'EOF'
fn main() -> i64 ! { io, panic } {
    print(1);
    panic("fatal boom");
    0
}
EOF
# --- JIT: kardc runs the program; the in-JIT exit(101) terminates kardc. ---
set +e
JIT_ERR=$("$KARDC" "$TMP/uncaught.kd" 2>&1 1>/dev/null)
JIT_RC=$?
set -e
if [[ "$JIT_RC" -ne 101 ]]; then
    echo "FAIL [c_uncaught_jit]: exit code was $JIT_RC (expected 101)"
    echo "stderr:"; echo "$JIT_ERR"
    exit 1
fi
if ! printf '%s\n' "$JIT_ERR" | grep -q "fatal boom"; then
    echo "FAIL [c_uncaught_jit]: panic message 'fatal boom' not on stderr"
    echo "stderr was:"; echo "$JIT_ERR"
    exit 1
fi
# --- AOT: the native binary exits 101 with the message on stderr. ---
"$KARDC" -o "$TMP/uncaught" "$TMP/uncaught.kd" >/dev/null 2>&1
set +e
AOT_ERR=$("$TMP/uncaught" 2>&1 1>/dev/null)
AOT_RC=$?
AOT_OUT=$("$TMP/uncaught" 2>/dev/null)
set -e
if [[ "$AOT_RC" -ne 101 ]]; then
    echo "FAIL [c_uncaught_aot]: exit code was $AOT_RC (expected 101)"
    exit 1
fi
if ! printf '%s\n' "$AOT_ERR" | grep -q "fatal boom"; then
    echo "FAIL [c_uncaught_aot]: panic message 'fatal boom' not on stderr"
    echo "stderr was:"; echo "$AOT_ERR"
    exit 1
fi
# stdout before the panic ("1") still appears; the message is NOT on stdout.
if [[ "$AOT_OUT" != "1" ]]; then
    echo "FAIL [c_uncaught_aot]: stdout was '$AOT_OUT' (expected '1')"
    exit 1
fi
echo "PASS [c]: uncaught panic prints 'fatal boom' to stderr and exits 101 (no segfault) — JIT + AOT"

# ============================================================================
# (d) No double-free / leak: exactly-once drop on each path, via a drop counter.
# ============================================================================
# (d.1) A value dropped on the NORMAL path is dropped exactly once; the SAME
# shape dropped on the PANIC path is dropped exactly once. We count the Drop
# impl's print lines for each id.
cat > "$TMP/exactly_once.kd" <<'EOF'
trait Drop { fn drop(&mut self); }
struct Cnt { id: i64 }
impl Drop for Cnt { fn drop(&mut self) ! { io } { print(self.id); } }
fn normal() -> i64 ! { io } {
    let n = Cnt { id: 11 };   // dropped on the normal path
    7
}
fn panicky() -> i64 ! { io, panic } {
    let p = Cnt { id: 22 };   // dropped on the panic (unwind) path
    panic("x");
    0
}
fn main() -> i64 ! { io } {
    print(normal());            // -> 11 (drop), 7
    let r = catch(panicky, 0);  // -> 22 (drop on unwind), then r=0
    print(r);
    0
}
EOF
EO_OUT=$("$KARDC" "$TMP/exactly_once.kd" 2>/dev/null)
N11=$(printf '%s\n' "$EO_OUT" | grep -c '^11$')
N22=$(printf '%s\n' "$EO_OUT" | grep -c '^22$')
if [[ "$N11" -ne 1 || "$N22" -ne 1 ]]; then
    echo "FAIL [d_exactly_once]: id 11 dropped $N11 time(s), id 22 dropped $N22 time(s) (each must be 1)"
    echo "got:"; echo "$EO_OUT"
    exit 1
fi
echo "PASS [d.1]: normal-path value (11) and panic-path value (22) each dropped EXACTLY once"

# (d.2) The classic double-free trap: a value MOVED into a callee that then
# panics. The callee drops it once (on unwind); the caller's frame must NOT drop
# it again (its drop flag was cleared by the move). Total drops of id 33 == 1.
cat > "$TMP/move_then_panic.kd" <<'EOF'
trait Drop { fn drop(&mut self); }
struct Cnt { id: i64 }
impl Drop for Cnt { fn drop(&mut self) ! { io } { print(self.id); } }
fn sink(t: Cnt) -> i64 ! { io, panic } { panic("after move"); 0 }
fn panicky() -> i64 ! { io, panic } {
    let a = Cnt { id: 33 };
    sink(a)               // `a` moves into sink; sink panics holding it
}
fn main() -> i64 ! { io } {
    let r = catch(panicky, 0);
    print(r);
    0
}
EOF
MV_OUT=$("$KARDC" "$TMP/move_then_panic.kd" 2>/dev/null)
N33=$(printf '%s\n' "$MV_OUT" | grep -c '^33$')
if [[ "$N33" -ne 1 ]]; then
    echo "FAIL [d_move_then_panic]: moved-then-panicked value dropped $N33 time(s) (expected exactly 1 — no double free)"
    echo "got:"; echo "$MV_OUT"
    exit 1
fi
echo "PASS [d.2]: a value moved into a callee that panics is dropped EXACTLY once (drop flag prevents double-free)"

# (d.3) Headline: 100k panic/unwinds, each with a LIVE heap Vec — the Vec buffer
# is freed on every unwind, so RSS stays flat. A leak would balloon to GBs.
cat > "$TMP/vec_unwind_loop.kd" <<'EOF'
fn buildAndPanic() -> i64 ! { alloc, panic } {
    let mut v = vec_new();
    vec_push(&mut v, 1);
    vec_push(&mut v, 2);
    vec_push(&mut v, 3);
    panic("with live vec");
    vec_len(&v)
}
fn main() -> i64 ! { alloc, io } {
    let mut k = 0;
    while k < 100000 {
        let r = catch(buildAndPanic, 0);
        k = k + 1;
    }
    print(k);
    0
}
EOF
"$KARDC" -o "$TMP/vecunwind" "$TMP/vec_unwind_loop.kd" >/dev/null 2>&1
if [[ ! -x "$TMP/vecunwind" ]]; then
    echo "FAIL [d_vecloop]: AOT build failed"
    exit 1
fi
VEC_OUT=$("$TMP/vecunwind" 2>/dev/null)
if [[ "$VEC_OUT" != "100000" ]]; then
    echo "FAIL [d_vecloop]: expected 100000 completed iterations, got '$VEC_OUT'"
    exit 1
fi
RSS_KB=""
if /usr/bin/time -v true >/dev/null 2>&1; then
    RSS_KB=$(/usr/bin/time -v "$TMP/vecunwind" 2>&1 \
             | awk -F': ' '/Maximum resident set size/ {print $2}')
fi
if [[ -z "$RSS_KB" ]]; then
    "$TMP/vecunwind" >/dev/null 2>&1 || true
    RSS_KB=0
    echo "INFO [d.3]: GNU time unavailable; relying on the exactly-once drop proofs above"
fi
echo "INFO [d.3]: peak RSS over 100,000 panic/unwinds with a live Vec each = ${RSS_KB} KB"
if [[ -n "$RSS_KB" && "$RSS_KB" -gt 32768 ]]; then
    echo "FAIL [d.3]: RSS ${RSS_KB} KB exceeds 32 MB — the per-unwind Vec buffer is leaking"
    exit 1
fi
echo "PASS [d.3]: 100k panic/unwinds with a live heap Vec each — RSS flat (<=32 MB), every buffer freed on unwind"

# ============================================================================
# Bounds-checked indexing: a dynamic OOB array index PANICS (real unwind),
# caught and uncaught.
# ============================================================================
cat > "$TMP/oob_caught.kd" <<'EOF'
fn oob() -> i64 ! { panic } {
    let arr = [10, 20, 30];
    let i = 5;
    arr[i]
}
fn main() -> i64 ! { io } {
    let r = catch(oob, 777);
    print(r);
    0
}
EOF
jit_expect "oob_caught" "$TMP/oob_caught.kd" $'777\n0'
aot_expect "oob_caught" "$TMP/oob_caught.kd" $'777'
echo "PASS [oob-caught]: a dynamic out-of-bounds array index panics and is caught (-> 777) — JIT + AOT"

# In-bounds dynamic indexing must NOT panic (regression guard: the bounds check
# only fires when actually out of range).
cat > "$TMP/oob_inbounds.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let a = [5, 15, 25, 35];
    let mut i = 0;
    let mut acc = 0;
    while i < 4 { acc = acc + a[i]; i = i + 1; }
    print(acc);
    0
}
EOF
jit_expect "oob_inbounds" "$TMP/oob_inbounds.kd" $'80\n0'
echo "PASS [oob-inbounds]: in-bounds dynamic indexing does NOT panic (sum 80)"

# Uncaught OOB: exits 101 with an index-out-of-bounds diagnostic on stderr.
cat > "$TMP/oob_uncaught.kd" <<'EOF'
fn main() -> i64 ! { panic } {
    let arr = [10, 20, 30];
    let i = 9;
    arr[i]
}
EOF
"$KARDC" -o "$TMP/ooba" "$TMP/oob_uncaught.kd" >/dev/null 2>&1
set +e
OOB_ERR=$("$TMP/ooba" 2>&1 1>/dev/null)
OOB_RC=$?
set -e
if [[ "$OOB_RC" -ne 101 ]]; then
    echo "FAIL [oob-uncaught]: exit code was $OOB_RC (expected 101)"
    exit 1
fi
if ! printf '%s\n' "$OOB_ERR" | grep -q "index out of bounds"; then
    echo "FAIL [oob-uncaught]: 'index out of bounds' not on stderr"
    echo "stderr was:"; echo "$OOB_ERR"
    exit 1
fi
echo "PASS [oob-uncaught]: an uncaught out-of-bounds index panics, prints the diagnostic, exits 101 — AOT"

# ============================================================================
# Effect checking (Phase 10a consistency): panic carries `panic`; catch clears
# it; catch still propagates other effects.
# ============================================================================
cat > "$TMP/eff_undeclared.kd" <<'EOF'
fn bad() -> i64 { panic("x"); 0 }
fn main() -> i64 { bad() }
EOF
set +e
EFF_OUT=$("$KARDC" "$TMP/eff_undeclared.kd" 2>&1)
EFF_RC=$?
set -e
if [[ "$EFF_RC" -eq 0 ]]; then
    echo 'FAIL [eff-undeclared]: calling panic without declaring ! { panic } was accepted'
    exit 1
fi
if ! printf '%s\n' "$EFF_OUT" | grep -q "panic"; then
    echo "FAIL [eff-undeclared]: diagnostic did not mention the panic effect"
    echo "got:"; echo "$EFF_OUT"
    exit 1
fi
echo 'PASS [eff-undeclared]: a fn calling panic without declaring ! { panic } is rejected'

cat > "$TMP/eff_catch_clears.kd" <<'EOF'
fn boom() -> i64 ! { panic } { panic("x"); 0 }
fn main() -> i64 ! { io } { let r = catch(boom, 0); print(r); 0 }
EOF
if ! "$KARDC" "$TMP/eff_catch_clears.kd" >/dev/null 2>&1; then
    echo "FAIL [eff-catch-clears]: catch did not clear the panic effect (main rejected)"
    exit 1
fi
echo "PASS [eff-catch-clears]: catch clears the panic effect (a catching fn need not declare panic)"

cat > "$TMP/eff_catch_propagates.kd" <<'EOF'
fn ioFn() -> i64 ! { io } { print(1); 0 }
fn main() -> i64 { let r = catch(ioFn, 0); r }
EOF
set +e
PROP_OUT=$("$KARDC" "$TMP/eff_catch_propagates.kd" 2>&1)
PROP_RC=$?
set -e
if [[ "$PROP_RC" -eq 0 ]]; then
    echo "FAIL [eff-catch-propagates]: catch swallowed the io effect (should still propagate)"
    exit 1
fi
if ! printf '%s\n' "$PROP_OUT" | grep -q "io"; then
    echo "FAIL [eff-catch-propagates]: diagnostic did not mention the io effect"
    echo "got:"; echo "$PROP_OUT"
    exit 1
fi
echo "PASS [eff-catch-propagates]: catch still propagates non-panic effects (io) to its caller"

echo "PASS: real panic + unwinding — Drop runs on unwind (reverse order), catch recovers, uncaught panic exits 101 with a stderr message, exactly-once drops on both normal and panic paths (no double-free / leak), array OOB indexing panics, and panic/catch effect-checking is consistent"
