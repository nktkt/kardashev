#!/usr/bin/env bash
# Phase 51 smoke test: Box<T> as a first-class impl target + deref ergonomics,
# closing the v8 deferral (trait Clone/Eq for Box).
#   1. `Box` is now a registrable impl target: the prelude ships
#      `impl<T: Clone> Clone for Box<T>` + `impl<T: Eq> Eq for Box<T>`, and
#      `box.clone()` / `box.eq(&other)` dispatch to the Box impl (NOT auto-deref
#      to the inner T). Proven for Box<i64> and Box<String>.
#   2. `#[derive(Clone, Eq)]` covers a `Box<T>` field — a recursive
#      `enum List { Nil, Cons(i64, Box<List>) }` clones its whole chain and
#      compares through the boxes, with the original intact (deep clone).
#   3. The `&*e` / `**e` deref ergonomics that make the Box Eq body expressible
#      (`&(**other)` lowers to the box pointer).
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

# --- 1. Box Clone/Eq trait methods (i64 + String) ---
cat > "$TMP/box.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let a: Box<i64> = Box::new(7);
    let b = a.clone();
    let same = if b.eq(&a) { 1 } else { 0 };          // 1
    let c: Box<i64> = Box::new(9);
    let diff = if a.eq(&c) { 0 } else { 1 };           // 1

    let s: Box<String> = Box::new(int_to_string(42));
    let s2 = s.clone();
    let sstr = if s.eq(&s2) { 1 } else { 0 };          // 1 (deep String compare)

    **(&b) * 1000 + same * 100 + diff * 10 + sstr      // 7000+100+10+1 = 7111
}
EOF
run box "$TMP/box.kd" 7111

# --- 2. #[derive(Clone, Eq)] over a Box<List> recursive field ---
cat > "$TMP/list.kd" <<'EOF'
#[derive(Clone, Eq)]
enum List { Nil, Cons(i64, Box<List>) }
fn len(l: &List) -> i64 { match l { Nil => 0, Cons(h, t) => 1 + len(&(**t)) } }
fn main() -> i64 ! { alloc } {
    let xs = Cons(1, Box::new(Cons(2, Box::new(Cons(3, Box::new(Nil))))));
    let ys = xs.clone();                         // clones the Box chain
    let same = if xs.eq(&ys) { 1 } else { 0 };
    let zs = Cons(1, Box::new(Cons(9, Box::new(Nil))));
    let diff = if xs.eq(&zs) { 0 } else { 1 };
    let n = len(&xs);                            // 3 — original intact after clone
    same * 1000 + diff * 100 + n                 // 1103
}
EOF
run list "$TMP/list.kd" 1103

echo "PASS: Phase 51 — Box<T> impl target + deref ergonomics + Box Clone/Eq (JIT + AOT)"
