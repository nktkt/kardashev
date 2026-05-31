#!/usr/bin/env bash
# Phase 102 (Roadmap v17 — "self-hosting, cont."): a whole-FUNCTION type checker
# in kardashev (examples/selfhost/funcheck.kd). Threads the Phase-101 expression
# type checker through an entire `fn NAME(PARAMS) -> RET { LETS ; RESULT }`: it
# parses params WITH their declared types (i64/bool), builds a type environment,
# type-checks each `let` (binding the RHS's inferred type, rejecting an ill-typed
# one), and requires the body's result type to EQUAL the declared return type.
# Checked: a well-typed i64 fn (->1), a well-typed bool fn (->1), a body/return
# MISMATCH (->0), and a let that makes the body ill-typed (->0). Output 1/1/0/0;
# witness r1*1000+r2*100+r3*10+r4 = 1100 (AOT exit 1100 & 255 = 76).
set -euo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"
SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/selfhost/funcheck.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/funcheck.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/funcheck.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/funcheck.kd" \
    "examples/selfhost/funcheck.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/funcheck.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

want=$'1\n1\n0\n0\n1100'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [funcheck/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [funcheck/jit]: whole-fn type checker — i64 fn ok, bool fn ok, ret mismatch rejected, ill-typed let rejected"

"$KARDC" --no-cache -o "$TMP/funcheck" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/funcheck"); rc=$?; set -e
    [[ "$rc" -eq 76 && "$aout" == $'1\n1\n0\n0' ]] || { echo "FAIL [funcheck/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [funcheck/aot]: same whole-function type-checking, exit 76 (=1100 & 255), deterministic"
echo "ALL PHASE 102 SMOKE TESTS PASSED"
