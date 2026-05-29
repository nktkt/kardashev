#!/usr/bin/env bash
# Phase 54 smoke test: string tokenizing (str_split / str_trim), written in
# kardashev over str_char_at / str_substring / str_len.
#   1. str_split("a,bb,,c", ',') -> ["a","bb","","c"] (4 pieces; an empty piece
#      between adjacent separators is kept). str_trim("  hi  ") -> "hi".
#   2. A constant-memory gate: a 200k split+trim+drop loop holds peak RSS flat
#      (each piece Vec + its String elements drop every round).
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

# --- 1. split + trim correctness ---
cat > "$TMP/tok.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let input = "a,bb,,c";
    let parts = str_split(&input, 44);                 // ',' -> ["a","bb","","c"]
    let nparts = vec_len(&parts);                      // 4
    let p1len = str_len(vec_get_ref(&parts, 1));       // 2
    let p2len = str_len(vec_get_ref(&parts, 2));       // 0 (empty)
    let padded = "  hi  ";
    let trimmed = str_trim(&padded);                   // "hi"
    let tlen = str_len(&trimmed);                      // 2
    nparts * 1000 + p1len * 100 + p2len * 10 + tlen    // 4202
}
EOF
jit=$("$KARDC" "$TMP/tok.kd" | tail -1)
[[ "$jit" == "4202" ]] || { echo "FAIL [tok/jit]: expected 4202 got $jit"; exit 1; }
"$KARDC" --no-cache -o "$TMP/tok" "$TMP/tok.kd" >/dev/null
set +e; "$TMP/tok" >/dev/null; rc=$?; set -e
exp=$((4202 % 256))
[[ "$rc" -ne "$exp" ]] && { echo "FAIL [tok/aot]: exit $rc expected $exp"; exit 1; }
echo "PASS [split/trim]: JIT 4202, AOT exit $rc"

# --- 2. constant memory over a split+trim loop ---
cat > "$TMP/loop.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let input = "  the quick,brown ,fox,, jumps  ";
    let mut i = 0;
    let mut acc = 0;
    while i < 200000 {
        let t = str_trim(&input);
        let parts = str_split(&t, 44);
        acc = acc + vec_len(&parts);
        i = i + 1;
    }
    if acc > 0 { 0 } else { 1 }
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null
rss=$( /usr/bin/time -v "$TMP/loop" 2>&1 | awk '/Maximum resident/ {print $NF}' )
echo "INFO [loop]: peak RSS over 200k split+trim+drop = ${rss} KB"
if [[ -n "$rss" && "$rss" -gt 32768 ]]; then
    echo "FAIL [loop]: RSS ${rss} KB > 32 MB — tokenizing leaks"; exit 1
fi
echo "PASS [loop]: split+trim+drop — RSS flat (<= 32 MB)"

echo "PASS: Phase 54 — string tokenizing str_split/str_trim (JIT + AOT)"
