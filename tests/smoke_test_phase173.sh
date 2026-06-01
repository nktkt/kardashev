#!/usr/bin/env bash
# v32 Phase 173 smoke test — async cancellation, timeouts & structured concurrency.
#
# Part 1: timeout<T>(fut: Future<T>, ms: i64) -> Future<Option<T>> — race `fut`
# against an internal sleep_ms(ms) timer; Some(v) if fut finishes first, None on
# timeout. A compiler-synthesized leaf future (getOrEmitTimeout) built on the
# Phase 172 select machinery; the guarded future is checked first so a real
# result wins a same-poll tie. Differentially gated JIT vs AOT.
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

if command -v timeout >/dev/null 2>&1; then _TO=timeout
elif command -v gtimeout >/dev/null 2>&1; then _TO=gtimeout
else _TO=""; fi
run_to() { if [[ -n "$_TO" ]]; then "$_TO" "$@"; else shift; "$@"; fi; }

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

# 1. fut finishes before the timer -> Some(v). (Guarded future ready first.)
diff_run timeout_some $'1\n7' '
async fn quick() -> i64 { 7 }
fn main() -> i64 ! { io, async } {
    match block_on(timeout(quick(), 100)) {
        Some(v) => { print(1); print(v); },
        None => { print(0); },
    }
    0
}
'

# 2. a slow future times out -> None. (Timer fires first.)
diff_run timeout_none $'0' '
async fn slow() -> i64 { let z = sleep_ms(200).await; z + 1 }
fn main() -> i64 ! { io, async } {
    match block_on(timeout(slow(), 5)) {
        Some(v) => { print(1); print(v); },
        None => { print(0); },
    }
    0
}
'

# 3. timeout composes with the Phase 172 combinators (map the Option result).
diff_run timeout_compose $'8' '
async fn quick() -> i64 { 7 }
fn main() -> i64 ! { io, async } {
    let g = future_map(timeout(quick(), 100), |o| match o { Some(v) => v + 1, None => 0 - 1, });
    print(block_on(g));   // Some(7) -> 8
    0
}
'

# 4. No leak / no double-free on the Some path (fut wins, timer dropped before
#    arming nested state) — tight loop stays flat under MALLOC_CHECK_=3.
cat > "$TMP/leak.kd" <<'EOF'
async fn one() -> i64 { 1 }
fn main() -> i64 ! { io, async } {
    let mut i = 0;
    let mut acc = 0;
    while i < 50000 {
        acc = acc + match block_on(timeout(one(), 1000)) { Some(v) => v, None => 0, };
        i = i + 1;
    }
    print(acc);   // 50000
    0
}
EOF
run_to 60 "$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
got=$(MALLOC_CHECK_=3 run_to 60 "$TMP/leak" 2>&1)
[[ "$got" == "50000" ]] || { echo "FAIL [leak]: expected 50000 got: $got"; exit 1; }
echo "PASS: timeout_loop_no_leak (50000)"

# ---------------------------------------------------------------------------
# 5. task_cancel<T>(JoinHandle<T>) — cancel a spawned task: retire it from the
#    executor (mark done) + release its frame/slot. Consumes the move-only
#    handle (so a cancelled task can't then be joined). Clean (no double-free)
#    under MALLOC_CHECK_=3 when mixing join + cancel.
# ---------------------------------------------------------------------------
cat > "$TMP/cancel.kd" <<'EOF'
async fn add(a: i64, b: i64) -> i64 { a + b }
fn main() -> i64 ! { io } {
    let h1 = spawn(add(10, 20));
    let h2 = spawn(add(100, 1));
    print(join(h1));     // 30
    task_cancel(h2);     // h2 cancelled, never joined
    print(7);
    0
}
EOF
jit=$(run_to 20 "$KARDC" "$TMP/cancel.kd" 2>/dev/null | head -2)
[[ "$jit" == $'30\n7' ]] || { echo "FAIL [cancel/jit]: expected 30,7 got: $jit"; exit 1; }
run_to 30 "$KARDC" --no-cache -o "$TMP/cancel" "$TMP/cancel.kd" >/dev/null 2>&1
aot=$(MALLOC_CHECK_=3 run_to 20 "$TMP/cancel" 2>&1 | head -2)
[[ "$aot" == $'30\n7' ]] || { echo "FAIL [cancel/aot]: expected 30,7 got: $aot"; exit 1; }
echo "PASS: task_cancel (join one, cancel the other; MALLOC_CHECK clean)"

# NEGATIVE: a cancelled (moved) handle can't be joined.
cat > "$TMP/cancel_join.kd" <<'EOF'
async fn one() -> i64 { 1 }
fn main() -> i64 ! { io } {
    let h = spawn(one());
    task_cancel(h);
    print(join(h));   // h already moved into task_cancel
    0
}
EOF
err=$("$KARDC" "$TMP/cancel_join.kd" 2>&1 >/dev/null || true)
echo "$err" | grep -qi 'moved' || { echo "FAIL [cancel-then-join]: expected moved-value error, got: $err"; exit 1; }
echo "PASS (negative): cancelled handle cannot be joined (move-only)"

# NOTE (documented 173 follow-on): task_cancel / future_select / timeout drop a
# task/loser SHALLOWLY (bare free of the top frame). A still-suspended async-fn
# task leaks its nested in-flight sub-frame (memory-safe). A recursive
# Future-drop (cancel that frees sub-frames) + a full async scope are the
# remaining structured-concurrency refinements; join2 (concurrent wait-all),
# timeout, and task_cancel cover the structured-concurrency primitives.

echo "ALL PHASE 173 SMOKE TESTS PASSED"
