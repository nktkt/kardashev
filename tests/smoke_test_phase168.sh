#!/usr/bin/env bash
# v31 Phase 168 smoke test — type-safe RwLock<T> + RAII lock guards.
#
# Adds a reader/writer lock `RwLock<T>` (a Copy shareable handle mirroring
# Mutex, over a pthread_rwlock_t block) and three move-only RAII lock guards —
# MutexGuard<T> / RwLockReadGuard<T> / RwLockWriteGuard<T> — whose Drop releases
# the held lock at scope exit (the scoped-lock pattern, like C++ lock_guard /
# shared_lock / unique_lock). Data is read/written through the lock's typed
# get/set while a guard holds it.
#
# Each program is differentially gated JIT (kardc f.kd) vs AOT (kardc -o out
# f.kd && ./out): the same exit code from both paths AND the hand-computed
# expected value. RAII correctness is proven by RE-LOCKING after a guard's
# scope — a missing unlock-on-Drop would DEADLOCK (so each runtime case runs
# under `timeout`). The owned lock block is process-lifetime (not freed); owned
# free-on-Drop arrives with Arc<Mutex<T>> (a later v31 phase) — but the GUARDED
# CELL is dropped on each set (no leak), which case 4 proves.
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

# Run a program on BOTH paths and assert it yields $2. The JIT prints main's
# return as the last stdout line (the kardc process itself exits 0); the AOT
# binary's process exit code IS main's return. A deadlock (missing
# unlock-on-Drop) trips the `timeout` -> the JIT value is empty / AOT exit 124.
run_both() { # $1=file $2=expected $3=label
    set +e
    local jout; jout=$(run_to 15 "$KARDC" "$1" 2>/dev/null); local jrc=$?
    local jval; jval=$(tail -1 <<<"$jout")
    "$KARDC" --no-cache -o "$1.out" "$1" >/dev/null 2>&1; local crc=$?
    set -e
    [[ "$crc" -eq 0 ]] || { echo "FAIL [$3]: AOT compile failed"; "$KARDC" "$1" 2>&1 | head -4; exit 1; }
    set +e; run_to 15 "$1.out"; local arc=$?; set -e
    [[ "$jrc" -eq 0 && "$jval" == "$2" ]] || { echo "FAIL [$3/jit]: value '$jval' rc $jrc (want $2; 124=deadlock)"; exit 1; }
    [[ "$arc" -eq "$2" ]] || { echo "FAIL [$3/aot]: exit $arc (want $2; 124=deadlock)"; exit 1; }
    echo "PASS [$3]: JIT==AOT==$2"
}
expect_reject() { # $1=file $2=regex $3=label
    if "$KARDC" "$1" >/dev/null 2>&1; then
        echo "FAIL [$3]: compiled but expected a compile error"; exit 1; fi
    local out; set +e; out=$("$KARDC" "$1" 2>&1); set -e
    grep -qiE "$2" <<<"$out" || { echo "FAIL [$3]: error did not match /$2/:"; echo "$out"; exit 1; }
    echo "PASS [$3]"
}

# ---------------------------------------------------------------------------
# 1. RwLock<i64> manual API: rwlock_new / write / set / get / unlock / read.
#    10 + 5 == 15.
# ---------------------------------------------------------------------------
cat > "$TMP/rw.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let rw = rwlock_new(10);
    rwlock_write(rw);
    rwlock_set(rw, rwlock_get(rw) + 5);
    rwlock_unlock(rw);
    rwlock_read(rw);
    let v = rwlock_get(rw);
    rwlock_unlock(rw);
    v
}
EOF
run_both "$TMP/rw.kd" 15 "rwlock-manual-rdwr"

# ---------------------------------------------------------------------------
# 2. Mutex RAII guard: lock via guard, mutate, scope-exit auto-unlock, RE-LOCK.
#    A missing unlock-on-Drop would deadlock the second guard. 0 + 7 + 1 == 8.
# ---------------------------------------------------------------------------
cat > "$TMP/mguard.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let m = mutex_new(0);
    {
        let g = mutex_guard(m);
        mutex_set(m, mutex_get(m) + 7);
    }
    {
        let g2 = mutex_guard(m);   // DEADLOCKS if g didn't unlock on Drop
        mutex_set(m, mutex_get(m) + 1);
    }
    mutex_get(m)
}
EOF
run_both "$TMP/mguard.kd" 8 "mutex-guard-raii-relock"

# ---------------------------------------------------------------------------
# 3. RwLock RAII guards: a write guard then a read guard, each auto-released.
#    100 + 23 == 123.
# ---------------------------------------------------------------------------
cat > "$TMP/rwguard.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let rw = rwlock_new(100);
    { let w = rwlock_write_guard(rw); rwlock_set(rw, rwlock_get(rw) + 23); }
    { let r = rwlock_read_guard(rw); }      // read guard auto-releases
    rwlock_read(rw);
    let v = rwlock_get(rw);
    rwlock_unlock(rw);
    v
}
EOF
run_both "$TMP/rwguard.kd" 123 "rwlock-guards-raii"

# ---------------------------------------------------------------------------
# 4. No leak: a RwLock<String> set 50k times stays heap-flat (the OLD cell
#    String is dropped on each set) and is MALLOC_CHECK_=3 clean. len("xyz")==3.
# ---------------------------------------------------------------------------
cat > "$TMP/rwstr.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let rw = rwlock_new("ab");
    let mut i = 0;
    while i < 50000 {
        rwlock_write(rw);
        rwlock_set(rw, "xyz");
        rwlock_unlock(rw);
        i = i + 1;
    }
    rwlock_read(rw);
    let s = rwlock_get(rw);
    rwlock_unlock(rw);
    str_len(&s)
}
EOF
"$KARDC" --no-cache -o "$TMP/rwstr.out" "$TMP/rwstr.kd" >/dev/null 2>&1 || {
    echo "FAIL [rwlock-string-noleak]: AOT compile failed"; "$KARDC" "$TMP/rwstr.kd" 2>&1 | head -4; exit 1; }
set +e; MALLOC_CHECK_=3 "$TMP/rwstr.out"; src=$?; set -e
[[ "$src" -eq 3 ]] || { echo "FAIL [rwlock-string-noleak]: exit $src (expected 3, MALLOC_CHECK fault?)"; exit 1; }
if /usr/bin/time -v true >/dev/null 2>&1; then
    # `/usr/bin/time -v` propagates the child's exit (3 here), so capture under
    # `set +e` (pipefail would otherwise abort the assignment).
    set +e; rss=$(/usr/bin/time -v "$TMP/rwstr.out" 2>&1 | awk '/Maximum resident/ {print $NF}'); set -e
    [[ "$rss" -lt 30000 ]] || { echo "FAIL [rwlock-string-noleak]: RSS ${rss}KB (leak — old cell not dropped?)"; exit 1; }
    echo "PASS [rwlock-string-noleak]: 50k RwLock<String> sets stay flat (RSS ${rss}KB), MALLOC_CHECK clean"
else
    echo "PASS [rwlock-string-noleak]: MALLOC_CHECK clean (GNU time unavailable for RSS gate)"
fi

# ---------------------------------------------------------------------------
# 5. Soundness gates.
# ---------------------------------------------------------------------------
# A RwLock cell must be Send + owned (not a shared handle) — like a Mutex cell.
cat > "$TMP/rw_rc.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } { let rw = rwlock_new(rc_new(1)); 0 }
EOF
expect_reject "$TMP/rw_rc.kd" "RwLock" "rwlock-rc-cell-rejected"

# A Mutex cell is still gated (regression of the existing rule).
cat > "$TMP/mx_rc.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } { let m = mutex_new(rc_new(1)); 0 }
EOF
expect_reject "$TMP/mx_rc.kd" "Mutex" "mutex-rc-cell-rejected"

# A lock guard is thread-bound — it cannot be moved into a spawned thread.
cat > "$TMP/guard_thread.kd" <<'EOF'
fn main() -> i64 ! { alloc, io, share } {
    let m = mutex_new(0);
    let g = mutex_guard(m);
    let t = thread_spawn(|| { let gg = g; 0 });
    thread_join(t);
    0
}
EOF
expect_reject "$TMP/guard_thread.kd" "MutexGuard|not .?Send|Copy" "guard-not-sendable-into-thread"

echo "ALL PHASE 168 SMOKE TESTS PASSED"
