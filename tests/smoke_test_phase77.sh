#!/usr/bin/env bash
# Phase 77 smoke test (Roadmap v13 — concurrency): the GENERIC channel — a
# channel<T> MOVES a real T across threads (per-T queue node), and the
# structural `Send` rule gets teeth at chan_send.
#   1. A channel<String> moves owned Strings from a producer thread to main
#      (ownership transfers sender -> node -> receiver, freed exactly once —
#      no clone, no double-free). JIT + AOT.
#   2. A channel<Vec<i64>> moves owned heap aggregates across.
#   3. isSend: sending a `&T` borrow (or any non-Send value) on a channel is a
#      compile error — only owned, Send data may cross.
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

# 1. channel<String> across threads — owned Strings moved, no double-free.
cat > "$TMP/str.kd" <<'EOF'
fn produce(tx: Sender<String>) -> i64 ! { alloc, share } {
    let a = "alpha"; let b = "bravo"; let c = "charlie";
    chan_send(&tx, clone(&a));
    chan_send(&tx, clone(&b));
    chan_send(&tx, clone(&c));
    chan_close(tx);
    0
}
fn main() -> i64 ! { io, alloc, share } {
    let (tx, rx) = channel();
    let h = thread_spawn(|| produce(tx));
    chan_close(tx);          // main relinquishes its own Sender
    let mut total = 0;
    let mut go = true;
    while go {
        match chan_recv(&rx) { Some(s) => { total = total + str_len(&s); }, None => { go = false; }, }
    }
    thread_join(h);
    total   // 5 + 5 + 7 = 17
}
EOF
for run in 1 2 3; do
    jit=$("$KARDC" "$TMP/str.kd" 2>/dev/null | tail -1)
    [[ "$jit" == "17" ]] || { echo "FAIL [str/jit run $run]: expected 17 got '$jit'"; exit 1; }
done
"$KARDC" --no-cache -o "$TMP/str" "$TMP/str.kd" >/dev/null 2>&1
set +e; "$TMP/str" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 17 ]] || { echo "FAIL [str/aot]: exit $rc expected 17 (double-free?)"; exit 1; }
echo "PASS [channel-String]: owned Strings moved across threads -> 17, JIT+AOT, no double-free"

# 2. channel<Vec<i64>> — move owned heap aggregates across.
cat > "$TMP/vec.kd" <<'EOF'
fn produce(tx: Sender<Vec<i64>>) -> i64 ! { alloc, share } {
    let mut v = vec_new();
    vec_push(&mut v, 3); vec_push(&mut v, 4); vec_push(&mut v, 5);
    chan_send(&tx, v);     // move the whole Vec across
    chan_close(tx);
    0
}
fn main() -> i64 ! { io, alloc, share } {
    let (tx, rx) = channel();
    let h = thread_spawn(|| produce(tx));
    chan_close(tx);          // main relinquishes its own Sender
    let mut total = 0;
    let mut go = true;
    while go {
        match chan_recv(&rx) {
            Some(v) => {
                let mut i = 0;
                while i < vec_len(&v) { total = total + vec_get(&v, i); i = i + 1; }
            },
            None => { go = false; },
        }
    }
    thread_join(h);
    total   // 3 + 4 + 5 = 12
}
EOF
jit=$("$KARDC" "$TMP/vec.kd" 2>/dev/null | tail -1)
[[ "$jit" == "12" ]] || { echo "FAIL [vec/jit]: expected 12 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/vec" "$TMP/vec.kd" >/dev/null 2>&1
set +e; "$TMP/vec" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 12 ]] || { echo "FAIL [vec/aot]: exit $rc expected 12"; exit 1; }
echo "PASS [channel-Vec]: an owned Vec<i64> moved across a thread -> 12, JIT+AOT"

# 3. isSend teeth: sending a borrow on a channel is rejected.
cat > "$TMP/bad.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    let x = 5;
    chan_send(&tx, &x);
    0
}
EOF
set +e; out=$("$KARDC" "$TMP/bad.kd" 2>&1); set -e
if "$KARDC" "$TMP/bad.kd" >/dev/null 2>&1; then echo "FAIL [send-borrow]: compiled"; exit 1; fi
grep -qi "not .Send.\|not Send" <<<"$out" || { echo "FAIL [send-borrow]: wrong message: $out"; exit 1; }
echo "PASS [send-not-send]: sending a borrow on a channel is rejected (not Send)"

echo "ALL PHASE 77 SMOKE TESTS PASSED"
