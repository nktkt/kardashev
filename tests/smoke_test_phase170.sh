#!/usr/bin/env bash
# v31 Phase 170 smoke test — channel select (multiplexed wait) + scoped threads.
#
# select2/select3/select4(&r0,..) block (poll-with-backoff) until one of N
# HOMOGENEOUS &Receiver<T> is ready, returning a prelude
# SelectResult<T> { Ready(idx, value), Closed(idx) } (Ready wins over Closed
# within a sweep). Scoped threads: a move-only `Scope` whose Drop JOINS every
# thread spawned via scope_spawn (RAII join-before-scope-end), so a worker is
# guaranteed finished before the scope binding goes out of scope.
#
# Differentially gated JIT (last stdout line == value) vs AOT (exit code), with
# deterministic-over-N-runs assertions to catch races. (The C backend has no
# threads/channels; the LLVM path is the oracle.)
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
# so fall back to running the command directly (the timeout is only a
# deadlock safety-net; Bazel's own test timeout still bounds a true hang).
if command -v timeout >/dev/null 2>&1; then _TO=timeout
elif command -v gtimeout >/dev/null 2>&1; then _TO=gtimeout
else _TO=""; fi
run_to() { if [[ -n "$_TO" ]]; then "$_TO" "$@"; else shift; "$@"; fi; }

# JIT last-line value == $2 AND AOT exit code == ($2 mod 256).
jit_aot() { # $1=file $2=expected $3=label
    set +e
    local jv; jv=$(run_to 15 "$KARDC" "$1" 2>/dev/null | tail -1)
    "$KARDC" --no-cache -o "$1.out" "$1" >/dev/null 2>&1; local crc=$?
    set -e
    [[ "$crc" -eq 0 ]] || { echo "FAIL [$3]: AOT compile failed"; "$KARDC" "$1" 2>&1 | head -5; exit 1; }
    set +e; run_to 15 "$1.out"; local arc=$?; set -e
    [[ "$jv" == "$2" ]] || { echo "FAIL [$3/jit]: '$jv' != $2"; exit 1; }
    [[ "$arc" -eq $(( $2 % 256 )) ]] || { echo "FAIL [$3/aot]: exit $arc != $(($2 % 256))"; exit 1; }
    echo "PASS [$3]: JIT==$2, AOT exit==$(($2 % 256))"
}
expect_reject() { # $1=file $2=regex $3=label
    if "$KARDC" "$1" >/dev/null 2>&1; then echo "FAIL [$3]: compiled"; exit 1; fi
    local out; set +e; out=$("$KARDC" "$1" 2>&1); set -e
    grep -qiE "$2" <<<"$out" || { echo "FAIL [$3]: /$2/ not in:"; echo "$out"; exit 1; }
    echo "PASS [$3]"
}

# ---------------------------------------------------------------------------
# SELECT
# ---------------------------------------------------------------------------
# (1) select2 returns Ready(idx, value) for the ready receiver.
cat > "$TMP/ready.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (txa, rxa) = channel();
    let (txb, rxb) = channel();
    chan_send(&txb, 99);
    match select2(&rxa, &rxb) {
        Ready(idx, v) => idx * 1000 + v,   // 1*1000 + 99 = 1099
        Closed(idx) => 0 - 1,
    }
}
EOF
jit_aot "$TMP/ready.kd" 1099 "select2-ready"

# (2) select2 returns Closed(idx) for a drained+closed receiver.
cat > "$TMP/closed.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (txa, rxa) = channel();
    let (txb, rxb) = channel();
    chan_close(txa);                       // close a (empty)
    match select2(&rxa, &rxb) {
        Ready(idx, v) => 500 + v,
        Closed(idx) => 700 + idx,          // 700 + 0 = 700
    }
}
EOF
jit_aot "$TMP/closed.kd" 700 "select2-closed"

# (3) select3 picks the ready one of three.
cat > "$TMP/sel3.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (txa, rxa) = channel();
    let (txb, rxb) = channel();
    let (txc, rxc) = channel();
    chan_send(&txc, 7);
    match select3(&rxa, &rxb, &rxc) {
        Ready(idx, v) => idx * 100 + v,    // 2*100 + 7 = 207
        Closed(idx) => 0 - 1,
    }
}
EOF
jit_aot "$TMP/sel3.kd" 207 "select3-ready"

# (4) CROSS-THREAD: two producer threads each send 1..5 (offset) then close;
#     main select2-drains all 10 items. Deterministic sum == 530 on every run.
cat > "$TMP/drain.kd" <<'EOF'
fn prod(tx: Sender<i64>, base: i64) -> i64 ! { share } {
    let mut i = 1;
    while i <= 5 { chan_send(&tx, base + i); i = i + 1; }
    chan_close(tx);
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let (txa, rxa) = channel();
    let (txb, rxb) = channel();
    let t1 = thread_spawn(|| prod(txa, 0));      // 1..5
    let t2 = thread_spawn(|| prod(txb, 100));    // 101..105
    let mut sum = 0;
    let mut got = 0;
    while got < 10 {
        match select2(&rxa, &rxb) {
            Ready(idx, v) => { sum = sum + v; got = got + 1; },
            Closed(idx) => { got = got + 0; },
        }
    }
    thread_join(t1); thread_join(t2);
    print(sum);                                  // 15 + 515 = 530
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/drain.out" "$TMP/drain.kd" >/dev/null 2>&1 || {
    echo "FAIL [select-cross-thread-drain]: compile failed"; "$KARDC" "$TMP/drain.kd" 2>&1 | head -5; exit 1; }
for run in $(seq 1 12); do
    v=$(run_to 20 "$TMP/drain.out" 2>/dev/null | tail -1)
    [[ "$v" == "530" ]] || { echo "FAIL [select-cross-thread-drain]: run $run got '$v' (expected 530)"; exit 1; }
done
echo "PASS [select-cross-thread-drain]: 2 producers x5 drained via select2 == 530 on all 12 runs"

# ---------------------------------------------------------------------------
# SCOPED THREADS
# ---------------------------------------------------------------------------
# (5) RAII join: 4 workers each bump a shared Mutex 25k times. The counter is
#     read AFTER the Scope binding's block ends, with NO explicit join — so it
#     equals EXACTLY 100000 only because the Scope's Drop joined all 4 workers
#     (a missing join would read <100000, non-deterministically). 12 runs.
cat > "$TMP/scope.kd" <<'EOF'
fn bump(c: Mutex<i64>, n: i64) -> i64 ! { io } {
    let mut i = 0;
    while i < n { mutex_lock(c); mutex_set(c, mutex_get(c) + 1); mutex_unlock(c); i = i + 1; }
    0
}
fn run() -> i64 ! { alloc, io, share } {
    let c = mutex_new(0);
    {
        let s = scope_new();
        scope_spawn(&s, || bump(c, 25000));
        scope_spawn(&s, || bump(c, 25000));
        scope_spawn(&s, || bump(c, 25000));
        scope_spawn(&s, || bump(c, 25000));
    }                                 // s drops here -> joins all 4 workers
    mutex_get(c)                      // exactly 100000 iff all joined
}
fn main() -> i64 ! { alloc, io, share } { print(run()); 0 }
EOF
"$KARDC" --no-cache -o "$TMP/scope.out" "$TMP/scope.kd" >/dev/null 2>&1 || {
    echo "FAIL [scope-raii-join]: compile failed"; "$KARDC" "$TMP/scope.kd" 2>&1 | head -5; exit 1; }
for run in $(seq 1 12); do
    v=$(run_to 25 "$TMP/scope.out" 2>/dev/null | tail -1)
    [[ "$v" == "100000" ]] || { echo "FAIL [scope-raii-join]: run $run got '$v' (expected 100000 — workers not joined?)"; exit 1; }
done
echo "PASS [scope-raii-join]: 4 scope_spawn workers joined on scope Drop -> exactly 100000 (12 runs)"

# (6) NEGATIVE: a scope_spawn closure capturing a `let mut` BY REFERENCE is
#     rejected (the thread-boundary Send-capture rule, same as thread_spawn).
cat > "$TMP/scope_ref.kd" <<'EOF'
fn run() -> i64 ! { alloc, io, share } {
    let s = scope_new();
    let mut n = 0;
    scope_spawn(&s, || { n = n + 1; n });
    0
}
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/scope_ref.kd" "by-reference capture across a thread" "scope-byref-capture-rejected"

# (7) NEGATIVE: a Scope is not Send/Copy — it cannot be moved into a thread.
cat > "$TMP/scope_send.kd" <<'EOF'
fn run() -> i64 ! { alloc, io, share } {
    let s = scope_new();
    let t = thread_spawn(|| { let inner = s; 0 });
    thread_join(t);
    0
}
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/scope_send.kd" "Scope|not .?Send|Copy" "scope-not-sendable"

echo "ALL PHASE 170 SMOKE TESTS PASSED"
