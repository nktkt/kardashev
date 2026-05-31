#!/usr/bin/env bash
# v27 Phase 148: UTF-8 correctness. A String stores UTF-8 BYTES; these add the
# char-aware layer over the byte builtins: str_char_width_at (lead byte -> 1-4),
# str_decode_char_at (decode the scalar at a byte offset), str_char_count (chars,
# not bytes), string_chars (-> Vec<char>), str_is_valid_utf8 (structural check).
# JIT + AOT.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
run_eq() { local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq $(( $3 & 255 )) ]] || { echo "FAIL [$1/aot]: exit $rc want $(( $3 & 255 ))"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) char count != byte length for multibyte text
cat > "$TMP/cc.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut s = string_new();
    str_push_char(&mut s, 'h');                    // 1 byte
    str_push_char(&mut s, 'i');                    // 1 byte
    str_push_char(&mut s, char_from_u32(0x1F600)); // 4 bytes
    str_push_char(&mut s, char_from_u32(0xE9));    // 2 bytes (é)
    str_len(&s) * 100 + str_char_count(&s)         // 8 bytes, 4 chars -> 804
}
EOF
run_eq count "$TMP/cc.kd" 804 "str_len=8 bytes, str_char_count=4 chars -> 804"

# 2) string_chars iterates Unicode scalars (not bytes)
cat > "$TMP/it.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut s = string_new();
    str_push_char(&mut s, 'A');
    str_push_char(&mut s, char_from_u32(0x1F600));
    let chars = string_chars(&s);
    let mut total = 0; let mut i = 0;
    while i < vec_len(&chars) { total = total + (vec_get(&chars, i) as i64); i = i + 1; }
    vec_len(&chars) * 1000000 + total              // 2 chars, sum 128577
}
EOF
run_eq chars "$TMP/it.kd" 2128577 "string_chars -> 2 scalars, codepoint sum 128577"

# 3) decode at a byte offset round-trips the encoded scalar
cat > "$TMP/rt.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut s = string_new();
    str_push_char(&mut s, char_from_u32(0x1F600));
    let w = str_char_width_at(&s, 0);
    str_decode_char_at(&s, 0) as i64 * 10 + w       // 128512*10 + 4
}
EOF
run_eq decode "$TMP/rt.kd" 1285124 "str_decode_char_at + str_char_width_at round-trip (emoji, width 4)"

# 4) validity: well-formed UTF-8 accepted, a stray 0xFF rejected
cat > "$TMP/v.kd" <<'EOF'
fn main() -> i64 ! { alloc } {
    let mut good = string_new();
    str_push_char(&mut good, char_from_u32(0x1F600));
    str_push_char(&mut good, char_from_u32(0xE9));
    let mut bad = string_new();
    str_push_byte(&mut bad, 0xFF);                  // not a valid lead byte
    let mut trunc = string_new();
    str_push_byte(&mut trunc, 0xE2);                // a 3-byte lead with no continuation
    let g = if str_is_valid_utf8(&good) { 1 } else { 0 };
    let b = if str_is_valid_utf8(&bad) { 1 } else { 0 };
    let t = if str_is_valid_utf8(&trunc) { 1 } else { 0 };
    g * 100 + b * 10 + t                             // good=1, bad=0, trunc=0 -> 100
}
EOF
run_eq valid "$TMP/v.kd" 100 "str_is_valid_utf8: well-formed=1, 0xFF=0, truncated lead=0"

echo "PASS: Phase 148 — UTF-8 char iteration / indexing / validation"
