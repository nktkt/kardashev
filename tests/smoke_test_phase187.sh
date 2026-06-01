#!/usr/bin/env bash
# v35 Phase 187 smoke test — ordered collections + double-ended queue.
#
# Three new prelude container types, written in kardashev over the `Vec`
# primitive and the existing `Ord` trait (`cmp -> i64`):
#   - VecDeque<T>     : a two-stack double-ended queue, O(1) amortized at both
#                       ends, pops returning Option<T>.
#   - BTreeMap<K:Ord,V>: an ordered map (parallel sorted key/value Vecs, binary
#                       search); iteration is in ascending key order — the
#                       property that distinguishes it from the unordered HashMap.
#   - BTreeSet<T:Ord> : an ordered set (sorted Vec, dedup on insert).
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

# 1. VecDeque: push at both ends, pop at both ends, including the empty-side
#    flip (push_front then pop_back must reach across the two stacks).
diff_run vecdeque $'3\n0\n2\n1\n99' '
fn main() -> i64 ! { io, alloc } {
    let mut d: VecDeque<i64> = vecdeque_new();
    vecdeque_push_back(&mut d, 1);
    vecdeque_push_back(&mut d, 2);
    vecdeque_push_front(&mut d, 0);                                   // 0,1,2
    print(vecdeque_len(&d));                                          // 3
    match vecdeque_pop_front(&mut d) { Some(x) => print(x), None => print(99) }  // 0
    match vecdeque_pop_back(&mut d)  { Some(x) => print(x), None => print(99) }  // 2
    match vecdeque_pop_front(&mut d) { Some(x) => print(x), None => print(99) }  // 1
    match vecdeque_pop_front(&mut d) { Some(x) => print(x), None => print(99) }  // empty -> 99
    0
}
'

# 2. BTreeMap<i64,i64>: insert out of order + a duplicate (replace), ascending
#    iteration, get hit/miss, contains, remove.
diff_run btreemap_i64 $'3\n10\n20\n30\n111\n-1\n1\n2' '
fn main() -> i64 ! { io, alloc } {
    let mut m: BTreeMap<i64, i64> = btreemap_new();
    btreemap_insert(&mut m, 30, 300);
    btreemap_insert(&mut m, 10, 100);
    btreemap_insert(&mut m, 20, 200);
    btreemap_insert(&mut m, 10, 111);                                  // replace
    print(btreemap_len(&m));                                           // 3
    let mut i = 0;
    while i < btreemap_len(&m) { print(btreemap_key_at(&m, i)); i = i + 1; }   // 10,20,30
    match btreemap_get(&m, &10) { Some(v) => print(v), None => print(-1) }     // 111
    match btreemap_get(&m, &25) { Some(v) => print(v), None => print(-1) }     // -1
    if btreemap_contains(&m, &20) { print(1); } else { print(0); }    // 1
    btreemap_remove(&mut m, &20);
    print(btreemap_len(&m));                                           // 2
    0
}
'

# 3. BTreeMap<String,i64>: ordering driven by the String Ord::cmp (lexicographic
#    bytes). Insert cherry/apple/banana → iterate alphabetically (first byte
#    a=97,b=98,c=99); get by string key.
diff_run btreemap_string $'97\n98\n99\n2' '
fn main() -> i64 ! { io, alloc } {
    let mut m: BTreeMap<String, i64> = btreemap_new();
    btreemap_insert(&mut m, "cherry", 3);
    btreemap_insert(&mut m, "apple", 1);
    btreemap_insert(&mut m, "banana", 2);
    let mut i = 0;
    while i < btreemap_len(&m) { let k = btreemap_key_at(&m, i); print(str_char_at(&k, 0)); i = i + 1; }
    match btreemap_get(&m, &"banana") { Some(v) => print(v), None => print(-1) }   // 2
    0
}
'

# 4. BTreeSet<i64>: dedup on insert, ascending iteration, membership.
diff_run btreeset $'3\n1\n2\n5\n0' '
fn main() -> i64 ! { io, alloc } {
    let mut s: BTreeSet<i64> = btreeset_new();
    btreeset_insert(&mut s, 5);
    btreeset_insert(&mut s, 1);
    btreeset_insert(&mut s, 2);
    btreeset_insert(&mut s, 1);                                        // dup -> ignored
    print(btreeset_len(&s));                                           // 3
    let mut i = 0;
    while i < btreeset_len(&s) { print(btreeset_at(&s, i)); i = i + 1; }   // 1,2,5
    if btreeset_contains(&s, &4) { print(1); } else { print(0); }     // 0
    0
}
'

echo "ALL PHASE 187 SMOKE TESTS PASSED"
