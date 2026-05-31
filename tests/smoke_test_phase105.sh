#!/usr/bin/env bash
# Phase 105 (Roadmap v17 CAPSTONE — "self-hosting, cont."): a self-hosted mini-
# COMPILER end-to-end (examples/selfhost/compile.kd). It takes a whole
# `fn NAME(PARAMS) -> RET { LETS ; RESULT }`, TYPE-CHECKS it, and — only if
# well-typed — COMPILES the body (now including `let` LOCALS, lowered to a STORE
# into a register slot) to stack-machine bytecode and EXECUTES it against a set of
# argument values. lex -> parse -> typecheck -> codegen -> VM, every stage in
# kardashev. Checked: f(a,b){let x=a+b; x*2} f(3,4)=14; g(a){let y=a*a; let z=y+a;
# z} g(5)=30; bool h(a,b){a<b} h(3,4)=1; and an ill-typed fn REJECTED before
# codegen (-1). Output 14/30/1/-1; witness r1*1000+r2*10+r3+(r4+1) = 14301
# (AOT exit 14301 & 255 = 221).
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/compile.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/compile.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/compile.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/compile.kd" \
    "examples/selfhost/compile.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/compile.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

want=$'14\n30\n1\n-1\n14301'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [compile/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [compile/jit]: whole-fn typecheck+codegen+run with let locals; ill-typed rejected before codegen"

"$KARDC" --no-cache -o "$TMP/compile" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/compile"); rc=$?; set -e
    [[ "$rc" -eq 221 && "$aout" == $'14\n30\n1\n-1' ]] || { echo "FAIL [compile/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [compile/aot]: same end-to-end compile+run, exit 221 (=14301 & 255), deterministic"
echo "ALL PHASE 105 SMOKE TESTS PASSED"
