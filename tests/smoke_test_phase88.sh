#!/usr/bin/env bash
# Phase 88 (Roadmap v15 — "self-hosting"): the FOUNDATION PROOF. A real lexer
# written IN kardashev (examples/selfhost/lexer.kd) tokenizes a kardashev source
# snippet — `fn add(a: i64, b: i64) -> i64 { a + b + 42 }` — into 9 identifiers,
# 1 number, 1 `->` operator, and 9 single-char punctuation tokens (whitespace
# skipped, multi-byte boundaries handled), with a position-weighted witness
# 9010109. Proves the front of the compiler is expressible in the language
# itself; later v15 phases grow it into a parser + checker toward a bootstrap.
# Built JIT + AOT, deterministic.
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

SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/selfhost/lexer.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/selfhost/lexer.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/lexer.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/selfhost/lexer.kd" \
    "examples/selfhost/lexer.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/lexer.kd not found"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

want=$'9\n1\n1\n9\n9010109'
# JIT: the four token counts + the witness, deterministic over several runs.
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [lexer/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [lexer/jit]: kardashev lexer -> 9 idents, 1 number, 1 arrow, 9 punct; witness 9010109"

# AOT: same counts, exit = 9010109 & 255 = 189.
"$KARDC" --no-cache -o "$TMP/lexer" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/lexer"); rc=$?; set -e
    [[ "$rc" -eq 189 && "$aout" == $'9\n1\n1\n9' ]] || { echo "FAIL [lexer/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [lexer/aot]: same token counts, exit 189 (= witness & 255), deterministic"

echo "ALL PHASE 88 SMOKE TESTS PASSED"
