#!/usr/bin/env bash
# Phase 6.0 smoke test: the built-in `print(i64) -> i64 ! { io }` writes
# one integer + newline to stdout. We verify both pipeline paths:
#   - JIT mode: kardc executes the program directly, capturing stdout.
#   - AOT mode: kardc emits a native binary; we run it and compare
#     stdout against the same expected output.
# Also confirms the typechecker rejects callers that don't declare the
# `io` effect.
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

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/print.kd" <<'EOF'
fn main() -> i64 ! { io } {
    print(42);
    print(123);
    print(0 - 7);
    0
}
EOF

EXPECTED=$'42\n123\n-7'

# JIT mode: kardc emits all printlines and then prints main's return
# (0) on a final line. We compare just the print output.
JIT_OUT=$("$KARDC" "$TMP/print.kd")
# The last line is main's return value (`0`) printed by kardc itself.
JIT_PRINT_OUT=$(head -n 3 <<< "$JIT_OUT")
if [[ "$JIT_PRINT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: JIT print output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$JIT_PRINT_OUT"
    exit 1
fi
echo "JIT print output matches"

# AOT mode: the binary itself writes the three lines to stdout.
"$KARDC" -o "$TMP/prog" "$TMP/print.kd"
AOT_OUT=$("$TMP/prog")
if [[ "$AOT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: AOT print output mismatch"
    echo "expected:"; echo "$EXPECTED"
    echo "got:"; echo "$AOT_OUT"
    exit 1
fi
echo "AOT print output matches"

# Pure caller without `! { io }`: must be a typecheck error.
cat > "$TMP/pure.kd" <<'EOF'
fn main() -> i64 { print(42); 0 }
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/pure.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: pure caller of print should be a typecheck error"
    exit 1
fi
if ! grep -q 'effect `io`' <<< "$ERR_OUT"; then
    echo 'FAIL: expected `io` effect-undeclared diagnostic, got:'
    echo "$ERR_OUT"
    exit 1
fi

echo "PASS: print() works in JIT + AOT, and the io effect is checked"
