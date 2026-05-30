#!/usr/bin/env bash
# Phase 71 smoke test (Roadmap v12 — the real stdlib): HashMap / HashSet
# enumeration + membership — hashmap_contains / hashmap_values (Eq+Clone prelude
# scans over hashmap_get_ref / hashmap_keys) and hashset_items (a codegen
# built-in delegating to the underlying map's keys).
#   1. hashmap_contains finds present keys, rejects absent ones; hashmap_values
#      enumerates the values (deep-cloned). JIT + AOT.
#   2. hashset_items enumerates a set's elements (dedup respected).
#   3. String values are deep-cloned (non-Copy), no double-free (AOT clean).
# NOTE: hashmap_remove / hashset_remove are a deliberate deferral — open-
# addressing deletion needs tombstone-aware get/insert; tracked for a later phase.
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

# 1: hashmap_contains + hashmap_values (i64).
cat > "$TMP/hm.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, 1, 100);
    hashmap_insert(&mut m, 2, 200);
    hashmap_insert(&mut m, 3, 300);
    let k1 = 1; let k9 = 9;
    let c1 = hashmap_contains(&m, &k1);   // true
    let c2 = hashmap_contains(&m, &k9);   // false
    let vals = hashmap_values(&m);        // {100,200,300} some order
    let mut sum = 0; let mut i = 0;
    while i < vec_len(&vals) { sum = sum + vec_get(&vals, i); i = i + 1; }
    let mut w = sum;                       // 600
    if c1 { w = w + 1000; } else {}
    if c2 { w = w + 5000; } else {}
    w   // 600 + 1000 = 1600
}
EOF
jit=$("$KARDC" "$TMP/hm.kd" 2>/dev/null | tail -1)
[[ "$jit" == "1600" ]] || { echo "FAIL [hm/jit]: expected 1600 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/hm" "$TMP/hm.kd" >/dev/null 2>&1
set +e; "$TMP/hm" >/dev/null; rc=$?; set -e
[[ "$rc" -eq $((1600 & 255)) ]] || { echo "FAIL [hm/aot]: exit $rc expected $((1600 & 255))"; exit 1; }
echo "PASS [hashmap]: contains + values -> 1600, JIT+AOT"

# 2: hashset_items enumerates (dedup respected).
cat > "$TMP/hs.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut s = hashset_new();
    hashset_insert(&mut s, 5);
    hashset_insert(&mut s, 7);
    hashset_insert(&mut s, 5);   // duplicate
    let items = hashset_items(&s);   // {5,7}
    let mut sum = 0; let mut i = 0;
    while i < vec_len(&items) { sum = sum + vec_get(&items, i); i = i + 1; }
    sum + vec_len(&items) * 100   // 12 + 200 = 212
}
EOF
jit=$("$KARDC" "$TMP/hs.kd" 2>/dev/null | tail -1)
[[ "$jit" == "212" ]] || { echo "FAIL [hs/jit]: expected 212 got '$jit'"; exit 1; }
echo "PASS [hashset-items]: enumerate a set (dedup) -> 212"

# 3: String values deep-cloned (non-Copy), no double-free.
cat > "$TMP/sv.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    let a = "xx"; let b = "yyy";
    hashmap_insert(&mut m, 1, clone(&a));
    hashmap_insert(&mut m, 2, clone(&b));
    let vals = hashmap_values(&m);   // {"xx","yyy"} cloned
    let mut total = 0; let mut i = 0;
    while i < vec_len(&vals) { total = total + str_len(vec_get_ref(&vals, i)); i = i + 1; }
    total   // 2 + 3 = 5
}
EOF
jit=$("$KARDC" "$TMP/sv.kd" 2>/dev/null | tail -1)
[[ "$jit" == "5" ]] || { echo "FAIL [sv/jit]: expected 5 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/sv" "$TMP/sv.kd" >/dev/null 2>&1
set +e; "$TMP/sv" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 5 ]] || { echo "FAIL [sv/aot]: exit $rc expected 5 (double-free?)"; exit 1; }
echo "PASS [string-values]: hashmap_values deep-clones non-Copy values, JIT+AOT 5"

echo "ALL PHASE 71 SMOKE TESTS PASSED"
