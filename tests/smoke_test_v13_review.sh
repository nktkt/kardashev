#!/usr/bin/env bash
# v13 PRE-MERGE ADVERSARIAL REVIEW regression test (Roadmap v13 — concurrency).
# Pins the three real defects the multi-agent review found in the first v13 cut,
# all fixed in the borrow checker + the refcounted channel-endpoint redesign
# (Phase 81). A green channel suite did NOT catch these — this test exists so
# they can never silently regress.
#
#   1. BLOCKER (use-after-free): a borrow returned by rc_get(&a) / vec_get_ref(&v)
#      is now tied to the owner's lifetime, so moving/dropping the owner while
#      the borrow is live is a COMPILE ERROR (was: compiled, read freed memory).
#   2. MAJOR (unbounded leak): channel() blocks, queued nodes, and undrained
#      droppable payloads are all reclaimed when the last endpoint drops — RSS is
#      FLAT over a channel-per-iteration loop (was: ~172 B leaked per channel).
#   3. MAJOR (multi-producer data loss): the channel closes only when the LAST
#      Sender is gone (refcounted), so one producer finishing can no longer
#      abandon another live producer's queued items (was: 84/100 runs lost ALL
#      of a second producer's data).
# Plus: moving an owned value across a channel still drops it EXACTLY once.
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

# ---------------------------------------------------------------------------
# 1. BLOCKER: rc_get / vec_get_ref borrow keeps the owner alive (no UAF).
# ---------------------------------------------------------------------------
cat > "$TMP/uaf_rc.kd" <<'EOF'
fn consume(a: Rc<i64>) -> i64 { 0 }
fn main() -> i64 ! { io, alloc } {
    let a = rc_new(42);
    let r = rc_get(&a);    // r: &i64 borrows into a's heap block
    consume(a);            // would free a's block while r is live
    print(*r);             // UAF — must be rejected at compile time
    0
}
EOF
set +e; out=$("$KARDC" "$TMP/uaf_rc.kd" 2>&1); compiled=$("$KARDC" "$TMP/uaf_rc.kd" >/dev/null 2>&1; echo $?); set -e
[[ "$compiled" -ne 0 ]] || { echo "FAIL [uaf-rc]: compiled (use-after-free)"; exit 1; }
grep -qi "borrow" <<<"$out" || { echo "FAIL [uaf-rc]: wrong diagnostic: $out"; exit 1; }
echo "PASS [uaf-rc]: moving an Rc while an rc_get borrow is live is rejected"

cat > "$TMP/uaf_vec.kd" <<'EOF'
fn consume(v: Vec<i64>) -> i64 { 0 }
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 42);
    let r = vec_get_ref(&v, 0);
    consume(v);            // would free v's buffer while r is live
    print(*r);
    0
}
EOF
if "$KARDC" "$TMP/uaf_vec.kd" >/dev/null 2>&1; then echo "FAIL [uaf-vec]: compiled (use-after-free)"; exit 1; fi
echo "PASS [uaf-vec]: moving a Vec while a vec_get_ref borrow is live is rejected"

# The legitimate use (borrow, read, never move the owner) still compiles + runs.
cat > "$TMP/ok_rc.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let a = rc_new(42);
    let r = rc_get(&a);
    print(*r);                    // 42
    print(rc_strong_count(&a));   // 1
    0
}
EOF
got=$("$KARDC" "$TMP/ok_rc.kd" 2>/dev/null); got=$(head -2 <<< "$got")
[[ "$got" == $'42\n1' ]] || { echo "FAIL [ok-rc]: legit rc_get use broke: $got"; exit 1; }
echo "PASS [ok-rc]: a live rc_get borrow without a move still compiles + runs"

# ---------------------------------------------------------------------------
# 2. MAJOR: no unbounded channel leak. Block + queued nodes + undrained
#    droppable payloads are all freed when the last endpoint drops -> flat RSS.
# ---------------------------------------------------------------------------
if command -v /usr/bin/time >/dev/null 2>&1 && /usr/bin/time -v true >/dev/null 2>&1; then
    # (a) channel created + fully drained per call, 1,000,000 times.
    cat > "$TMP/leak.kd" <<'EOF'
fn one() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(&tx, 1);
    match chan_recv(&rx) { Some(x) => x, None => 0 }
}
fn main() -> i64 ! { alloc, share } {
    let mut i = 0; let mut acc = 0;
    while i < 1000000 { acc = (acc + one()) - 1; i = i + 1; }
    acc
}
EOF
    "$KARDC" --no-cache -o "$TMP/leak" "$TMP/leak.kd" >/dev/null 2>&1
    rss=$(/usr/bin/time -v "$TMP/leak" 2>&1 | awk '/Maximum resident/ {print $NF}')
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [chan-no-leak]: RSS ${rss}KB over 1M channels (block leak)"; exit 1; }
    echo "PASS [chan-no-leak]: 1,000,000 channels created+drained stay flat (RSS ${rss}KB)"

    # (b) channel dropped WITH undrained heap-owning (Vec) payloads, 200k times:
    #     the teardown drains the queue, dropping each payload + freeing nodes.
    cat > "$TMP/drain.kd" <<'EOF'
fn one() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    let mut v = vec_new(); vec_push(&mut v, 1); vec_push(&mut v, 2); vec_push(&mut v, 3);
    chan_send(&tx, v);     // sent, never received
    0                      // tx/rx drop -> teardown drains + frees the Vec + node
}
fn main() -> i64 ! { alloc, share } {
    let mut i = 0; let mut acc = 0;
    while i < 200000 { acc = acc + one(); i = i + 1; }
    acc
}
EOF
    "$KARDC" --no-cache -o "$TMP/drain" "$TMP/drain.kd" >/dev/null 2>&1
    rss=$(/usr/bin/time -v "$TMP/drain" 2>&1 | awk '/Maximum resident/ {print $NF}')
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [chan-drain]: RSS ${rss}KB (undrained node/payload leak)"; exit 1; }
    echo "PASS [chan-drain]: 200k channels dropped with undrained Vec payloads stay flat (RSS ${rss}KB)"
else
    echo "SKIP [chan-no-leak/chan-drain]: GNU /usr/bin/time -v unavailable"
fi

# Moving an owned Vec across a channel and receiving it drops it EXACTLY once
# (no double-free on the happy path) — vec_len of the received Vec is 2.
cat > "$TMP/move.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    let mut v = vec_new(); vec_push(&mut v, 7); vec_push(&mut v, 8);
    chan_send(&tx, v);
    match chan_recv(&rx) { Some(w) => vec_len(&w), None => 0 }
}
EOF
"$KARDC" --no-cache -o "$TMP/move" "$TMP/move.kd" >/dev/null 2>&1
bad=0
for r in 1 2 3 4 5 6 7 8 9 10; do
    set +e; MALLOC_CHECK_=3 "$TMP/move" >/dev/null 2>"$TMP/mc.err"; rc=$?; set -e
    if [[ "$rc" -ge 128 || -s "$TMP/mc.err" ]]; then bad=$((bad+1)); fi
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [chan-move]: $bad/10 runs corrupted the heap (double-free)"; exit 1; }
echo "PASS [chan-move]: an owned Vec moved across a channel is dropped exactly once (MALLOC_CHECK_=3 x10)"

# ---------------------------------------------------------------------------
# 3. MAJOR: multi-producer integrity. One producer finishing/closing must NOT
#    abandon another live producer's queued items — the channel closes only
#    when the LAST Sender is gone. recv-until-None must drain ALL 600 items.
# ---------------------------------------------------------------------------
cat > "$TMP/mpc.kd" <<'EOF'
fn produce(tx: Sender<i64>, n: i64) -> i64 ! { share } {
    let mut i = 0; while i < n { chan_send(&tx, 1); i = i + 1; }
    0   // this producer's Sender drops here (senders--, not a close unless last)
}
fn main() -> i64 ! { io, alloc, share } {
    let (tx, rx) = channel();
    let h1 = thread_spawn(|| produce(tx, 300));   // tx cloned
    let h2 = thread_spawn(|| produce(tx, 300));   // tx cloned
    chan_close(tx);                               // main relinquishes its Sender
    let mut total = 0;
    let mut go = true;
    while go {
        match chan_recv(&rx) { Some(v) => { total = total + v; }, None => { go = false; }, }
    }
    thread_join(h1); thread_join(h2);
    total   // EXACTLY 600 — no producer's items abandoned
}
EOF
"$KARDC" --no-cache -o "$TMP/mpc" "$TMP/mpc.kd" >/dev/null 2>&1
# A hang-guard if a `timeout`-like command exists (GNU `timeout` on Linux,
# `gtimeout` from coreutils on macOS); otherwise run directly — a sound build
# never hangs, and macOS lacks `timeout` by default.
TIMEOUT=""
if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 10";
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT="gtimeout 10"; fi
bad=0
for r in $(seq 1 60); do
    set +e; $TIMEOUT "$TMP/mpc" >/dev/null 2>&1; rc=$?; set -e
    if [[ -n "$TIMEOUT" && "$rc" -eq 124 ]]; then echo "FAIL [mpc]: hang on run $r"; exit 1; fi
    # exit code is total & 255 = 600 & 255 = 88; a lost item changes the total.
    [[ "$rc" -eq $((600 & 255)) ]] || { bad=$((bad+1)); echo "run $r: exit $rc (expected $((600 & 255)))"; }
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [mpc]: $bad/60 runs lost data (multi-producer close)"; exit 1; }
echo "PASS [mpc]: 2 producers x300, one closing early -> exactly 600 every run, no loss (60 runs)"

# Single-producer send-then-close still drains all items then None (regression).
cat > "$TMP/sp.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    let mut i = 0; while i < 500 { chan_send(&tx, 1); i = i + 1; }
    chan_close(tx);
    let mut total = 0; let mut go = true;
    while go {
        match chan_recv(&rx) { Some(v) => { total = total + v; }, None => { go = false; }, }
    }
    total   // 500
}
EOF
jit=$("$KARDC" "$TMP/sp.kd" 2>/dev/null | tail -1)
[[ "$jit" == "500" ]] || { echo "FAIL [single-producer-drain]: expected 500 got '$jit'"; exit 1; }
echo "PASS [single-producer-drain]: send 500 then close drains all then None -> 500"

echo "ALL v13 REVIEW REGRESSION TESTS PASSED"
