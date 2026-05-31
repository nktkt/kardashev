#!/usr/bin/env bash
# Phase 58 smoke test (Roadmap v10): monomorphize over a const VALUE.
#   1. `struct Mat<const N> { data: [i64; N] }` instantiated at 3 and 5 produces
#      two DISTINCT LLVM struct types — `%Mat__c3 = type { [3 x i64] }` and
#      `%Mat__c5 = type { [5 x i64] }` (verified via --emit-llvm) — and the
#      program runs correctly JIT + AOT.
#   2. A 2-D `Matrix<const R, const C>` over `[[i64; C]; R]` constructs from a
#      nested literal (both dims inferred) and lowers to `[R x [C x i64]]`.
#   3. Negatives: a dimension mismatch (`Mat<3>` where `Mat<5>` is wanted), a
#      TYPE in a const slot (`Mat<i64>`), a const VALUE in a type slot
#      (`Wrap<3>`), a negative const arg, and an un-inferable const param are
#      all rejected with a clear message.
# (The ast_print round-trip of `Mat<3>` is guarded by parser_test.)
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" \
    "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rejects() { # name file needle
    local n=$1 f=$2 needle=$3 out
    set +e; out=$("$KARDC" "$f" 2>&1); set -e
    if "$KARDC" "$f" >/dev/null 2>&1; then
        echo "FAIL [$n]: expected REJECTION, but it compiled"; exit 1
    fi
    if [[ -n "$needle" ]] && ! grep -qi "$needle" <<< "$out"; then
        echo "FAIL [$n]: rejected but missing '$needle'; got: $out"; exit 1
    fi
    echo "PASS [$n]: rejected as expected"
}

# 1. acceptance: distinct monomorphic instances at 3 and 5.
cat > "$TMP/p58.kd" <<'EOF'
struct Mat<const N: i64> { data: [i64; N] }
fn sum3(m: Mat<3>) -> i64 { m.data[0] + m.data[1] + m.data[2] }
fn sum5(m: Mat<5>) -> i64 {
    m.data[0] + m.data[1] + m.data[2] + m.data[3] + m.data[4]
}
fn main() -> i64 {
    let a: Mat<3> = Mat { data: [10, 20, 30] };
    let b: Mat<5> = Mat { data: [1, 2, 3, 4, 5] };
    sum3(a) + sum5(b)
}
EOF
jit=$("$KARDC" "$TMP/p58.kd" 2>/dev/null | tail -1)
[[ "$jit" == "75" ]] || { echo "FAIL [accept/jit]: expected 75 got '$jit'"; exit 1; }
# NB: use a here-string, not `echo | grep -q` — under `pipefail`, grep -q
# closing the pipe early gives echo a SIGPIPE that fails the pipeline.
ll=$("$KARDC" --emit-llvm "$TMP/p58.kd" 2>/dev/null)
grep -q "%Mat__c3 = type { \[3 x i64\] }" <<<"$ll" || {
    echo "FAIL [accept/llvm]: missing distinct Mat__c3 [3 x i64]"; exit 1; }
grep -q "%Mat__c5 = type { \[5 x i64\] }" <<<"$ll" || {
    echo "FAIL [accept/llvm]: missing distinct Mat__c5 [5 x i64]"; exit 1; }
"$KARDC" --no-cache -o "$TMP/p58" "$TMP/p58.kd" >/dev/null 2>&1
set +e; "$TMP/p58" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 75 ]] || { echo "FAIL [accept/aot]: exit $rc expected 75"; exit 1; }
echo "PASS [distinct-monomorphs]: Mat<3>/Mat<5> distinct LLVM types, JIT 75, AOT exit 75"

# 2. a 2-D matrix over a nested array — both dims inferred from the literal.
cat > "$TMP/mat2d.kd" <<'EOF'
struct Matrix<const R: i64, const C: i64> { data: [[i64; C]; R] }
fn get(m: Matrix<2, 3>) -> i64 { m.data[0][0] + m.data[1][2] }
fn main() -> i64 {
    let m: Matrix<2, 3> = Matrix { data: [[1, 2, 3], [4, 5, 6]] };
    get(m)
}
EOF
jit=$("$KARDC" "$TMP/mat2d.kd" 2>/dev/null | tail -1)
[[ "$jit" == "7" ]] || { echo "FAIL [mat2d/jit]: expected 7 got '$jit'"; exit 1; }
ll2=$("$KARDC" --emit-llvm "$TMP/mat2d.kd" 2>/dev/null)
grep -q "%Matrix__c2_c3 = type { \[2 x \[3 x i64\]\] }" <<<"$ll2" || {
    echo "FAIL [mat2d/llvm]: missing %Matrix__c2_c3 = { [2 x [3 x i64]] }"; exit 1; }
echo "PASS [matrix-2d]: Matrix<2,3> over [[i64;3];2], JIT 7"

# 3. negatives.
cat > "$TMP/mism.kd" <<'EOF'
struct Mat<const N: i64> { data: [i64; N] }
fn sum5(m: Mat<5>) -> i64 { m.data[0] }
fn main() -> i64 {
    let a: Mat<3> = Mat { data: [1, 2, 3] };
    sum5(a)
}
EOF
rejects dim-mismatch "$TMP/mism.kd" ""

printf 'struct Mat<const N: i64> { data: [i64; N] }\nfn main() -> i64 { let a: Mat<i64> = Mat { data: [1] }; 0 }\n' > "$TMP/typeslot.kd"
rejects type-in-const-slot "$TMP/typeslot.kd" "const"

printf 'struct Wrap<T> { x: T }\nfn main() -> i64 { let a: Wrap<3> = Wrap { x: 1 }; 0 }\n' > "$TMP/constslot.kd"
rejects const-in-type-slot "$TMP/constslot.kd" "expects a type"

# A const param is inferred from the dimensions of the field that carries it.
# A "phantom" const used in no array field can't be inferred from a bare literal
# (a clear error, not a silent miscompile). v28 Phase 153/154: but it CAN be
# supplied by the binding's type annotation now (expected-type propagation), so
# only the un-annotated form errors.
printf 'struct Phantom<const N: i64> { x: i64 }\nfn main() -> i64 { let p = Phantom { x: 0 }; p.x }\n' > "$TMP/infer.kd"
rejects phantom-uninferable "$TMP/infer.kd" "cannot infer const parameter"
printf 'struct Phantom2<const N: i64> { x: i64 }\nfn main() -> i64 { let p: Phantom2<4> = Phantom2 { x: 7 }; p.x }\n' > "$TMP/infer2.kd"
"$KARDC" "$TMP/infer2.kd" >/dev/null 2>&1 && echo "PASS [phantom-from-annotation]: annotation supplies the const arg" || { echo "FAIL [phantom-from-annotation]: annotation should supply N"; exit 1; }

echo "ALL PHASE 58 SMOKE TESTS PASSED"
