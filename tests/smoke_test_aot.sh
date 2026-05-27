#!/usr/bin/env bash
# Phase 5a smoke test: AOT-compile a kardashev program to a native
# executable and verify the produced binary returns the right exit code
# when run. Exercises the full pipeline: parse / typecheck / borrow-
# check / effect-check / codegen / object emission / clang link.
#
# fib(10) == 55. We pick a value < 128 so it fits in a process exit code
# byte (POSIX caps return-via-exit at 8 bits unsigned).
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

cat > "$TMP/fib.kd" <<'EOF'
fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } }
fn main() -> i64 { fib(10) }
EOF

"$KARDC" -o "$TMP/fib" "$TMP/fib.kd"
if [[ ! -x "$TMP/fib" ]]; then
    echo "FAIL: AOT did not produce an executable"
    exit 1
fi

# Run the AOT binary and capture its exit code. POSIX truncates the
# exit status to 8 bits unsigned, so we expect 55 back from main().
set +e
"$TMP/fib"
rc=$?
set -e

if [[ "$rc" -ne 55 ]]; then
    echo "FAIL: AOT executable returned $rc (expected 55)"
    exit 1
fi

echo "PASS: AOT fib(10) executable returned exit code 55"
