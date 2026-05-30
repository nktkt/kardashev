#!/usr/bin/env bash
# Phase 103 (Roadmap v17 — "self-hosting, cont."): the CODEGEN step, self-hosted
# (examples/selfhost/emit.kd). After parse + type-check, it COMPILES the Expr AST
# to a flat STACK-MACHINE BYTECODE and runs it on a tiny VM (operand stack +
# register file) — the back-end shape of a real compiler, in kardashev. Codegen
# is PROVEN correct by cross-checking every program against a direct tree-walking
# `eval`: the compiled (VM) result must equal the interpreted result. Checked:
# `a + b * 2` -> 11 (precedence), `if a<b {a+1} else {a*b}` -> 4, `if a==b {100}
# else {(a+b)*b}` -> 28; all match eval (allok=1). Output 11/4/28/1; witness
# v1*10000+v2*1000+v3+allok = 114029 (AOT exit 114029 & 255 = 109).
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/emit.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/emit.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/emit.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/emit.kd" \
    "examples/selfhost/emit.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/emit.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

want=$'11\n4\n28\n1\n114029'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [emit/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [emit/jit]: AST -> bytecode -> VM, results match the reference interpreter (allok=1)"

"$KARDC" --no-cache -o "$TMP/emit" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/emit"); rc=$?; set -e
    [[ "$rc" -eq 109 && "$aout" == $'11\n4\n28\n1' ]] || { echo "FAIL [emit/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [emit/aot]: same compile+run, exit 109 (=114029 & 255), deterministic"
echo "ALL PHASE 103 SMOKE TESTS PASSED"
