#!/usr/bin/env bash
# Phase 79 smoke test (Roadmap v13 — concurrency): the PARALLELISM PAYOFF —
# fork-join parallel computation built from the v13 primitives, plus
# chan_try_recv (a non-blocking receive).
#   1. FORK-JOIN: split 0..1000 across 4 worker threads; each sums its range and
#      sends the partial on a SHARED Sender (multi-producer); main gathers the
#      4 partials (MPSC: 4 producers -> 1 consumer) and folds -> EXACTLY 499500,
#      DETERMINISTIC across runs, JIT + AOT. Real parallel work, gathered safely.
#   2. chan_try_recv drains a channel without blocking (None when momentarily
#      empty) — for poll loops.
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

# 1. fork-join parallel sum.
cat > "$TMP/pj.kd" <<'EOF'
fn worker(lo: i64, hi: i64, tx: Sender<i64>) -> i64 ! { share } {
    let mut s = 0; let mut i = lo;
    while i < hi { s = s + i; i = i + 1; }
    chan_send(tx, s);
    0
}
fn main() -> i64 ! { io, alloc, share } {
    let n = 1000; let w = 4;
    let (tx, rx) = channel();
    let chunk = n / w;
    let mut handles = vec_new();
    let mut k = 0;
    while k < w {
        let lo = k * chunk;
        let hi = if k == w - 1 { n } else { (k + 1) * chunk };
        let h = thread_spawn(|| worker(lo, hi, tx));   // shared Sender, Copy work
        vec_push(&mut handles, h);
        k = k + 1;
    }
    let mut total = 0; let mut got = 0;
    while got < w {
        match chan_recv(rx) { Some(p) => { total = total + p; got = got + 1; }, None => {}, }
    }
    let mut j = 0;
    while j < w { thread_join(vec_get(&handles, j)); j = j + 1; }
    total   // 0 + 1 + ... + 999 = 499500
}
EOF
for run in 1 2 3 4 5; do
    jit=$("$KARDC" "$TMP/pj.kd" 2>/dev/null | tail -1)
    [[ "$jit" == "499500" ]] || { echo "FAIL [pj/jit run $run]: expected 499500 got '$jit'"; exit 1; }
done
"$KARDC" --no-cache -o "$TMP/pj" "$TMP/pj.kd" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; "$TMP/pj" >/dev/null; rc=$?; set -e
    [[ "$rc" -eq $((499500 & 255)) ]] || { echo "FAIL [pj/aot run $run]: exit $rc expected $((499500 & 255))"; exit 1; }
done
echo "PASS [fork-join]: 4 workers sum 0..1000 -> 499500, deterministic (5 JIT + 3 AOT)"

# 2. chan_try_recv non-blocking drain.
cat > "$TMP/tr.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(tx, 5); chan_send(tx, 9); chan_send(tx, 14);
    let mut sum = 0;
    let mut go = true;
    while go {
        match chan_try_recv(rx) { Some(v) => { sum = sum + v; }, None => { go = false; }, }
    }
    sum   // 28, then None (empty) stops the loop without blocking
}
EOF
jit=$("$KARDC" "$TMP/tr.kd" 2>/dev/null | tail -1)
[[ "$jit" == "28" ]] || { echo "FAIL [try_recv]: expected 28 got '$jit'"; exit 1; }
echo "PASS [chan_try_recv]: non-blocking drain -> 28, None when empty"

echo "ALL PHASE 79 SMOKE TESTS PASSED"
