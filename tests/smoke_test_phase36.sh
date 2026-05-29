#!/usr/bin/env bash
# Phase 36 smoke test: enum-typed struct fields + non-Copy aggregate (tuple)
# elements + tuple `match` patterns + Result errors.
#   1. A struct with a recursive-ENUM field is built and its field is read by
#      reference (`&h.t`) and matched.
#   2. A fn returns a non-Copy tuple `(String, i64)`; it is destructured with a
#      tuple pattern, its String read, and dropped exactly once.
#   3. `match` on a tuple: Copy, wildcard, and an enum nested in a tuple.
#   4. A parser returning `Result<Json, ParseError>` propagates errors via `?`.
#   5. Constant memory: a tuple-destructure (moving a String out) in a loop —
#      no leak, no double-free.
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

check() { # name file expected
    local n=$1 f=$2 w=$3 jit
    jit=$("$KARDC" "$f")
    [[ "$jit" != "$w" ]] && { echo "FAIL [$n/jit]: expected $w got $jit"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$n" "$f" >/dev/null
    set +e; "$TMP/$n" >/dev/null; local r=$?; set -e
    local wm=$(( ( (w % 256) + 256 ) % 256 ))
    [[ "$r" -ne "$wm" ]] && { echo "FAIL [$n/aot]: exit $r expected $wm"; exit 1; }
    echo "PASS [$n]: JIT=$jit, AOT matches"
}

# --- 1. struct with a recursive-enum field; read it by reference ---
cat > "$TMP/field.kd" <<'EOF'
enum Tree { Leaf(i64), Node(Vec<Tree>) }
struct Holder { t: Tree, tag: i64 }
fn val(t: &Tree) -> i64 ! { alloc } {
    match t { Leaf(n) => *n, Node(k) => vec_len(k) }
}
fn main() -> i64 ! { alloc } {
    let mut k = vec_new();
    vec_push(&mut k, Leaf(1));
    vec_push(&mut k, Leaf(2));
    let h = Holder { t: Node(k), tag: 40 };
    val(&h.t) + h.tag                       // vec_len 2 + 40 = 42
}
EOF
check field "$TMP/field.kd" 42

# --- 2. fn returning a non-Copy tuple (String, i64), destructured ---
cat > "$TMP/tup.kd" <<'EOF'
fn split() -> (String, i64) ! { alloc } { (int_to_string(7777), 9) }
fn main() -> i64 ! { alloc } {
    match split() { (s, n) => str_len(&s) + n }   // 4 + 9 = 13; s dropped once
}
EOF
check tup "$TMP/tup.kd" 13

# --- 3. tuple match: copy, wildcard, enum-in-tuple ---
cat > "$TMP/match.kd" <<'EOF'
enum E { A(i64), B }
fn main() -> i64 {
    let p = (3, 4);
    let q = (A(7), 100);
    let r = match p { (a, b) => a + b };               // 7
    let s = match q { (A(x), y) => x + y, (B, y) => y }; // 107
    let t = match (1, 9) { (_, y) => y };              // 9
    r + s + t                                            // 123
}
EOF
check match "$TMP/match.kd" 123

# --- 4. Result<_, ParseError> with `?` propagation ---
cat > "$TMP/result.kd" <<'EOF'
enum ParseError { Empty, Bad }
fn digit(c: i64) -> Result<i64, ParseError> {
    if c < 48 { Err(Bad) } else { if c > 57 { Err(Bad) } else { Ok(c - 48) } }
}
fn two(a: i64, b: i64) -> Result<i64, ParseError> {
    let x = digit(a)?;
    let y = digit(b)?;
    Ok(x * 10 + y)
}
fn main() -> i64 {
    let ok = match two(53, 55) { Ok(n) => n, Err(e) => -1 };   // '5''7' -> 57
    let bad = match two(53, 99) { Ok(n) => n, Err(e) => -1 };  // bad 2nd -> -1
    ok + bad                                                    // 57 + -1 = 56
}
EOF
check result "$TMP/result.kd" 56

# --- 5. constant memory: tuple destructure (String moved out) drops once ---
cat > "$TMP/loop.kd" <<'EOF'
fn mk(i: i64) -> (String, i64) ! { alloc } { (int_to_string(i), i) }
fn main() -> i64 ! { alloc } {
    let mut i = 0; let mut acc = 0;
    while i < 300000 {
        match mk(i) { (s, n) => { acc = acc + str_len(&s) + n; } }
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$( /usr/bin/time -v "$TMP/loop" 2>&1 | awk '/Maximum resident/ {print $NF}' )
echo "INFO [loop]: peak RSS over 300k tuple-destructure+drop = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [loop]: RSS ${rss} KB > 32 MB — tuple element leaked or double-freed"; exit 1
fi
echo "PASS [loop]: non-Copy tuple destructure drops exactly once — RSS flat"

echo "PASS: Phase 36 — enum-typed struct fields + non-Copy tuples + tuple match + Result (JIT + AOT)"
