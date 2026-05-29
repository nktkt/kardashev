#!/usr/bin/env bash
# Phase 46 smoke test: generic Clone/Eq TRAIT impls for HashMap (the v7
# deferral, unblocked by Phase 45). Before v8 the prelude had no
# `impl<..> Clone/Eq for HashMap<K,V>`, so `map.clone()` only worked via the
# structural `clone(&x)` intrinsic and `#[derive]` could not cover a HashMap
# field (the synthesized clone/eq dispatch to the field's trait impl).
#   1. Direct trait methods: `map.clone()` deep-copies; `map.eq(&other)` is
#      ORDER-INDEPENDENT (key-set + per-key value equality).
#   2. #[derive(Clone, Eq)] on a struct WITH a HashMap field — clone is deep
#      (mutating the clone's map leaves the original intact) and the derived eq
#      compares maps order-independently.
#   3. No-regression: a map-FREE derived `eq` stays PURE — comparing such a
#      struct inside a non-alloc fn still compiles (the alloc effect is added to
#      a derived eq ONLY when a field transitively contains a map).
# JIT + AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f" | tail -1)
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT exit $r"
}

# --- 1. direct HashMap Clone + Eq trait methods ---
cat > "$TMP/direct.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut a = hashmap_new();
    hashmap_insert(&mut a, "x", 1);
    hashmap_insert(&mut a, "y", 2);
    let b = a.clone();                       // deep copy via the Clone impl
    let mut c = hashmap_new();               // same entries, inserted in REVERSE
    hashmap_insert(&mut c, "y", 2);
    hashmap_insert(&mut c, "x", 1);
    let eq_ab = if a.eq(&b) { 1 } else { 0 };   // 1
    let eq_ac = if a.eq(&c) { 1 } else { 0 };   // 1 (order-independent)
    let mut d = a.clone();
    hashmap_insert(&mut d, "x", 99);            // perturb one value
    let eq_ad = if a.eq(&d) { 0 } else { 1 };   // 1 (differs)
    eq_ab * 100 + eq_ac * 10 + eq_ad            // 111
}
EOF
run direct "$TMP/direct.kd" 111

# --- 2. #[derive(Clone, Eq)] over a HashMap field ---
cat > "$TMP/derive.kd" <<'EOF'
#[derive(Clone, Eq)]
struct Config { name: String, limits: HashMap<String, i64> }
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, "cpu", 8);
    hashmap_insert(&mut m, "mem", 32);
    let c = Config { name: "prod", limits: m };
    let d = c.clone();                          // derived Clone clones the map
    let same = if c.eq(&d) { 1 } else { 0 };    // derived Eq (order-independent)
    let mut d2 = d.clone();
    hashmap_insert(&mut d2.limits, "cpu", 99);
    let diff = if c.eq(&d2) { 0 } else { 1 };   // deep clone => original intact
    let n = hashmap_len(&c.limits);             // 2
    same * 1000 + diff * 100 + n                // 1102
}
EOF
run derive "$TMP/derive.kd" 1102

# --- 3. no-regression: a map-free derived eq is PURE (callable from pure fn) ---
cat > "$TMP/pure.kd" <<'EOF'
#[derive(Eq)]
struct Point { x: i64, y: i64 }
fn same(a: &Point, b: &Point) -> bool { a.eq(b) }   // NO effect row — must compile
fn main() -> i64 {
    let p = Point { x: 1, y: 2 };
    let q = Point { x: 1, y: 2 };
    if same(&p, &q) { 7 } else { 0 }
}
EOF
run pure "$TMP/pure.kd" 7

echo "PASS: Phase 46 — generic Clone/Eq trait impls for HashMap + #[derive] over a map field (JIT + AOT)"
