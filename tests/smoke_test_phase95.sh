#!/usr/bin/env bash
# Phase 95 (Roadmap v16 — "self-hosting, cont."): a STATEMENT/BLOCK parser +
# evaluator written in kardashev (examples/selfhost/stmt.kd). Grows the body
# grammar from a single expression to a block: a sequence of `let NAME = EXPR ;`
# bindings + a final result expression, parsed into Block { lets: Vec<Stmt>,
# result: Box<Expr> } and evaluated by running each `let` in order (EXTENDING the
# environment) then the result — how an interpreter executes a function body.
# Over `let x = a + 1 ; let y = x * 2 ; y` with { a: 3 }: 2 lets, x=4, y=8 -> 8
# (a `let` references both the outer binding and an earlier `let`). Built JIT+AOT.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/stmt.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/stmt.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/stmt.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/stmt.kd" \
    "examples/selfhost/stmt.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/stmt.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
want=$'2\n8\n8'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [stmt/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [stmt/jit]: let x=a+1; let y=x*2; y  with {a:3}  -> 2 lets, result 8 (env extended per let)"
"$KARDC" --no-cache -o "$TMP/stmt" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/stmt"); rc=$?; set -e
    [[ "$rc" -eq 8 && "$aout" == $'2\n8' ]] || { echo "FAIL [stmt/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [stmt/aot]: same block evaluation, exit 8, deterministic"
echo "ALL PHASE 95 SMOKE TESTS PASSED"
