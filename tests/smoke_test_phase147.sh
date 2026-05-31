#!/usr/bin/env bash
# v27 Phase 147: a real `char` type (Unicode scalar). Char literals `'a'` with
# escapes (`\n \t \\ \' \u{...}`); the `char` type lowers to an i32 codepoint;
# equality/ordering (NO arithmetic); `char as i64` / `i64 as char` casts; char
# literal patterns in `match`; and the char<->string bridges (`char_to_string`
# UTF-8-encodes, `char_from_u32` validates, `str_push_char`, `print_char`). JIT
# + AOT.
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
expect_err() { local out; out=$("$KARDC" "$2" 2>&1)
    [[ $? -ne 0 ]] || { echo "FAIL [$1]: expected error, compiled"; exit 1; }
    echo "$out" | grep -qiE "$3" || { echo "FAIL [$1]: want /$3/, got: $out"; exit 1; }
    echo "PASS [$1]: $4"; }
out_eq() { local got; got=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$got" == "$3" ]] || { echo "FAIL [$1/jit]: want '$3' got '$got'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    local aot; aot=$("$TMP/b" 2>&1)
    [[ "$aot" == "$3" ]] || { echo "FAIL [$1/aot]: want '$3' got '$aot'"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) char literal + cast to codepoint
cat > "$TMP/a.kd" <<'EOF'
fn main() -> i64 { 'A' as i64 }
EOF
run_eq cast "$TMP/a.kd" 65 "'A' as i64 == 65"

# 2) ordering + equality
cat > "$TMP/o.kd" <<'EOF'
fn main() -> i64 {
    let a = 'a'; let z = 'z';
    let lt = if a < z { 1 } else { 0 };
    let eq = if a == 'a' { 1 } else { 0 };
    lt * 10 + eq
}
EOF
run_eq order "$TMP/o.kd" 11 "char ordering + equality (a<z, a=='a')"

# 3) escapes + \u{...}
cat > "$TMP/e.kd" <<'EOF'
fn main() -> i64 { let nl = '\n'; let smile = '\u{1F600}'; nl as i64 * 1000000 + smile as i64 }
EOF
run_eq escapes "$TMP/e.kd" 10128512 "'\\n'==10, '\\u{1F600}'==128512"

# 4) char literal patterns in match (exhaustive with `_`)
cat > "$TMP/m.kd" <<'EOF'
fn classify(c: char) -> i64 { match c { 'a' => 1, 'b' => 2, _ => 99 } }
fn main() -> i64 { classify('a') * 100 + classify('b') * 10 + classify('q') }
EOF
run_eq match "$TMP/m.kd" 219 "char match: a/b/_ dispatch == 219"

# 5) char_from_u32 validates (out-of-range / surrogate -> U+FFFD)
cat > "$TMP/v.kd" <<'EOF'
fn main() -> i64 {
    let bad = char_from_u32(0x110000) as i64;     // > max -> FFFD
    let sur = char_from_u32(0xD800) as i64;        // surrogate -> FFFD
    let ok  = char_from_u32(0x41) as i64;          // 'A'
    bad * 1000000 + sur * 1000 + ok
}
EOF
# 65533*1000000 + 65533*1000 + 65 = 65598533065 (fits i64).
run_eq from_u32 "$TMP/v.kd" 65598533065 "char_from_u32 validates surrogate/oob -> U+FFFD, 0x41 -> 'A'"

# 6) char_to_string + print_char UTF-8-encode (multi-byte)
cat > "$TMP/s.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    print_char('H'); print_char('i'); print_char(char_from_u32(0x1F600));
    let s = char_to_string('Z'); print_str(&s);
    0
}
EOF
out_eq encode "$TMP/s.kd" "Hi😀Z" "print_char/char_to_string UTF-8-encode (ASCII + 4-byte emoji)"

# 7) arithmetic on char rejected
cat > "$TMP/bad.kd" <<'EOF'
fn main() -> i64 { let x = 'a' + 'b'; 0 }
EOF
expect_err arith "$TMP/bad.kd" "not defined for .char." "char arithmetic is rejected"

# 8) char/int type mismatch rejected
cat > "$TMP/mm.kd" <<'EOF'
fn main() -> i64 { if 'a' == 97 { 1 } else { 0 } }
EOF
expect_err mismatch "$TMP/mm.kd" "char|comparison" "char vs int comparison is rejected (cast needed)"

echo "PASS: Phase 147 — the char type + UTF-8 char<->string bridges"
