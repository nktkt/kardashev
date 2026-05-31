#!/usr/bin/env bash
# v27 Phase 151: char classification + string encode helpers. char_is_digit /
# _alpha / _alnum / _whitespace + char_to_upper / _to_lower (ASCII-correct);
# str_join (Vec<String> + separator), str_replace (byte-sequence replace),
# str_lines (split on '\n'). Grapheme-cluster segmentation (UAX #29) is a
# documented future item — scalar-level iteration (str_char_count / string_chars,
# Phase 148) is what's provided. JIT + AOT.
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
out_eq() { "$KARDC" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/jit]: run"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    local aot; aot=$("$TMP/b" 2>&1)
    [[ "$aot" == "$3" ]] || { echo "FAIL [$1/aot]: want '$3' got '$aot'"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) char classification predicates
cat > "$TMP/c.kd" <<'EOF'
fn b(x: bool) -> i64 { if x { 1 } else { 0 } }
fn main() -> i64 {
    b(char_is_digit('7')) * 100000
    + b(char_is_digit('a')) * 10000
    + b(char_is_alpha('Q')) * 1000
    + b(char_is_alnum('_')) * 100
    + b(char_is_whitespace('\t')) * 10
    + b(char_is_whitespace('x'))
}
EOF
run_eq classify "$TMP/c.kd" 101010 "is_digit/is_alpha/is_alnum/is_whitespace (1,0,1,0,1,0)"

# 2) char case mapping (ASCII)
cat > "$TMP/m.kd" <<'EOF'
fn main() -> i64 {
    (char_to_upper('a') as i64) * 1000 + (char_to_lower('Z') as i64)
}
EOF
run_eq case "$TMP/m.kd" 65122 "char_to_upper('a')='A'(65), char_to_lower('Z')='z'(122)"

# 3) str_join
cat > "$TMP/j.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut parts = vec_new();
    vec_push(&mut parts, clone(&"alpha"));
    vec_push(&mut parts, clone(&"beta"));
    vec_push(&mut parts, clone(&"gamma"));
    print_no_nl(&str_join(&parts, &", "));
    0
}
EOF
out_eq join "$TMP/j.kd" "alpha, beta, gamma" "str_join(Vec<String>, sep)"

# 4) str_replace (multi-char needle, byte-sequence)
cat > "$TMP/r.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    print_no_nl(&str_replace(&"one and two and three", &" and ", &" + "));
    0
}
EOF
out_eq replace "$TMP/r.kd" "one + two + three" "str_replace replaces every occurrence"

# 5) str_lines
cat > "$TMP/l.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let lines = str_lines(&"a\nbb\nccc");
    let mut total = 0; let mut i = 0;
    while i < vec_len(&lines) { total = total + str_len(vec_get_ref(&lines, i)); i = i + 1; }
    vec_len(&lines) * 100 + total
}
EOF
run_eq lines "$TMP/l.kd" 306 "str_lines -> 3 lines, total bytes 1+2+3=6 -> 306"

echo "PASS: Phase 151 — char classification + string encode helpers"
