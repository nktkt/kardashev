#!/usr/bin/env bash
# Phase 123 (Roadmap v21 — "generalize the i64/bool-MVP surfaces"): Mutex was
# Mutex<i64> only; its guarded cell is now an arbitrary T. mutex_new/get/set are
# specialized per cell type over a block `{ [64 x i8] pthread_mutex_t, T value }`
# while the i64 HANDLE stays Copy + shareable (so it still captures by value into
# a thread closure). This mirrors the handle-based `join<T>`/`spawn<T>` idiom: T
# is type-erased through the i64 handle, so `mutex_get<T>` (T return-only) is
# pinned by context or an explicit annotation. This test pins:
#   (1) a bool cell (T pinned by `if`), a struct cell (annotated), and i64
#       backward-compat all read/write correctly;
#   (2) mutex_get CLONES the cell + mutex_set DROPS the old value, so a String
#       cell over 100k sets is RSS-flat and heap-clean (MALLOC_CHECK_=3);
#   (3) a STRUCT cell shared across two threads with lock/unlock lands on the
#       exact total (mutual exclusion holds for a non-i64 cell).
# The struct/String/thread (AOT) parts skip cleanly if no clang.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 1. bool / struct / i64 cells (JIT — needs only kardc).
cat > "$TMP/cells.kd" <<'EOF'
struct Point { x: i64, y: i64 }
fn main() -> i64 ! { alloc, io } {
    let bm = mutex_new(true);
    print(if mutex_get(bm) { 1 } else { 0 });   // 1 (bool pinned by `if`)
    mutex_set(bm, false);
    print(if mutex_get(bm) { 1 } else { 0 });   // 0

    let pm = mutex_new(Point { x: 3, y: 4 });
    let p: Point = mutex_get(pm);                // annotation pins T = Point
    print(p.x + p.y);                            // 7
    mutex_set(pm, Point { x: 10, y: 20 });
    let q: Point = mutex_get(pm);
    print(q.x + q.y);                            // 30

    let im = mutex_new(100);                      // backward-compat: i64 cell
    mutex_set(im, mutex_get(im) + 1);
    print(mutex_get(im));                         // 101
    0
}
EOF
got=$("$KARDC" "$TMP/cells.kd" 2>/dev/null)
[[ "$got" == $'1\n0\n7\n30\n101\n0' ]] || { echo "FAIL [cells]: got '$got' want 1,0,7,30,101,0"; exit 1; }
echo "PASS [cells]: Mutex over bool / struct / i64 reads + writes the right values"

CLANG="$(command -v clang || true)"
if [[ -z "$CLANG" ]]; then
    echo "PASS [heap/threads]: SKIPPED (no clang for AOT)"
    echo "ALL MUTEX-GENERIC SMOKE TESTS PASSED"
    exit 0
fi

# 2. String cell: get clones, set drops the old value — 100k sets stay RSS-flat
#    and heap-clean (a missing drop would leak; a double-free would abort).
cat > "$TMP/str.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let sm = mutex_new("seed".to_string());
    let mut i = 0;
    while i < 100000 { mutex_set(sm, int_to_string(i)); i = i + 1; }
    let last: String = mutex_get(sm);
    print(str_len(&last));   // len("99999") = 5
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/str" "$TMP/str.kd" >/dev/null 2>&1 || { echo "FAIL [heap/str]: build failed"; exit 1; }
out=$(MALLOC_CHECK_=3 "$TMP/str" 2>"$TMP/e"); rc=$?
[[ "$rc" -eq 0 ]] || { echo "FAIL [heap/str]: rc=$rc ($(head -1 "$TMP/e"))"; exit 1; }
[[ "$out" == "5" ]] || { echo "FAIL [heap/str]: got '$out' want 5"; exit 1; }
rss=""
if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -v "$TMP/str" >/dev/null 2>"$TMP/t"
    rss=$(grep -oE 'Maximum resident set size \(kbytes\): [0-9]+' "$TMP/t" 2>/dev/null | grep -oE '[0-9]+$' || true)
fi
if [[ -n "$rss" ]]; then
    [[ "$rss" -lt 32768 ]] || { echo "FAIL [heap/str]: RSS $rss KB over 100k mutex_set — set leaks the old value"; exit 1; }
    echo "PASS [heap/str]: Mutex<String> get-clones/set-drops — 100k sets RSS-flat (${rss} KB), heap-clean"
else
    echo "PASS [heap/str]: Mutex<String> heap-clean under MALLOC_CHECK_=3 (RSS gate skipped — no GNU time)"
fi

# 3. A STRUCT cell shared across two threads: mutual exclusion must hold for a
#    non-i64 cell (an unsynchronized struct read-modify-write would lose updates).
cat > "$TMP/threads.kd" <<'EOF'
struct Counter { hits: i64, sum: i64 }
fn worker(m: i64) -> i64 ! { io } {
    let mut i = 0;
    while i < 100000 {
        mutex_lock(m);
        let c: Counter = mutex_get(m);
        mutex_set(m, Counter { hits: c.hits + 1, sum: c.sum + 2 });
        mutex_unlock(m);
        i = i + 1;
    }
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let m = mutex_new(Counter { hits: 0, sum: 0 });
    let t1 = thread_spawn(|| worker(m));
    let t2 = thread_spawn(|| worker(m));
    thread_join(t1);
    thread_join(t2);
    let f: Counter = mutex_get(m);
    print(f.hits);   // 200000
    print(f.sum);    // 400000
    0
}
EOF
"$KARDC" --no-cache -o "$TMP/threads" "$TMP/threads.kd" >/dev/null 2>&1 || { echo "FAIL [threads]: build failed"; exit 1; }
# AOT binary: main's `0` return is the exit code, not printed -> 2 lines.
bad=0
for r in 1 2 3; do
    out=$("$TMP/threads" 2>/dev/null)
    [[ "$out" == $'200000\n400000' ]] || { bad=$((bad+1)); }
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [threads]: $bad/3 runs lost updates (last '$out', want 200000,400000)"; exit 1; }
# JIT path also echoes main's return value (trailing 0).
jout=$("$KARDC" "$TMP/threads.kd" 2>/dev/null)
[[ "$jout" == $'200000\n400000\n0' ]] || { echo "FAIL [threads/JIT]: got '$jout' want 200000,400000,0"; exit 1; }
echo "PASS [threads]: Mutex<struct> across 2 threads -> exact 200000/400000 (mutual exclusion, JIT+AOT)"

echo "ALL MUTEX-GENERIC SMOKE TESTS PASSED"
