#!/usr/bin/env bash
# Phase 98 (Roadmap v17 — "self-hosting, cont."): a WHOLE-FUNCTION parser +
# interpreter in kardashev (examples/selfhost/func.kd). Unifies v15's signature
# parser and v16's body interpreter: it parses a complete
# `fn NAME ( PARAMS ) -> RET { BODY }` into an `Fn { name, params, ret, body }`
# AST and interprets it — scope-check the body against the parameters, bind the
# call arguments, evaluate the body. Over `fn f(a: i64, b: i64) -> i64 { let x =
# a + b ; x * 2 }` called with (3, 4): 2 params, x=7, x*2=14; witness 214.
# Built JIT + AOT.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/func.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/func.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/func.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/func.kd" \
    "examples/selfhost/func.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/func.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
want=$'2\n14\n214'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [func/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [func/jit]: parse + interpret whole fn f(a,b){let x=a+b; x*2}; f(3,4)=14 (2 params)"
"$KARDC" --no-cache -o "$TMP/func" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/func"); rc=$?; set -e
    [[ "$rc" -eq 214 && "$aout" == $'2\n14' ]] || { echo "FAIL [func/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [func/aot]: same whole-function interpretation, exit 214, deterministic"
echo "ALL PHASE 98 SMOKE TESTS PASSED"
