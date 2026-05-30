#!/usr/bin/env bash
# Phase 76 smoke test (Roadmap v13 — concurrency): typed MPSC CHANNELS over a
# pthread mutex + condition variable. channel() -> (Sender<i64>, Receiver<i64>);
# chan_send / chan_recv (-> Option, None once closed+drained) / chan_close.
#   1. Single-threaded send/recv/close drains correctly.
#   2. A PRODUCER THREAD sends 1..=100 then closes; the main thread blocks on
#      chan_recv (condvar) summing until None -> EXACTLY 5050, DETERMINISTIC
#      over many runs (JIT + AOT). The Sender (a Copy, Send i64 handle) crosses
#      into the producer thread by value.
#   3. The Receiver is the single-consumer endpoint and is NOT Send: moving it
#      into a thread is a compile error.
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

# 1. single-threaded send/recv/close.
cat > "$TMP/one.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(tx, 10);
    chan_send(tx, 20);
    chan_send(tx, 12);
    chan_close(tx);
    let mut sum = 0;
    let mut go = true;
    while go {
        match chan_recv(rx) { Some(v) => { sum = sum + v; }, None => { go = false; }, }
    }
    sum   // 42
}
EOF
jit=$("$KARDC" "$TMP/one.kd" 2>/dev/null | tail -1)
[[ "$jit" == "42" ]] || { echo "FAIL [one/jit]: expected 42 got '$jit'"; exit 1; }
echo "PASS [single-thread]: send/recv/close drains -> 42"

# 2. producer thread + blocking recv: deterministic 5050, JIT (many runs) + AOT.
cat > "$TMP/prod.kd" <<'EOF'
fn produce(tx: Sender<i64>) -> i64 ! { share } {
    let mut i = 1;
    while i <= 100 { chan_send(tx, i); i = i + 1; }
    chan_close(tx);
    0
}
fn main() -> i64 ! { io, alloc, share } {
    let (tx, rx) = channel();
    let h = thread_spawn(|| produce(tx));
    let mut sum = 0;
    let mut go = true;
    while go {
        match chan_recv(rx) { Some(v) => { sum = sum + v; }, None => { go = false; }, }
    }
    thread_join(h);
    sum   // 5050
}
EOF
for run in 1 2 3 4 5; do
    jit=$("$KARDC" "$TMP/prod.kd" 2>/dev/null | tail -1)
    [[ "$jit" == "5050" ]] || { echo "FAIL [prod/jit run $run]: expected 5050 got '$jit'"; exit 1; }
done
"$KARDC" --no-cache -o "$TMP/prod" "$TMP/prod.kd" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; "$TMP/prod" >/dev/null; rc=$?; set -e
    [[ "$rc" -eq $((5050 & 255)) ]] || { echo "FAIL [prod/aot run $run]: exit $rc expected $((5050 & 255))"; exit 1; }
done
echo "PASS [producer-thread]: blocking recv sums 1..100 -> 5050, deterministic (5 JIT + 3 AOT)"

# 3. moving a Receiver into a thread is rejected (single-consumer / not Send).
cat > "$TMP/neg.kd" <<'EOF'
fn consume(rx: Receiver<i64>) -> i64 ! { share } {
    match chan_recv(rx) { Some(v) => v, None => 0 }
}
fn main() -> i64 ! { io, alloc, share } {
    let (tx, rx) = channel();
    chan_send(tx, 1);
    let h = thread_spawn(|| consume(rx));
    thread_join(h)
}
EOF
set +e; out=$("$KARDC" "$TMP/neg.kd" 2>&1); set -e
if "$KARDC" "$TMP/neg.kd" >/dev/null 2>&1; then echo "FAIL [recv-not-send]: compiled"; exit 1; fi
grep -qi "Receiver" <<<"$out" && grep -qi "not Send\|single-consumer" <<<"$out" || { echo "FAIL [recv-not-send]: wrong message: $out"; exit 1; }
echo "PASS [receiver-not-send]: moving a Receiver into a thread is rejected"

echo "ALL PHASE 76 SMOKE TESTS PASSED"
