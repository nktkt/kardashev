#!/usr/bin/env bash
# Phase 72 smoke test (Roadmap v12 — the real stdlib): String methods —
# str_starts_with / str_ends_with / str_contains / str_index_of (pure reads) and
# str_to_upper / str_to_lower / str_concat / str_repeat (fresh heap Strings).
# All are kardashev prelude functions over str_char_at / str_len / str_push_byte.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" \
    "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/s.kd" <<'EOF'
fn b2i(b: bool) -> i64 { if b { 1 } else { 0 } }
fn main() -> i64 ! { io, alloc } {
    let s = "Hello, World";
    let h = "Hello"; let w = "World"; let z = "xyz"; let cm = "lo, W";
    print(b2i(str_starts_with(&s, &h)));   // 1
    print(b2i(str_starts_with(&s, &w)));   // 0
    print(b2i(str_ends_with(&s, &w)));     // 1
    print(b2i(str_ends_with(&s, &h)));     // 0
    print(b2i(str_contains(&s, &cm)));     // 1
    print(b2i(str_contains(&s, &z)));      // 0
    print(str_index_of(&s, &w));           // 7
    print(str_index_of(&s, &z));           // -1
    let u = str_to_upper(&s); print_str(&u);   // HELLO, WORLD
    let l = str_to_lower(&s); print_str(&l);   // hello, world
    let c = str_concat(&h, &w); print_str(&c); // HelloWorld
    let ab = "ab"; let r = str_repeat(&ab, 3); print_str(&r);  // ababab
    0
}
EOF
want=$'1\n0\n1\n0\n1\n0\n7\n-1\nHELLO, WORLD\nhello, world\nHelloWorld\nababab'
got=$("$KARDC" "$TMP/s.kd" 2>/dev/null | head -12)
[[ "$got" == "$want" ]] || { echo "FAIL [str/jit]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
echo "PASS [str/jit]: starts/ends/contains/index_of + upper/lower/concat/repeat"

"$KARDC" --no-cache -o "$TMP/s" "$TMP/s.kd" >/dev/null 2>&1
aot=$("$TMP/s")
[[ "$aot" == "$want" ]] || { echo "FAIL [str/aot]:"; diff <(echo "$want") <(echo "$aot"); exit 1; }
echo "PASS [str/aot]: same transcript AOT-compiled"

echo "ALL PHASE 72 SMOKE TESTS PASSED"
