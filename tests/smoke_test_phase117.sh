#!/usr/bin/env bash
# Phase 117 (Roadmap v20 — "toward a real bootstrap"): the self-hosted LLVM-IR
# compiler (examples/selfhost/structgen.kd) now handles STRUCTS — it parses
# `struct NAME { f: i64, ... }`, builds struct literals, reads fields, and lowers
# them to first-class LLVM aggregates (`insertvalue`/`extractvalue` over
# `{ i64, ... }`). It emits a compilable module (clang -> native) and is
# differential-gated against the host on several struct programs (self-hosted
# result == host compiler result). MVP: struct fields are i64. (Struct names
# avoid single uppercase letters, which collide with the host's generic params.)
# Skips if clang is unavailable.
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
    "${TEST_SRCDIR:-}/_main/examples/selfhost/structgen.kd" "${TEST_SRCDIR:-}/kardashev/examples/selfhost/structgen.kd" \
    "${RUNFILES_DIR:-}/_main/examples/selfhost/structgen.kd" "${RUNFILES_DIR:-}/kardashev/examples/selfhost/structgen.kd" \
    "examples/selfhost/structgen.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/selfhost/structgen.kd not found"; exit 1; }
CLANG="$(command -v clang || true)"
[[ -z "$CLANG" ]] && { echo "PASS [phase117]: SKIPPED (no clang to compile the emitted IR)"; exit 0; }
echo "Using kardc at: $KARDC ; clang at: $CLANG"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

"$KARDC" --no-cache -o "$TMP/sg" "$SRC" >/dev/null 2>&1 || { echo "FAIL [phase117]: struct compiler did not build"; exit 1; }

# 1. Demo (no args): emit IR, confirm it uses struct-aggregate ops, run -> 7.
"$TMP/sg" > "$TMP/d.ll" 2>/dev/null || { echo "FAIL [phase117]: struct compiler did not run"; exit 1; }
grep -q 'insertvalue { i64, i64 }' "$TMP/d.ll" || { echo "FAIL [phase117]: no 'insertvalue' (struct build) in IR"; cat "$TMP/d.ll"; exit 1; }
grep -q 'extractvalue { i64, i64 }' "$TMP/d.ll" || { echo "FAIL [phase117]: no 'extractvalue' (field read) in IR"; cat "$TMP/d.ll"; exit 1; }
"$CLANG" "$TMP/d.ll" -o "$TMP/d" 2>/dev/null || { echo "FAIL [phase117]: clang rejected struct IR"; cat "$TMP/d.ll"; exit 1; }
"$TMP/d" >/dev/null 2>&1; rd=$?
[[ "$rd" -eq 7 ]] || { echo "FAIL [phase117]: demo exit $rd (want 7 = 3+4)"; exit 1; }
echo "PASS [struct-ir]: struct literal -> insertvalue, field read -> extractvalue; native exit 7"

# 2. DIFFERENTIAL: several struct programs, self-hosted IR vs host. (safe names)
diff_case() {  # $1 source, $2 a, $3 b, $4 label
    "$TMP/sg" "$1" "$2" "$3" > "$TMP/s.ll" 2>/dev/null || { echo "FAIL [phase117/$4]: selfcc errored"; exit 1; }
    "$CLANG" "$TMP/s.ll" -o "$TMP/s" 2>/dev/null || { echo "FAIL [phase117/$4]: clang rejected IR"; cat "$TMP/s.ll"; exit 1; }
    "$TMP/s" >/dev/null 2>&1; local r_self=$?
    printf '%s\nfn main() -> i64 { f(%s, %s) }\n' "$1" "$2" "$3" > "$TMP/h.kd"
    "$KARDC" --no-cache -o "$TMP/h" "$TMP/h.kd" >/dev/null 2>&1 || { echo "FAIL [phase117/$4]: host rejected program"; exit 1; }
    "$TMP/h" >/dev/null 2>&1; local r_host=$?
    [[ "$r_self" -eq "$r_host" ]] || { echo "FAIL [phase117/$4]: self=$r_self != host=$r_host"; exit 1; }
    echo "PASS [$4]: self == host == $r_self"
}
diff_case "struct P { x: i64, y: i64 } fn f(a: i64, b: i64) -> i64 { let p = P { x: a, y: b } ; p.x + p.y }" 3 4 "build+sum"
diff_case "struct P { x: i64, y: i64 } fn f(a: i64, b: i64) -> i64 { let p = P { x: a, y: b } ; if p.x < p.y { p.y } else { p.x } }" 9 2 "field-in-if"
diff_case "struct Pair { p: i64, q: i64, r: i64 } fn f(a: i64, b: i64) -> i64 { let t = Pair { p: a, q: b, r: a * b } ; (t.p + t.q) + t.r }" 3 4 "three-field"
diff_case "struct Pair { p: i64, q: i64, r: i64 } fn f(a: i64, b: i64) -> i64 { let t = Pair { p: a + 1, q: b * 2, r: a } ; if t.p == t.r { t.q } else { t.p * t.r } }" 5 3 "mixed"

# A struct-RETURNING function (self-only — the host can't wrap a struct return in
# an i64 main): the self-hosted compiler returns the aggregate and `main` extracts
# field 0 as the exit code.
"$TMP/sg" "struct P { x: i64, y: i64 } fn f(a: i64, b: i64) -> P { P { x: a + b, y: a } }" 3 4 > "$TMP/sr.ll" 2>/dev/null
"$CLANG" "$TMP/sr.ll" -o "$TMP/sr" 2>/dev/null || { echo "FAIL [phase117/struct-return]: clang rejected aggregate-return IR"; cat "$TMP/sr.ll"; exit 1; }
"$TMP/sr" >/dev/null 2>&1; rsr=$?
[[ "$rsr" -eq 7 ]] || { echo "FAIL [phase117/struct-return]: exit $rsr (want 7 = field0 = a+b)"; exit 1; }
echo "PASS [struct-return]: a struct-returning fn returns the aggregate; main extracts field 0 ($rsr)"

echo "ALL PHASE 117 SMOKE TESTS PASSED"
