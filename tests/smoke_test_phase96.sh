#!/usr/bin/env bash
# Phase 96 (Roadmap v16 — "self-hosting, cont."): a scope/semantic CHECKER over
# the body grammar, written in kardashev (examples/selfhost/scopechk.kd). It
# parses a block and walks the AST verifying every VARIABLE REFERENCE is bound —
# an outer parameter or an earlier `let` — reporting UNDEFINED variables (a let
# RHS is checked BEFORE its own name binds; each let extends the scope). With
# params {a, b}: `let x = a + 1 ; x + b` -> 0 undefined; `let x = a + 1 ; x + c`
# -> 1 undefined (c). witness 1. Built JIT + AOT.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/scopechk.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/scopechk.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/scopechk.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/scopechk.kd" \
    "examples/selfhost/scopechk.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/scopechk.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
want=$'0\n1\n1'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [scopechk/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [scopechk/jit]: well-scoped block -> 0 undefined; undeclared var c -> 1 undefined"
"$KARDC" --no-cache -o "$TMP/sc" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/sc"); rc=$?; set -e
    [[ "$rc" -eq 1 && "$aout" == $'0\n1' ]] || { echo "FAIL [scopechk/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [scopechk/aot]: same scope analysis, exit 1, deterministic"
echo "ALL PHASE 96 SMOKE TESTS PASSED"
