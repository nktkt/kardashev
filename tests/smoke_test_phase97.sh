#!/usr/bin/env bash
# Phase 97 CAPSTONE (Roadmap v16 — "self-hosting, cont."): a function-body
# INTERPRETER written in kardashev (examples/selfhost/interp.kd). It ties the
# whole body pipeline it built across Phases 94-96 — lex -> parse a block ->
# SCOPE-CHECK -> EVALUATE — into one interpret(body, params, args): it rejects a
# body referencing an undefined variable (-1), else binds args to params and
# runs the block. Body `let sq = x*x ; let dbl = y+y ; sq + dbl` with x=3,y=4 ->
# 17; ill-scoped `... sq + z` -> -1; witness 1701. Built JIT + AOT.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/interp.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/interp.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/interp.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/interp.kd" \
    "examples/selfhost/interp.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/interp.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
want=$'17\n-1\n1701'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [interp/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [interp/jit]: body interpreter — f(x=3,y=4){sq=x*x; dbl=y+y; sq+dbl}=17; undefined-var body rejected (-1)"
"$KARDC" --no-cache -o "$TMP/in" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/in"); rc=$?; set -e
    [[ "$rc" -eq 165 && "$aout" == $'17\n-1' ]] || { echo "FAIL [interp/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [interp/aot]: same interpretation, exit 165 (= 1701 & 255), deterministic"
echo "ALL PHASE 97 SMOKE TESTS PASSED"
