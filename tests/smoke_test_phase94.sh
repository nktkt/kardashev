#!/usr/bin/env bash
# Phase 94 (Roadmap v16 — "self-hosting, cont."): an EXPRESSION parser + evaluator
# written in kardashev (examples/selfhost/expr.kd). Extends the v15 front from
# signatures to the body grammar: a recursive-descent parser builds an `enum Expr`
# AST (Num / Var / Add / Mul, Box-recursive) for an arithmetic expression with
# VARIABLE REFERENCES (the step beyond examples/calc), then EVALUATES it against a
# HashMap<String,i64> environment { a:3, b:4 }. Proves precedence (`a + b * 2`=11)
# and parentheses (`(a + b) * 2`=14); witness 1114. Built JIT + AOT.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/expr.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/expr.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/expr.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/expr.kd" \
    "examples/selfhost/expr.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/expr.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
want=$'11\n14\n1114'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [expr/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [expr/jit]: a + b*2 = 11 (precedence), (a + b)*2 = 14 (parens), vars resolved via HashMap env"
"$KARDC" --no-cache -o "$TMP/expr" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/expr"); rc=$?; set -e
    [[ "$rc" -eq 90 && "$aout" == $'11\n14' ]] || { echo "FAIL [expr/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [expr/aot]: same evaluations, exit 90 (= 1114 & 255), deterministic"
echo "ALL PHASE 94 SMOKE TESTS PASSED"
