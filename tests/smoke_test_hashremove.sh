#!/usr/bin/env bash
# Phase 122 (Roadmap v21 — "close the leaks / the one genuinely-missing stdlib
# op"): HashMap/HashSet `remove`. Open-addressing deletion is done by
# BACKWARD-SHIFT (Knuth Algorithm R) rather than tombstones, so get/insert/grow
# stay untouched — the "stop at the first empty slot" probe invariant holds
# because every live key remains reachable from its home by a contiguous run of
# occupied slots. This test pins:
#   (1) head / middle / tail removal of a collision chain (incl. wrap-around)
#       keeps every surviving key findable with the right value;
#   (2) a 50-key insert-then-remove-evens ORACLE: every odd key present with its
#       value, every even key absent, len exactly right;
#   (3) String key/value remove drops the key + moves the value out exactly once
#       (heap-clean under MALLOC_CHECK_=3);
#   (4) a 200k insert+remove churn loop is RSS-flat and never infinite-probes
#       (no tombstone accumulation).
# The JIT correctness parts need only kardc; the MALLOC_CHECK / RSS parts AOT-
# compile and are skipped cleanly if no clang is available.
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

# 1. Collision-chain correctness (head / middle / tail / wrap-around). At cap 8,
#    keys that share a residue mod 8 form a contiguous probe chain.
cat > "$TMP/chain.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let mut m: HashMap<i64, i64> = hashmap_new();
    // 0,16,32 all hash to slot 0 (cap 8): chain slot0,1,2.
    hashmap_insert(&mut m, 0, 100);
    hashmap_insert(&mut m, 16, 116);
    hashmap_insert(&mut m, 32, 132);
    match hashmap_remove(&mut m, 16) { Some(v) => print(v), None => print(-1) } // 116 (middle)
    match hashmap_get(&m, 32) { Some(v) => print(v), None => print(-1) }        // 132 (tail still found)
    match hashmap_get(&m, 0)  { Some(v) => print(v), None => print(-1) }        // 100
    match hashmap_get(&m, 16) { Some(v) => print(v), None => print(-1) }        // -1
    // wrap-around: 7,15,23 all hash to slot 7 -> slot7,0,1.
    let mut w: HashMap<i64, i64> = hashmap_new();
    hashmap_insert(&mut w, 7, 700);
    hashmap_insert(&mut w, 15, 715);
    hashmap_insert(&mut w, 23, 723);
    match hashmap_remove(&mut w, 7) { Some(v) => print(v), None => print(-1) }  // 700 (head, wrapped)
    match hashmap_get(&w, 15) { Some(v) => print(v), None => print(-1) }        // 715
    match hashmap_get(&w, 23) { Some(v) => print(v), None => print(-1) }        // 723
    0
}
EOF
got=$("$KARDC" "$TMP/chain.kd" 2>/dev/null)
want=$'116\n132\n100\n-1\n700\n715\n723\n0'
[[ "$got" == "$want" ]] || { echo "FAIL [chain]: got '$got' want '$want' (back-shift broke a probe chain)"; exit 1; }
echo "PASS [chain]: head/middle/tail + wrap-around removal preserves the probe chain"

# 2. 50-key oracle: insert 0..49 (value k*7+1), remove the evens, verify every
#    key. ok must be 1 and len exactly 25 (the 25 odd keys).
cat > "$TMP/oracle.kd" <<'EOF'
// 1 iff the lookup result matches the oracle: present-odd keys hold their value,
// even keys are absent (got == -1 sentinel; real values are i*7+1, never -1).
fn check(got: i64, odd: i64, want_val: i64) -> i64 {
    if odd == 1 { if got == want_val { 1 } else { 0 } }
    else { if got == 0 - 1 { 1 } else { 0 } }
}
fn main() -> i64 ! { alloc, io } {
    let mut m: HashMap<i64, i64> = hashmap_new();
    let mut i = 0;
    while i < 50 { hashmap_insert(&mut m, i, i * 7 + 1); i = i + 1; }
    i = 0;
    while i < 50 { hashmap_remove(&mut m, i * 2); i = i + 1; }  // removes evens (50..98 are misses)
    let mut ok = 1;
    i = 0;
    while i < 50 {
        let g = match hashmap_get(&m, i) { Some(v) => v, None => 0 - 1 };
        ok = ok * check(g, i % 2, i * 7 + 1);
        i = i + 1;
    }
    print(ok);              // 1
    print(hashmap_len(&m)); // 25
    0
}
EOF
got=$("$KARDC" "$TMP/oracle.kd" 2>/dev/null)
[[ "$got" == $'1\n25\n0' ]] || { echo "FAIL [oracle]: got '$got' want 1,25,0 (a removed/surviving key lookup is wrong)"; exit 1; }
echo "PASS [oracle]: 50-key insert + remove-evens — all 50 lookups correct, len=25"

# 3. hashset_remove correctness (bool present/absent).
cat > "$TMP/set.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let mut s: HashSet<i64> = hashset_new();
    hashset_insert(&mut s, 10);
    hashset_insert(&mut s, 20);
    hashset_insert(&mut s, 30);
    if hashset_remove(&mut s, 20) { print(1); } else { print(0); }  // 1 (present)
    if hashset_remove(&mut s, 20) { print(1); } else { print(0); }  // 0 (already gone)
    if hashset_contains(&s, 10) { print(1); } else { print(0); }    // 1
    if hashset_contains(&s, 20) { print(1); } else { print(0); }    // 0
    if hashset_contains(&s, 30) { print(1); } else { print(0); }    // 1
    print(hashset_len(&s));  // 2
    0
}
EOF
got=$("$KARDC" "$TMP/set.kd" 2>/dev/null)
[[ "$got" == $'1\n0\n1\n0\n1\n2\n0' ]] || { echo "FAIL [set]: got '$got' want 1,0,1,0,1,2,0"; exit 1; }
echo "PASS [set]: hashset_remove reports present/absent and updates membership + len"

# 4 & 5 (AOT, heap-clean): String key/value drop-once + churn RSS-flat. Skipped
#    if no clang.
CLANG="$(command -v clang || true)"
if [[ -z "$CLANG" ]]; then
    echo "PASS [heap]: SKIPPED (no clang for AOT MALLOC_CHECK / RSS gates)"
    echo "ALL HASHREMOVE SMOKE TESTS PASSED"
    exit 0
fi

# 4. String key/value remove: the removed key is dropped, the value moved out;
#    repeated under MALLOC_CHECK_=3 a double-free would abort.
cat > "$TMP/str.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut total = 0;
    let mut n = 0;
    while n < 20000 {
        let mut m: HashMap<String, String> = hashmap_new();
        hashmap_insert(&mut m, "alpha".to_string(), "A".to_string());
        hashmap_insert(&mut m, "beta".to_string(), "B".to_string());
        hashmap_insert(&mut m, "gamma".to_string(), "G".to_string());
        match hashmap_remove(&mut m, "beta".to_string()) {
            Some(v) => { total = total + str_len(&v); },   // value moved out, then dropped
            None => { total = total + 0; },
        }
        // miss path drops the lookup key too
        match hashmap_remove(&mut m, "zzz".to_string()) {
            Some(v) => { total = total + str_len(&v); },
            None => { total = total + 0; },
        }
        total = total + hashmap_len(&m);  // 2 each iter
        n = n + 1;
    }
    total
}
EOF
"$KARDC" --no-cache -o "$TMP/str" "$TMP/str.kd" >/dev/null 2>&1 || { echo "FAIL [heap/str]: build failed"; exit 1; }
bad=0
for r in 1 2 3; do
    MALLOC_CHECK_=3 "$TMP/str" >/dev/null 2>"$TMP/e"; rc=$?
    if [[ "$rc" -eq 134 ]] || grep -qi 'free\|corrupt' "$TMP/e"; then bad=$((bad+1)); fi
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [heap/str]: $bad/3 runs corrupted the heap (remove drops the key/value wrong)"; exit 1; }
echo "PASS [heap/str]: 20k String-map remove (hit+miss) heap-clean under MALLOC_CHECK_=3"

# 5. Churn: 200k insert+remove over 64 keys must stay RSS-flat and never hang
#    (no tombstone accumulation => no infinite probe).
cat > "$TMP/churn.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m: HashMap<i64, i64> = hashmap_new();
    let mut i = 0; let mut acc = 0;
    while i < 200000 {
        hashmap_insert(&mut m, i % 64, i % 64 + 1);
        match hashmap_remove(&mut m, (i + 1) % 64) {
            Some(v) => { acc = acc + v; }, None => { acc = acc + 0; },
        }
        i = i + 1;
    }
    acc + hashmap_len(&m)
}
EOF
"$KARDC" --no-cache -o "$TMP/churn" "$TMP/churn.kd" >/dev/null 2>&1 || { echo "FAIL [heap/churn]: build failed"; exit 1; }
TO="timeout 30"; command -v timeout >/dev/null 2>&1 || TO=""
bad=0
for r in 1 2 3; do
    $TO env MALLOC_CHECK_=3 "$TMP/churn" >/dev/null 2>"$TMP/e"; rc=$?
    if [[ "$rc" -eq 124 ]]; then echo "FAIL [heap/churn]: timed out (infinite probe?)"; exit 1; fi
    if [[ "$rc" -eq 134 ]] || grep -qi 'free\|corrupt' "$TMP/e"; then bad=$((bad+1)); fi
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [heap/churn]: $bad/3 runs corrupted the heap"; exit 1; }
rss=""
if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -v "$TMP/churn" >/dev/null 2>"$TMP/t"
    rss=$(grep -oE 'Maximum resident set size \(kbytes\): [0-9]+' "$TMP/t" 2>/dev/null | grep -oE '[0-9]+$' || true)
fi
if [[ -n "$rss" ]]; then
    [[ "$rss" -lt 32768 ]] || { echo "FAIL [heap/churn]: RSS $rss KB over 200k insert+remove — leak"; exit 1; }
    echo "PASS [heap/churn]: 200k insert+remove RSS-flat (${rss} KB), heap-clean, no infinite probe"
else
    echo "PASS [heap/churn]: 200k insert+remove heap-clean, no infinite probe (RSS gate skipped — no GNU time)"
fi

echo "ALL HASHREMOVE SMOKE TESTS PASSED"
