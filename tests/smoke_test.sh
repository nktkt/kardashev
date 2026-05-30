#!/usr/bin/env bash
# Phase 1 smoke test: drive the kardashev REPL via stdin to define `fib`
# and then evaluate `fib(10)`, asserting the JIT'd native code returns 55.
# Supersedes the Phase 0 "kardc returns 42" check — `fib(10) == 55`
# exercises every component of the V1 pipeline (lex / parse / monotype
# HM typecheck / LLVM IR codegen / ORC v2 JIT / native execution).
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
    echo "  TEST_SRCDIR=${TEST_SRCDIR:-(unset)}"
    echo "  RUNFILES_DIR=${RUNFILES_DIR:-(unset)}"
    echo "  PWD=$(pwd)"
    find . -name kardc -type f 2>/dev/null | head -5
    exit 1
fi

echo "Using kardc at: $KARDC"

INPUT='fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } }
fib(10)
'

OUTPUT=$(printf '%s' "$INPUT" | "$KARDC")
echo "--- REPL output ---"
echo "$OUTPUT"
echo "-------------------"

if grep -qx '55' <<< "$OUTPUT"; then
    echo "PASS: kardashev REPL computed fib(10) == 55 via JIT"
    exit 0
fi

echo "FAIL: expected a line equal to '55' in REPL output"
exit 1
