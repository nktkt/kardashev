#!/usr/bin/env bash
# v31 Phase 169 smoke test — atomics + CAS + memory orderings.
#
# AtomicI64 / AtomicBool are Copy i64 handles over a naturally-aligned heap
# cell, Send+Sync, with load/store/swap/fetch_*/compare_exchange lowered to real
# LLVM atomicrmw/cmpxchg/atomic-load/store with the ordering baked into the op
# NAME (so the LLVM AtomicOrdering is a compile-time constant). An ergonomic
# `enum Ordering` + `impl AtomicI64/AtomicBool` layer (prelude) matches the
# ordering and dispatches to the statically-named builtins.
#
# Gating, weakest -> strongest:
#   (1) single-thread functional correctness vs hand-computed exit codes;
#   (2) the CONCURRENCY ORACLE — 8 threads x 50k fetch_add must equal EXACTLY
#       400000 on EVERY one of N repeated runs (a non-atomic add loses updates
#       and fails flakily — this is the real teeth);
#   (3) cross-check the atomic counter against the already-trusted Mutex path
#       (both must reach 400000).
# The C backend has no atomics (emit_c refuses them), so the LLVM AOT path is
# the oracle here.
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

# Compile (AOT) + run; assert the process exit code == $2.
run_exit() { # $1=file $2=expected_exit $3=label
    "$KARDC" --no-cache -o "$1.out" "$1" >/dev/null 2>&1 || {
        echo "FAIL [$3]: compile failed"; "$KARDC" "$1" 2>&1 | head -5; exit 1; }
    set +e; "$1.out"; local rc=$?; set -e
    [[ "$rc" -eq "$2" ]] || { echo "FAIL [$3]: exit $rc != $2"; exit 1; }
    echo "PASS [$3]: exit==$2"
}

# ---------------------------------------------------------------------------
# 1. Single-thread functional correctness (ergonomic API + Ordering).
# ---------------------------------------------------------------------------
cat > "$TMP/fa.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let a = AtomicI64::new(40);
    a.fetch_add(2, Ordering::SeqCst);     // 42
    a.load(Ordering::Acquire)
}
EOF
run_exit "$TMP/fa.kd" 42 "atomic-fetch-add"

cat > "$TMP/cas.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let a = AtomicI64::new(5);
    let ok = a.compare_exchange(5, 100, Ordering::AcqRel);   // succeeds -> 100
    let no = a.compare_exchange(5, 7, Ordering::AcqRel);     // fails (a==100)
    a.fetch_or(3, Ordering::SeqCst);                          // 100 | 3 = 103
    let v = a.load(Ordering::SeqCst);
    if ok { if no { v + 1000 } else { v } } else { 0 }        // 103
}
EOF
run_exit "$TMP/cas.kd" 103 "atomic-compare-exchange-and-bitops"

cat > "$TMP/bool.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let b = AtomicBool::new(false);
    b.store(true, Ordering::Release);
    let old = b.swap(false, Ordering::AcqRel);     // old == true
    let now = b.load(Ordering::Acquire);            // false
    if old { if now { 9 } else { 1 } } else { 0 }   // 1
}
EOF
run_exit "$TMP/bool.kd" 1 "atomic-bool"

cat > "$TMP/raw.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let a = atomic_i64_new(8);
    atomic_i64_fetch_sub_relaxed(a, 3);             // 5
    atomic_i64_fetch_xor_seqcst(a, 6);              // 5 ^ 6 = 3
    atomic_i64_load_relaxed(a)
}
EOF
run_exit "$TMP/raw.kd" 3 "atomic-raw-builtins"

# ---------------------------------------------------------------------------
# 2. THE CONCURRENCY ORACLE: 8 threads x 50k atomic fetch_add == EXACTLY
#    400000, on EVERY repeated run. A lost update (non-atomic add / wrong
#    ordering) fails flakily — repeating catches it.
# ---------------------------------------------------------------------------
cat > "$TMP/stress.kd" <<'EOF'
fn worker(c: AtomicI64, n: i64) -> i64 ! { io } {
    let mut i = 0;
    while i < n { c.fetch_add(1, Ordering::SeqCst); i = i + 1; }
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let c = AtomicI64::new(0);
    let t1 = thread_spawn(|| worker(c, 50000));
    let t2 = thread_spawn(|| worker(c, 50000));
    let t3 = thread_spawn(|| worker(c, 50000));
    let t4 = thread_spawn(|| worker(c, 50000));
    let t5 = thread_spawn(|| worker(c, 50000));
    let t6 = thread_spawn(|| worker(c, 50000));
    let t7 = thread_spawn(|| worker(c, 50000));
    let t8 = thread_spawn(|| worker(c, 50000));
    thread_join(t1); thread_join(t2); thread_join(t3); thread_join(t4);
    thread_join(t5); thread_join(t6); thread_join(t7); thread_join(t8);
    print(c.load(Ordering::SeqCst));   // exactly 400000
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/stress.out" "$TMP/stress.kd" >/dev/null 2>&1 || {
    echo "FAIL [atomic-counter-stress]: compile failed"; "$KARDC" "$TMP/stress.kd" 2>&1 | head -5; exit 1; }
RUNS=20
for run in $(seq 1 "$RUNS"); do
    v=$(run_to 30 "$TMP/stress.out" 2>/dev/null | head -1)
    [[ "$v" == "400000" ]] || { echo "FAIL [atomic-counter-stress]: run $run got '$v' (expected 400000 — lost update)"; exit 1; }
done
echo "PASS [atomic-counter-stress]: 8 threads x 50k fetch_add == 400000 on all $RUNS runs"

# ---------------------------------------------------------------------------
# 3. Cross-check the atomic counter against the trusted Mutex-guarded counter:
#    both must reach 400000 (an independent witness for the atomic path).
# ---------------------------------------------------------------------------
cat > "$TMP/mtx.kd" <<'EOF'
fn bump(c: Mutex<i64>, n: i64) -> i64 ! { io } {
    let mut i = 0;
    while i < n { mutex_lock(c); mutex_set(c, mutex_get(c) + 1); mutex_unlock(c); i = i + 1; }
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let c = mutex_new(0);
    let t1 = thread_spawn(|| bump(c, 50000));
    let t2 = thread_spawn(|| bump(c, 50000));
    let t3 = thread_spawn(|| bump(c, 50000));
    let t4 = thread_spawn(|| bump(c, 50000));
    let t5 = thread_spawn(|| bump(c, 50000));
    let t6 = thread_spawn(|| bump(c, 50000));
    let t7 = thread_spawn(|| bump(c, 50000));
    let t8 = thread_spawn(|| bump(c, 50000));
    thread_join(t1); thread_join(t2); thread_join(t3); thread_join(t4);
    thread_join(t5); thread_join(t6); thread_join(t7); thread_join(t8);
    print(mutex_get(c));
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/mtx.out" "$TMP/mtx.kd" >/dev/null 2>&1 || {
    echo "FAIL [atomic-vs-mutex-crosscheck]: mutex compile failed"; exit 1; }
mv=$(run_to 30 "$TMP/mtx.out" 2>/dev/null | head -1)
av=$(run_to 30 "$TMP/stress.out" 2>/dev/null | head -1)
[[ "$mv" == "400000" && "$av" == "400000" ]] || {
    echo "FAIL [atomic-vs-mutex-crosscheck]: atomic=$av mutex=$mv (both should be 400000)"; exit 1; }
echo "PASS [atomic-vs-mutex-crosscheck]: atomic counter agrees with the Mutex path (both 400000)"

echo "ALL PHASE 169 SMOKE TESTS PASSED"
