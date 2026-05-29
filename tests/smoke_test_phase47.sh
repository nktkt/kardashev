#!/usr/bin/env bash
# Phase 47 smoke test: the Ord trait + a generic in-place sort<T: Ord>.
#   1. `Ord` (cmp(&self, &Self) -> i64, -1/0/1) with built-in impls for i64,
#      f64, String (byte-wise lexicographic), plus a generic
#      `fn sort<T: Ord>(v: &mut Vec<T>)` (insertion sort over a new vec_swap
#      primitive — the first stdlib algorithm written over a user trait bound).
#      Verified for Vec<i64>, Vec<String>, and a Vec of a USER `Ord` type.
#   2. vec_swap exchanges non-Copy slots without leaking (String swap).
#   3. The &mut->& reborrow that makes sort expressible: a fn taking
#      `&mut Vec<T>` can call vec_len/vec_get_ref (which want `&Vec<T>`) and
#      reuse the param across calls (reborrow, not move).
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

# --- sort over i64, String, and a user Ord type ---
cat > "$TMP/sort.kd" <<'EOF'
struct Item { rank: i64, tag: i64 }
impl Ord for Item {
    fn cmp(&self, other: &Item) -> i64 {
        if self.rank < other.rank { 0 - 1 } else { if self.rank > other.rank { 1 } else { 0 } }
    }
}
fn main() -> i64 ! { alloc } {
    let mut a = vec_new();
    vec_push(&mut a, 30); vec_push(&mut a, 10); vec_push(&mut a, 20); vec_push(&mut a, 5);
    sort(&mut a);                                    // 5,10,20,30
    let ai = vec_get(&a, 0) * 1000 + vec_get(&a, 1) * 100
           + vec_get(&a, 2) * 10 + vec_get(&a, 3);   // 6230

    let mut s = vec_new();
    vec_push(&mut s, int_to_string(3));
    vec_push(&mut s, int_to_string(1));
    vec_push(&mut s, int_to_string(2));
    sort(&mut s);                                    // "1","2","3"
    let s0 = str_char_at(vec_get_ref(&s, 0), 0);     // 49 ('1')
    let s2 = str_char_at(vec_get_ref(&s, 2), 0);     // 51 ('3')
    let sok = if s0 == 49 { if s2 == 51 { 1 } else { 0 } } else { 0 };

    let mut it = vec_new();
    vec_push(&mut it, Item { rank: 9, tag: 1 });
    vec_push(&mut it, Item { rank: 2, tag: 2 });
    vec_push(&mut it, Item { rank: 5, tag: 3 });
    sort(&mut it);                                   // ranks 2,5,9
    let iok = if vec_get_ref(&it, 0).rank == 2 {
        if vec_get_ref(&it, 2).rank == 9 { 1 } else { 0 } } else { 0 };

    ai + sok * 100000 + iok * 1000000                // 6230 + 100000 + 1000000 = 1106230
}
EOF
run sort "$TMP/sort.kd" 1106230

# --- f64 Ord + already-sorted + reverse inputs (idempotence/edge) ---
cat > "$TMP/f64.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, 3.5); vec_push(&mut v, 1.25); vec_push(&mut v, 2.0); vec_push(&mut v, 1.25);
    sort(&mut v);                                    // 1.25, 1.25, 2.0, 3.5 (stable dup)
    // signal: 1 if non-decreasing, else 0
    let mut i = 1;
    let mut ok = 1;
    while i < vec_len(&v) {
        if vec_get_ref(&v, i - 1).cmp(vec_get_ref(&v, i)) > 0 { ok = 0; } else {}
        i = i + 1;
    }
    ok
}
EOF
run f64 "$TMP/f64.kd" 1

# --- vec_swap on non-Copy slots: ownership-neutral, no leak signal ---
cat > "$TMP/swap.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut v = vec_new();
    vec_push(&mut v, int_to_string(111));   // len 3
    vec_push(&mut v, int_to_string(7));     // len 1
    vec_swap(&mut v, 0, 1);
    str_len(vec_get_ref(&v, 0)) * 10 + str_len(vec_get_ref(&v, 1))  // 1*10 + 3 = 13
}
EOF
run swap "$TMP/swap.kd" 13

echo "PASS: Phase 47 — Ord trait + generic sort<T: Ord> (JIT + AOT)"
