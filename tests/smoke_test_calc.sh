#!/usr/bin/env bash
# Phase 26 smoke test: the calc capstone + the str_char_at builtin.
#
# A real "toolchain-flavored" program written in kardashev and compiled by
# the real kardc:
#   1. The calc interpreter (examples/calc/main.kd) — a recursive-descent
#      arithmetic expression interpreter. Tokenizes a string with str_char_at,
#      parses with correct precedence + parens (recursive descent returning
#      (value, pos, err) tuples), and evaluates. We build it AOT and JIT-run
#      it and assert the exact printed results for several expressions
#      including precedence, parens, division, whitespace, divide-by-zero,
#      and a malformed input:
#        "12 + 3 * (4 - 1)"  => 21   (precedence + parens)
#        "(1 + 2) * (3 + 4)" => 21   (parens override)
#        "100 / 5 / 2"       => 10   (left-associative division)
#        "2 * 3 + 4 * 5"     => 26   (precedence, no parens)
#        "7 + 0"             => 7    (whitespace handling)
#        "10 / 0"            => -2   (division by zero, recoverable)
#        "1 + "              => -1   (parse error, recoverable)
#      The process exit code is the first expression's result (21).
#   2. The str_char_at(&String, i) -> i64 builtin: in-bounds returns the byte
#      value (zero-extended), negative / past-the-end returns -1 (a clean EOF
#      sentinel). Bounds-checked like vec_get (no OOB read / crash). JIT + AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found in runfiles"
    exit 1
fi

# Locate the real calc capstone source. Under `make` it sits at
# examples/calc/main.kd from the repo root; under Bazel it is staged into the
# sh_test runfiles. We build the ACTUAL example file (not an inlined copy) so
# this test is also a build check of the shipped capstone.
CALC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/examples/calc/main.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/calc/main.kd" \
    "${RUNFILES_DIR:-}/_main/examples/calc/main.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/calc/main.kd" \
    "./examples/calc/main.kd"; do
    if [[ -n "$candidate" && -f "$candidate" ]]; then
        CALC="$candidate"
        break
    fi
done

if [[ -z "$CALC" ]]; then
    echo "FAIL: examples/calc/main.kd not found in runfiles"
    exit 1
fi

echo "Using kardc at: $KARDC"
echo "Using calc source at: $CALC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ===========================================================================
# 1. The calc capstone: JIT + AOT, exact output + exit code.
# ===========================================================================
# JIT: kardc prints each show() result line, then main's return value (21).
WANT_JIT=$'21\n21\n10\n26\n7\n-2\n-1\n21'
# AOT: the program prints the 7 result lines; the process exit code is 21.
WANT_AOT=$'21\n21\n10\n26\n7\n-2\n-1'

GOT_JIT=$("$KARDC" "$CALC")
if [[ "$GOT_JIT" != "$WANT_JIT" ]]; then
    echo "FAIL [calc-jit]: JIT output mismatch"
    echo "expected:"; echo "$WANT_JIT"
    echo "got:"; echo "$GOT_JIT"
    exit 1
fi
echo "PASS [calc-jit]: interpreter JIT-runs to 21/21/10/26/7/-2/-1 (+ ret 21)"

"$KARDC" -o "$TMP/calc" "$CALC"
set +e
GOT_AOT=$("$TMP/calc")
"$TMP/calc" > /dev/null
RC=$?
set -e
if [[ "$GOT_AOT" != "$WANT_AOT" ]]; then
    echo "FAIL [calc-aot]: AOT stdout mismatch"
    echo "expected:"; echo "$WANT_AOT"
    echo "got:"; echo "$GOT_AOT"
    exit 1
fi
if [[ "$RC" -ne 21 ]]; then
    echo "FAIL [calc-aot]: AOT exit code was $RC (expected 21)"
    exit 1
fi
echo "PASS [calc-aot]: interpreter AOT-runs to 21/21/10/26/7/-2/-1, exits 21"

# ===========================================================================
# 2. str_char_at: in-bounds byte values + past-end / negative -> -1.
# ===========================================================================
cat > "$TMP/charat.kd" <<'EOF'
fn main() -> i64 ! { io } {
    let s = "A0 z";              // bytes: 'A'=65 '0'=48 ' '=32 'z'=122
    print(str_char_at(&s, 0));   // 65
    print(str_char_at(&s, 1));   // 48
    print(str_char_at(&s, 2));   // 32 (space is a real byte, not OOB)
    print(str_char_at(&s, 3));   // 122
    print(str_char_at(&s, 4));   // past end -> -1
    print(str_char_at(&s, 0 - 5)); // negative -> -1
    str_char_at(&s, 100)         // way past end -> -1
}
EOF
# JIT prints the six print() lines, then kardc prints main's return (-1).
WANT_CHAR_JIT=$'65\n48\n32\n122\n-1\n-1\n-1'
GOT_CHAR_JIT=$("$KARDC" "$TMP/charat.kd")
if [[ "$GOT_CHAR_JIT" != "$WANT_CHAR_JIT" ]]; then
    echo "FAIL [charat-jit]: str_char_at JIT output mismatch"
    echo "expected:"; echo "$WANT_CHAR_JIT"
    echo "got:"; echo "$GOT_CHAR_JIT"
    exit 1
fi
# AOT: six lines printed; exit code = main's return (-1) truncated to u8 = 255.
"$KARDC" -o "$TMP/charat" "$TMP/charat.kd"
set +e
GOT_CHAR_AOT=$("$TMP/charat")
"$TMP/charat" > /dev/null
CHAR_RC=$?
set -e
WANT_CHAR_AOT=$'65\n48\n32\n122\n-1\n-1'
if [[ "$GOT_CHAR_AOT" != "$WANT_CHAR_AOT" ]]; then
    echo "FAIL [charat-aot]: str_char_at AOT output mismatch"
    echo "expected:"; echo "$WANT_CHAR_AOT"
    echo "got:"; echo "$GOT_CHAR_AOT"
    exit 1
fi
if [[ "$CHAR_RC" -ne 255 ]]; then
    echo "FAIL [charat-aot]: exit code was $CHAR_RC (expected 255 = -1 as u8)"
    exit 1
fi
echo "PASS [charat]: str_char_at reads bytes in-bounds, -1 past-end/negative (JIT + AOT)"

echo "PASS: calc interpreter + str_char_at builtin work in JIT + AOT"
