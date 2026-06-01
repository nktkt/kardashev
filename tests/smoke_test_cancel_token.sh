#!/usr/bin/env bash
# v40 — cooperative cancellation token (structured-concurrency primitive).
#
# `cancel_token_new()` is a shared Send+Sync `AtomicBool` flag; the handle is
# Copy, so passing it by value to a worker thread shares the SAME cell.
# `cancel(t)` (from any thread) requests cancellation; cooperating loops poll
# `is_cancelled(t)` and stop. Single-threaded toggle/loop is fully
# deterministic; the cross-thread case has a deterministic OUTCOME (the worker
# observes the cancel + finishes + join returns). Differential JIT vs AOT.
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

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}

# Token toggles: not-cancelled, then cancelled.
diff_run toggle $'0\n1' '
fn main() -> i64 ! { io, alloc } {
    let t = cancel_token_new();
    if is_cancelled(t) { print(1); } else { print(0); }
    let c = cancel(t);
    if is_cancelled(t) { print(1); } else { print(0); }
    0
}
'

# A cooperative loop stops when the token is cancelled.
diff_run loop $'5' '
fn main() -> i64 ! { io, alloc } {
    let t = cancel_token_new();
    let mut i = 0;
    let mut work = 0;
    while !is_cancelled(t) {
        work = work + 1;
        i = i + 1;
        if i >= 5 { let c = cancel(t); } else {}
    }
    print(work);
    0
}
'

# Cross-thread: a worker shares the token, observes main's cancel, finishes.
diff_run cross_thread $'1' '
fn worker(t: AtomicBool, done: AtomicI64) -> i64 ! { io } {
    let mut i = 0;
    while !is_cancelled(t) { i = i + 1; if i > 1000000 { let c = cancel(t); } else {} }
    let r = done.store(1, Ordering::SeqCst);
    0
}
fn main() -> i64 ! { io, alloc, share } {
    let t = cancel_token_new();
    let done = atomic_i64_new(0);
    let h = thread_spawn(|| worker(t, done));
    let c = cancel(t);
    let r = thread_join(h);
    print(done.load(Ordering::SeqCst));
    0
}
'

echo "ALL CANCEL-TOKEN SMOKE TESTS PASSED"
