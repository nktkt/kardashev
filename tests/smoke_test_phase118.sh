#!/usr/bin/env bash
# Phase 118 (Roadmap v20 — "toward a real bootstrap"): the self-hosted LLVM-IR
# compiler (examples/selfhost/enumgen.kd) now handles ENUMS + `match`. An enum is
# a tagged pair `{ i64 tag, i64 payload }` (MVP: every variant takes one i64);
# constructing `V(e)` -> insertvalue; an enum-typed `if` -> select over the
# aggregate; `match e { V(x) => body, ... }` -> extractvalue tag/payload + a
# branch-free SELECT-CHAIN on the tag (pure language, so all arms compute and the
# matching one is selected — no phi/blocks). Emits a compilable module
# (clang -> native), differential-gated against the host. Skips if no clang.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/selfhost/enumgen.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/enumgen.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/enumgen.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/enumgen.kd" \
    "examples/selfhost/enumgen.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/enumgen.kd not found"; exit 1; }
CLANG="$(command -v clang || true)"
[[ -z "$CLANG" ]] && { echo "PASS [phase118]: SKIPPED (no clang to compile the emitted IR)"; exit 0; }
echo "Using kardc at: $KARDC ; clang at: $CLANG"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

"$KARDC" --no-cache -o "$TMP/eg" "$SRC" >/dev/null 2>&1 || { echo "FAIL [phase118]: enum compiler did not build"; exit 1; }

# 1. Demo (no args): emit IR, confirm enum-aggregate ops + match select-chain, run -> 103.
"$TMP/eg" > "$TMP/d.ll" 2>/dev/null || { echo "FAIL [phase118]: enum compiler did not run"; exit 1; }
grep -q 'insertvalue { i64, i64 } undef, i64' "$TMP/d.ll" || { echo "FAIL [phase118]: no enum construction (insertvalue) in IR"; cat "$TMP/d.ll"; exit 1; }
grep -q 'select i1 .*{ i64, i64 }' "$TMP/d.ll" || { echo "FAIL [phase118]: no enum-typed select (if over aggregate) in IR"; cat "$TMP/d.ll"; exit 1; }
grep -q 'extractvalue { i64, i64 } .*, 0' "$TMP/d.ll" || { echo "FAIL [phase118]: no tag extract (match) in IR"; cat "$TMP/d.ll"; exit 1; }
"$CLANG" "$TMP/d.ll" -o "$TMP/d" 2>/dev/null || { echo "FAIL [phase118]: clang rejected enum IR"; cat "$TMP/d.ll"; exit 1; }
"$TMP/d" >/dev/null 2>&1; rd=$?
[[ "$rd" -eq 103 ]] || { echo "FAIL [phase118]: demo exit $rd (want 103: Add(3)->3+100)"; exit 1; }
echo "PASS [enum-ir]: enum construct/if-select/match-select-chain; native exit 103"

# 2. DIFFERENTIAL: enum programs over both branches + variants, self vs host.
diff_case() {  # $1 source, $2 a, $3 b, $4 label
    "$TMP/eg" "$1" "$2" "$3" > "$TMP/s.ll" 2>/dev/null || { echo "FAIL [phase118/$4]: enum compiler errored"; exit 1; }
    "$CLANG" "$TMP/s.ll" -o "$TMP/s" 2>/dev/null || { echo "FAIL [phase118/$4]: clang rejected IR"; cat "$TMP/s.ll"; exit 1; }
    "$TMP/s" >/dev/null 2>&1; local r_self=$?
    printf '%s\nfn main() -> i64 { f(%s, %s) }\n' "$1" "$2" "$3" > "$TMP/h.kd"
    "$KARDC" --no-cache -o "$TMP/h" "$TMP/h.kd" >/dev/null 2>&1 || { echo "FAIL [phase118/$4]: host rejected program"; exit 1; }
    "$TMP/h" >/dev/null 2>&1; local r_host=$?
    [[ "$r_self" -eq "$r_host" ]] || { echo "FAIL [phase118/$4]: self=$r_self != host=$r_host"; exit 1; }
    echo "PASS [$4]: self == host == $r_self"
}
OP="enum Op { Add(i64), Mul(i64) } fn f(a: i64, b: i64) -> i64 { let e = if a < b { Add(a) } else { Mul(b) } ; match e { Add(x) => x + 100 , Mul(y) => y * 2 } }"
diff_case "$OP" 3 4 "two-variant-Add"
diff_case "$OP" 5 2 "two-variant-Mul"
SEL="enum Sel { A(i64), B(i64), C(i64) } fn f(a: i64, b: i64) -> i64 { let e = if a == b { A(a) } else { if a < b { B(b) } else { C(a) } } ; match e { A(x) => x * 10 , B(y) => y + 1 , C(z) => z + 1000 } }"
diff_case "$SEL" 4 4 "three-variant-A"
diff_case "$SEL" 2 8 "three-variant-B"
diff_case "$SEL" 9 3 "three-variant-C"
# Regression (adversarial review): a `match` whose ARMS return enum values — the
# match select-chain must use the arm RESULT type ({i64,i64}), not hardcoded i64.
NEST="enum P2 { L(i64), R(i64) } fn f(a: i64, b: i64) -> i64 { let r = match L(a) { L(x) => R(x) , R(y) => L(y) } ; match r { L(p) => p , R(q) => q + 1000 } }"
diff_case "$NEST" 5 0 "match-returns-enum"

echo "ALL PHASE 118 SMOKE TESTS PASSED"
