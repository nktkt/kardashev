#!/usr/bin/env bash
# Phase 35 smoke test: sound recursive heap enums + the `clone` deep-copy.
#   1. A recursive `enum Tree { Leaf(i64), Node(Vec<Tree>) }` is built, cloned,
#      match-&-traversed, and dropped — the clone is independent (deep copy):
#      sum(original) == sum(clone), and both drop with no double-free.
#   2. clone is a genuine DEEP copy: a Vec<String> and the original share no
#      heap buffer (both readable after the clone; both freed once).
#   3. A 1000-node-deep linear tree clones + drops without corruption (the
#      drop/clone recursion happens at run time, so deep nesting is fine).
#   4. HashMap<String,i64> clone is deep (keys re-allocated).
#   5. Constant memory: build + clone + match-by-ref + drop a recursive
#      heap-String tree in a 200k-iteration loop — peak RSS stays flat.
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

# Portable peak-RSS (KB): GNU `time -v` (Linux) or BSD `time -l` (macOS); empty
# if neither exists (caller SKIPs the gate). Safe under `set -e`/`pipefail`.
peak_rss_kb() {
    local f; f=$(mktemp)
    if /usr/bin/time -v true >/dev/null 2>&1; then
        { /usr/bin/time -v "$@" >/dev/null; } 2>"$f" || true
        awk '/Maximum resident set size/ {print $NF}' "$f"
    elif /usr/bin/time -l true >/dev/null 2>&1; then
        { /usr/bin/time -l "$@" >/dev/null; } 2>"$f" || true
        awk '/maximum resident set size/ {print int($1/1024)}' "$f"
    fi
    rm -f "$f"
}

check() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    # AOT exit code is the i64 result mod 256; compare to (w mod 256).
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT exit matches"
}

# --- 1. recursive Tree: clone is an independent deep copy ---
cat > "$TMP/tree.kd" <<'EOF'
enum Tree { Leaf(i64), Node(Vec<Tree>) }
fn build(depth: i64, seed: i64) -> Tree ! { alloc } {
    if depth <= 0 { Leaf(seed) }
    else {
        let mut k = vec_new();
        vec_push(&mut k, build(depth - 1, seed * 2));
        vec_push(&mut k, build(depth - 1, seed * 2 + 1));
        Node(k)
    }
}
fn sum(t: &Tree) -> i64 ! { alloc } {
    match t {
        Leaf(n) => *n,
        Node(kids) => {
            let mut i = 0; let mut s = 0; let len = vec_len(kids);
            while i < len { s = s + sum(vec_get_ref(kids, i)); i = i + 1; } s
        }
    }
}
fn main() -> i64 ! { alloc } {
    let t = build(3, 1);            // leaves 8..15 -> sum 92
    let c = clone(&t);              // deep clone
    let st = sum(&t);
    let sc = sum(&c);               // identical; t and c each dropped once
    st + sc                          // 184
}
EOF
check tree "$TMP/tree.kd" 184

# --- 2. clone is DEEP: Vec<String> original + clone both valid, no shared buf ---
cat > "$TMP/deep.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, int_to_string(111));     // len 3
    vec_push(&mut v, int_to_string(2222));    // len 4
    let c = clone(&v);                         // deep copy
    let a = str_len(vec_get_ref(&v, 0)) + str_len(vec_get_ref(&v, 1));  // 7
    let b = str_len(vec_get_ref(&c, 0)) + str_len(vec_get_ref(&c, 1));  // 7
    a + b                                       // 14 (both freed once)
}
EOF
check deep "$TMP/deep.kd" 14

# --- 3. a 1000-node-deep linear tree clones + drops without corruption ---
cat > "$TMP/deeptree.kd" <<'EOF'
enum Tree { Leaf(i64), Node(Vec<Tree>) }
fn chain(depth: i64) -> Tree ! { alloc } {
    if depth <= 0 { Leaf(1) }
    else { let mut k = vec_new(); vec_push(&mut k, chain(depth - 1)); Node(k) }
}
fn count(t: &Tree) -> i64 ! { alloc } {
    match t {
        Leaf(n) => 1,
        Node(kids) => {
            let mut i = 0; let mut c = 0; let len = vec_len(kids);
            while i < len { c = c + count(vec_get_ref(kids, i)); i = i + 1; } c
        }
    }
}
fn main() -> i64 ! { alloc } {
    let t = chain(1000);            // 1000 Nodes + 1 Leaf
    let c = clone(&t);              // deep clone of a 1000-deep tree
    count(&t) + count(&c)            // 1 + 1 = 2 leaves total
}
EOF
check deeptree "$TMP/deeptree.kd" 2

# --- 4. HashMap<String,i64> clone is deep (keys re-allocated) ---
cat > "$TMP/map.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut m = hashmap_new();
    hashmap_insert(&mut m, int_to_string(7), 70);
    hashmap_insert(&mut m, int_to_string(8), 80);
    let c = clone(&m);                              // deep clone (keys copied)
    let a = match hashmap_get(&m, int_to_string(7)) { Some(x) => x, None => 0 };
    let b = match hashmap_get(&c, int_to_string(8)) { Some(x) => x, None => 0 };
    a + b                                            // 70 + 80 = 150
}
EOF
check map "$TMP/map.kd" 150

# --- 5. constant memory: build + clone + match-by-ref + drop in a loop ---
cat > "$TMP/loop.kd" <<'EOF'
enum Tree { Leaf(String), Node(Vec<Tree>) }
fn build(depth: i64, seed: i64) -> Tree ! { alloc } {
    if depth <= 0 { Leaf(int_to_string(seed)) }
    else {
        let mut k = vec_new();
        vec_push(&mut k, build(depth - 1, seed * 2));
        vec_push(&mut k, build(depth - 1, seed * 2 + 1));
        Node(k)
    }
}
fn count(t: &Tree) -> i64 ! { alloc } {
    match t {
        Leaf(s) => str_len(s),
        Node(kids) => {
            let mut i = 0; let mut c = 0; let len = vec_len(kids);
            while i < len { c = c + count(vec_get_ref(kids, i)); i = i + 1; } c
        }
    }
}
fn main() -> i64 ! { alloc } {
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        let t = build(3, i);          // heap-String tree
        let c = clone(&t);            // deep clone
        acc = acc + count(&t) + count(&c);   // match BOTH by reference
        i = i + 1;                     // t and c both dropped each iter
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$(peak_rss_kb "$TMP/loop")
if [[ -z "$rss" ]]; then
    echo "SKIP [loop]: no GNU/BSD /usr/bin/time available for the RSS gate"
else
    echo "INFO [loop]: peak RSS over 200k build+clone+match+drop = ${rss} KB"
    if [[ "$rss" -gt 32768 ]]; then
        echo "FAIL [loop]: RSS ${rss} KB > 32 MB — clone/drop leaked or shared a buffer"; exit 1
    fi
    echo "PASS [loop]: recursive clone + drop — RSS flat (<= 32 MB)"
fi

echo "PASS: Phase 35 — sound recursive heap enums + deep clone (JIT + AOT)"
