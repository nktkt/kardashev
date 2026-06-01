#!/usr/bin/env bash
# v35 Phase 188 smoke test — iterator-adaptor / reducer completeness.
#
# Eager Vec-based adaptors (each returns a fresh owned Vec) + reducers, plus a
# lazy `iter_collect` that drains ANY `Iterator<T>` (e.g. a Range) into a Vec:
#   adaptors : vec_take / vec_skip / vec_chain / vec_zip / vec_enumerate
#   reducers : vec_sum / vec_any / vec_all / vec_find / vec_min / vec_max
#   lazy     : iter_collect (the bridge from the `Iterator` trait)
# Differentially gated JIT vs AOT.
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

# Adaptors: take / skip / chain / zip / enumerate over [1,2,3,4].
diff_run adaptors $'2\n3\n7\n8\n6\n3\n4' '
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 1); vec_push(&mut v, 2); vec_push(&mut v, 3); vec_push(&mut v, 4);
    let t = vec_take(&v, 2);  print(vec_len(&t)); print(vec_sum(&t));   // 2, 3
    let s = vec_skip(&v, 2);  print(vec_sum(&s));                       // 7
    print(vec_len(&vec_chain(&v, &v)));                                 // 8
    let z = vec_zip(&v, &v);  let p = vec_get(&z, 2); print(p.0 + p.1); // 6
    let e = vec_enumerate(&v); let q = vec_get(&e, 3); print(q.0); print(q.1); // 3, 4
    0
}
'

# Reducers: sum / any / all / find / min / max.
diff_run reducers $'10\n1\n0\n4\n1\n4' '
fn is_even(x: &i64) -> bool { (*x % 2) == 0 }
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 3); vec_push(&mut v, 1); vec_push(&mut v, 4); vec_push(&mut v, 2);
    print(vec_sum(&v));                                          // 10
    if vec_any(&v, is_even) { print(1); } else { print(0); }    // 1
    if vec_all(&v, is_even) { print(1); } else { print(0); }    // 0
    match vec_find(&v, is_even) { Some(x) => print(x), None => print(-1) }  // 4 (first even = 4)
    match vec_min(&v) { Some(x) => print(x), None => print(-1) }            // 1
    match vec_max(&v) { Some(x) => print(x), None => print(-1) }            // 4
    0
}
'

# Lazy bridge: iter_collect drains a Range (an Iterator) into a Vec.
diff_run iter_collect $'6\n3' '
fn main() -> i64 ! { io, alloc } {
    let mut r = 1..4;                      // 1,2,3
    let v = iter_collect(&mut r);
    print(vec_sum(&v));                    // 6
    print(vec_len(&v));                    // 3
    0
}
'

echo "ALL PHASE 188 SMOKE TESTS PASSED"
