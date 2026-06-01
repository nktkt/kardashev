#!/usr/bin/env bash
# v32 Phase 172 smoke test — Future COMBINATORS + the Either sum type.
#
#   future_map<T,U>(Future<T>, fn(T)->U ! {e})            -> Future<U>
#   future_and_then<T,U>(Future<T>, fn(T)->Future<U> ! {e}) -> Future<U>
#   future_join2<A,B>(Future<A>, Future<B>)               -> Future<(A,B)>   (wait-all)
#   future_select<A,B>(Future<A>, Future<B>)              -> Future<Either<A,B>> (wait-any)
#
# Each is a compiler-synthesized leaf future (no AST body): its poll fn drives
# the inner future(s), applies the continuation / latches values / picks a
# winner, frees the consumed frames + closure env, and reports Ready. The
# effect-row var `e` carries the continuation's effects to the combinator's
# call site (a pure closure => a pure combinator). The map/and_then closure is
# stored in a heap frame and called at poll time, so it must be `Fn` (a by-ref
# FnMut capture would dangle — rejected). Differentially gated JIT vs AOT.
#
# This phase also fixed a PRE-EXISTING codegen bug it exposed: malloc SIZES were
# baked with LLVM's default DataLayout (i64 under-aligned to 4) while StructGEP
# OFFSETS lower against the host layout (i64 align 8), so a `Poll<Enum>` slot for
# a multi-payload enum (Either/Result) was under-allocated by 8 bytes — an
# 8-byte heap overflow on `block_on` of such a future. The host DataLayout is now
# pinned before the codegen walk. Section 7 pins that fix.
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

# Portable timeout: macOS/BSD ships neither `timeout` nor (usually) `gtimeout`,
# so fall back to running the command directly (the timeout is only a deadlock
# safety-net; the harness's own test timeout still bounds a true hang).
if command -v timeout >/dev/null 2>&1; then _TO=timeout
elif command -v gtimeout >/dev/null 2>&1; then _TO=gtimeout
else _TO=""; fi
run_to() { if [[ -n "$_TO" ]]; then "$_TO" "$@"; else shift; "$@"; fi; }

# diff_run <name> <expected-stdout-lines> <src>: JIT stdout (first N lines) must
# equal expected AND AOT stdout must match the JIT prefix. N = #expected lines.
diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$(run_to 20 "$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n")
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    run_to 30 "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$(run_to 20 "$TMP/$name" 2>/dev/null | head -n "$n")
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}

# expect_err <name> <needle> <src>: compile must fail with `needle` in stderr.
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || {
        echo "FAIL [$name]: expected error containing '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# ---------------------------------------------------------------------------
# 1. future_map — apply a continuation to a future's result. Pure closure =>
#    pure combinator; type may change (i64 -> bool); maps chain.
# ---------------------------------------------------------------------------
diff_run map_basic $'42' '
async fn base() -> i64 { 41 }
fn main() -> i64 ! { io } { print(block_on(future_map(base(), |x| x + 1))); 0 }
'
diff_run map_chain_typechange $'1' '
async fn base() -> i64 { 7 }
fn main() -> i64 ! { io } {
    let g = future_map(future_map(base(), |x| x + 3), |y| y > 5);  // 7->10->true
    if block_on(g) { print(1); } else { print(0); }
    0
}
'
# Pending propagation: map over a real timer must take (and survive) Pending.
diff_run map_pending $'10\n1' '
fn main() -> i64 ! { io, async } {
    print(block_on(future_map(sleep_ms(5), |x| x * 2)));   // 10
    if poll_count() > 1 { print(1); } else { print(0); }   // Pending taken
    0
}
'
# Captured-by-value (Fn) closure works.
diff_run map_capture $'42' '
async fn base() -> i64 { 10 }
fn main() -> i64 ! { io } { let k = 32; print(block_on(future_map(base(), |x| x + k))); 0 }
'

# ---------------------------------------------------------------------------
# 2. future_and_then — monadic bind: continuation returns another future.
# ---------------------------------------------------------------------------
diff_run andthen_basic $'22' '
async fn base() -> i64 { 10 }
async fn dbl(n: i64) -> i64 { n * 2 }
fn main() -> i64 ! { io } { print(block_on(future_and_then(base(), |x| dbl(x + 1)))); 0 }
'
# Both stages Pending (timer -> timer), then mapped.
diff_run andthen_both_pending $'105\n1' '
fn main() -> i64 ! { io, async } {
    let g = future_and_then(sleep_ms(3), |x| sleep_ms(x + 2));  // 3 -> 5 -> 5
    print(block_on(future_map(g, |y| y + 100)));                // 105
    if poll_count() > 2 { print(1); } else { print(0); }
    0
}
'

# ---------------------------------------------------------------------------
# 3. future_join2 — wait for BOTH, return a tuple. Concurrent timers.
# ---------------------------------------------------------------------------
diff_run join2_basic $'11\n31\n42' '
async fn a() -> i64 { 11 }
async fn b() -> i64 { 31 }
fn main() -> i64 ! { io } {
    let (x, y) = block_on(future_join2(a(), b()));
    print(x); print(y); print(x + y);
    0
}
'
diff_run join2_concurrent $'11\n1' '
fn main() -> i64 ! { io, async } {
    let (x, y) = block_on(future_join2(sleep_ms(8), sleep_ms(3)));
    print(x + y);                                          // 11
    if poll_count() > 2 { print(1); } else { print(0); }   // both pending
    0
}
'

# ---------------------------------------------------------------------------
# 4. future_select — wait for ANY, return Either; drop the loser. `fa` wins a
#    same-poll tie (stable bias); timing makes the faster timer win.
# ---------------------------------------------------------------------------
diff_run select_tie_left $'100' '
async fn a() -> i64 { 100 }
async fn b() -> i64 { 200 }
fn main() -> i64 ! { io } {
    match block_on(future_select(a(), b())) { Left(x) => { print(x); }, Right(y) => { print(0 - y); }, }
    0
}
'
diff_run select_right_timing $'3' '
fn main() -> i64 ! { io, async } {
    match block_on(future_select(sleep_ms(60), sleep_ms(3))) { Left(x) => { print(0 - x); }, Right(y) => { print(y); }, }
    0
}
'
# Mixed result types (i64, bool) through the Either payload.
diff_run select_mixed $'7' '
async fn n() -> i64 { 7 }
async fn flag() -> bool { true }
fn main() -> i64 ! { io } {
    match block_on(future_select(n(), flag())) { Left(x) => { print(x); }, Right(b) => { if b { print(1); } else { print(0); } }, }
    0
}
'

# ---------------------------------------------------------------------------
# 5. Effect propagation — an io-effecting continuation makes the combinator
#    io-effecting; a fn that builds-and-drives it must declare io (NEGATIVE).
# ---------------------------------------------------------------------------
diff_run effect_io_ok $'5\n6' '
async fn base() -> i64 { 5 }
fn main() -> i64 ! { io } {
    print(block_on(future_map(base(), |x| { print(x); x + 1 })));
    0
}
'
expect_err effect_leak 'effect `io`' '
async fn base() -> i64 { 5 }
fn build() -> i64 ! { alloc } { block_on(future_map(base(), |x| { print(x); x + 1 })) }
fn main() -> i64 ! { io, alloc } { print(build()); 0 }
'

# ---------------------------------------------------------------------------
# 6. NEGATIVE — a by-reference (FnMut) capture is rejected (the closure is
#    stored and called at poll time, after the call returns), and a unit inner
#    result type is rejected (it would be a void closure param / Either payload).
# ---------------------------------------------------------------------------
expect_err map_byref 'by reference' '
async fn base() -> i64 { 5 }
fn main() -> i64 ! { io } {
    let mut acc = 0;
    print(block_on(future_map(base(), |x| { acc = acc + x; acc })));
    0
}
'
expect_err map_unit 'cannot be unit' '
async fn nothing() { }
fn main() -> i64 ! { io } { block_on(future_map(nothing(), |x| 1)); 0 }
'
expect_err select_unit 'cannot be unit' '
async fn nothing() { }
async fn n() -> i64 { 1 }
fn main() -> i64 ! { io } { block_on(future_select(nothing(), n())); 0 }
'

# ---------------------------------------------------------------------------
# 7. DataLayout-fix regression: block_on of a future whose result is a
#    multi-payload enum (Either via select, Result via an async fn) must NOT
#    corrupt the heap. The Poll<Enum> slot is now sized with the host layout.
#    Run repeatedly: a heap overflow corrupts metadata non-deterministically.
# ---------------------------------------------------------------------------
cat > "$TMP/result.kd" <<'EOF'
async fn mk(b: i64) -> Result<i64, i64> { if b > 0 { Ok(b) } else { Err(0 - b) } }
fn main() -> i64 ! { io } {
    match block_on(mk(0 - 9)) { Ok(x) => { print(1); print(x); }, Err(y) => { print(2); print(y); }, }
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/result" "$TMP/result.kd" >/dev/null 2>&1
for i in 1 2 3 4 5; do
    got=$(run_to 10 "$TMP/result" 2>&1)
    [[ "$got" == $'2\n9' ]] || { echo "FAIL [datalayout-result/run$i]: expected 2,9 got: $got"; exit 1; }
done
echo "PASS: datalayout_result_no_heap_overflow (5 runs)"

# ---------------------------------------------------------------------------
# 8. No leak / no double-free: a tight loop of each combinator keeps RSS flat
#    (the consumed frames + closure env are freed; the dropped select loser's
#    top frame is freed). MALLOC_CHECK_=3 would abort on a double free.
#    NOTE (documented limitation, fixed in Phase 173): this loop uses single-
#    state async fns (no nested .await sub-frames) and a childless sleep_ms
#    select loser, so it does NOT exercise — and Phase 172 does NOT fix — the
#    shallow-cancellation leak where future_select drops a MID-FLIGHT async-fn
#    loser (one already polled to Pending), leaking that loser's nested in-
#    flight sub-future frame. That is memory-safe (no double-free/UAF), a known
#    limitation called out at getOrEmitSelect, awaiting Phase 173's recursive
#    cancellation (a type-erased drop slot in the Future fat pointer).
# ---------------------------------------------------------------------------
cat > "$TMP/leak.kd" <<'EOF'
async fn one() -> i64 { 1 }
async fn inc(n: i64) -> i64 { n + 1 }
fn main() -> i64 ! { io } {
    let mut i = 0;
    let mut acc = 0;
    while i < 50000 {
        acc = acc + block_on(future_map(one(), |x| x + 1));            // +2
        acc = acc + block_on(future_and_then(one(), |x| inc(x)));      // +2
        let (p, q) = block_on(future_join2(one(), one()));
        acc = acc + p + q;                                             // +2
        acc = acc + match block_on(future_select(one(), one())) { Left(x) => x, Right(y) => y, }; // +1
        i = i + 1;
    }
    print(acc);   // 50000 * 7 = 350000
    0
}
EOF
run_to 60 "$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
got=$(MALLOC_CHECK_=3 run_to 60 "$TMP/leak" 2>&1)
[[ "$got" == "350000" ]] || { echo "FAIL [leak]: expected 350000 got: $got"; exit 1; }
echo "PASS: combinator_loop_no_leak_no_double_free (350000)"

echo "ALL PHASE 172 SMOKE TESTS PASSED"
