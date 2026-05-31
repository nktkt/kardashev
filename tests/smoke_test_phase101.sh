#!/usr/bin/env bash
# Phase 101 (Roadmap v17 — "self-hosting, cont."): a real TYPE CHECKER written in
# kardashev (examples/selfhost/typeck.kd). The self-hosted expression language has
# TWO types — i64 and bool — so checking is non-trivial: `+ *` are int×int→int,
# `< ==` are int×int→bool, and `if c { t } else { e }` requires a bool condition
# and equal branch types. type_of infers a type tag (0 TErr, 1 TInt, 2 TBool),
# propagating errors. Checked on: a well-typed `if` (->TInt), an int+bool mismatch
# (->TErr), a non-bool `if` condition (->TErr), and a bare comparison (->TBool).
# Output 1/0/0/2; witness t1*1000+t2*100+t3*10+t4 = 1002 (AOT exit 1002 & 255 = 234).
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/typeck.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/typeck.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/typeck.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/typeck.kd" \
    "examples/selfhost/typeck.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/typeck.kd not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

want=$'1\n0\n0\n2\n1002'
for run in 1 2 3; do
    got=$("$KARDC" "$SRC" 2>/dev/null)
    [[ "$got" == "$want" ]] || { echo "FAIL [typeck/jit run $run]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
done
echo "PASS [typeck/jit]: i64/bool type checker — well-typed if->TInt, int+bool->TErr, non-bool cond->TErr, cmp->TBool"

"$KARDC" --no-cache -o "$TMP/typeck" "$SRC" >/dev/null 2>&1
for run in 1 2 3; do
    set +e; aout=$("$TMP/typeck"); rc=$?; set -e
    [[ "$rc" -eq 234 && "$aout" == $'1\n0\n0\n2' ]] || { echo "FAIL [typeck/aot run $run]: exit $rc out '$aout'"; exit 1; }
done
echo "PASS [typeck/aot]: same type-checking, exit 234 (=1002 & 255), deterministic"
echo "ALL PHASE 101 SMOKE TESTS PASSED"
