#!/usr/bin/env bash
# Phase 25 smoke test: compile-time constants + const evaluation, end to end
# through both pipeline paths (JIT + AOT).
#
#   - `const SIZE: i64 = 3 + 2;` evaluates at compile time; `main` returns 5,
#     and the emitted LLVM IR contains the FOLDED literal (no runtime add /
#     global load) — proving the const reached codegen as an immediate.
#   - `const fn sq(x) { x * x }` called in a const context (`const NINE =
#     sq(3)`) is fully evaluated -> 9.
#   - A const referencing another const (`const A = 10; const B = A * 2`) -> 20.
#   - Const-generic array lengths: `[i64; N]` (N a const item) and
#     `[i64; sq(2)]` (N a const-fn call) — the Phase 22 literal-only lift.
#   - A `const fn` is ALSO an ordinary runtime fn: `sq(y)` on a runtime `y`
#     still compiles + runs.
#   - Error cases produce CLEAR compile errors (not crashes / hangs): a
#     div-by-zero const initializer, a const fn doing I/O in a const context,
#     a cyclic const, and a runaway recursive const fn (bounded, not a hang).
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Run a program through JIT (kardc prints main's return on the final line) and
# AOT (native binary exit code), asserting both equal $3.
#   $1 = label, $2 = source file, $3 = expected i64 result
check_jit_aot() {
    local label="$1" src="$2" want="$3"
    local jit
    jit=$("$KARDC" "$src" | tail -n 1)
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL ($label): JIT returned '$jit', expected '$want'"
        exit 1
    fi
    "$KARDC" -o "$TMP/bin_$label" "$src" >/dev/null
    set +e
    "$TMP/bin_$label"
    local rc=$?
    set -e
    if [[ "$rc" -ne "$want" ]]; then
        echo "FAIL ($label): AOT exit code $rc, expected $want"
        exit 1
    fi
    echo "OK ($label): JIT + AOT both = $want"
}

# --- 1. const item evaluated at compile time -------------------------------
cat > "$TMP/size.kd" <<'EOF'
const SIZE: i64 = 3 + 2;
fn main() -> i64 { SIZE }
EOF
check_jit_aot "const_item" "$TMP/size.kd" 5

# The const must be a FOLDED literal in the IR: `ret i64 5`, with no runtime
# `add` and no global storage for SIZE. Check at -O0 (where folding is purely
# the const-evaluator's doing, not the optimizer's).
IR=$("$KARDC" --emit-llvm -O0 "$TMP/size.kd")
MAIN_BODY=$(echo "$IR" | awk '/define .*@main\(/{f=1} f{print} /^}/{if(f)exit}')
if ! echo "$MAIN_BODY" | grep -q 'ret i64 5'; then
    echo "FAIL: expected folded 'ret i64 5' in main; got:"
    echo "$MAIN_BODY"
    exit 1
fi
if echo "$MAIN_BODY" | grep -Eq '\badd\b|\bload\b'; then
    echo "FAIL: main contains a runtime add/load — const was not folded:"
    echo "$MAIN_BODY"
    exit 1
fi
echo "OK (fold): SIZE compiled to the literal 5 (no add/load in main)"

# --- 2. const fn evaluated in a const context ------------------------------
cat > "$TMP/sq.kd" <<'EOF'
const fn sq(x: i64) -> i64 { x * x }
const NINE: i64 = sq(3);
fn main() -> i64 { NINE }
EOF
check_jit_aot "const_fn" "$TMP/sq.kd" 9

# --- 3. const referencing const --------------------------------------------
cat > "$TMP/chain.kd" <<'EOF'
const A: i64 = 10;
const B: i64 = A * 2;
fn main() -> i64 { B }
EOF
check_jit_aot "const_ref_const" "$TMP/chain.kd" 20

# --- 4a. const-generic array length from a const item ----------------------
cat > "$TMP/arrn.kd" <<'EOF'
const N: i64 = 2 + 1;
fn main() -> i64 { let a: [i64; N] = [10, 20, 30]; a[0] + a[2] }
EOF
check_jit_aot "array_len_const" "$TMP/arrn.kd" 40
# The array type must be [3 x i64] in the IR (N evaluated to 3).
if ! "$KARDC" --emit-llvm -O0 "$TMP/arrn.kd" | grep -q '\[3 x i64\]'; then
    echo "FAIL: expected array type [3 x i64] from const N=3"
    exit 1
fi
echo "OK (array N): const N=3 produced [3 x i64]"

# --- 4b. const-generic array length from a const-fn call -------------------
cat > "$TMP/arrsq.kd" <<'EOF'
const fn sq(x: i64) -> i64 { x * x }
fn main() -> i64 {
    let a: [i64; sq(2)] = [1, 2, 3, 4];
    a[0] + a[1] + a[2] + a[3]
}
EOF
check_jit_aot "array_len_constfn" "$TMP/arrsq.kd" 10
if ! "$KARDC" --emit-llvm -O0 "$TMP/arrsq.kd" | grep -q '\[4 x i64\]'; then
    echo "FAIL: expected array type [4 x i64] from sq(2)=4"
    exit 1
fi
echo "OK (array sq(2)): const-fn length produced [4 x i64]"

# --- 5. a const fn is ALSO an ordinary runtime fn --------------------------
cat > "$TMP/runtime.kd" <<'EOF'
const fn sq(x: i64) -> i64 { x * x }
fn main() -> i64 { let y = 7; sq(y) }
EOF
check_jit_aot "const_fn_runtime" "$TMP/runtime.kd" 49

# --- nested const fn (let + if + call) -------------------------------------
cat > "$TMP/nested.kd" <<'EOF'
const fn dbl(x: i64) -> i64 { x + x }
const fn pick(a: i64, b: i64) -> i64 { let m = dbl(a); if m > b { m } else { b } }
const R: i64 = pick(3, 5);
fn main() -> i64 { R }
EOF
check_jit_aot "nested_const_fn" "$TMP/nested.kd" 6

# --- Error cases: must be CLEAR compile errors (rc != 0), no crash/hang ----
expect_error() {
    local label="$1" src="$2" needle="$3"
    set +e
    local out rc
    out=$(timeout 20 "$KARDC" "$src" 2>&1)
    rc=$?
    set -e
    if [[ "$rc" -eq 124 ]]; then
        echo "FAIL ($label): compiler HUNG (timeout) — const eval not bounded"
        exit 1
    fi
    if [[ "$rc" -eq 0 ]]; then
        echo "FAIL ($label): expected a compile error, but compilation succeeded"
        exit 1
    fi
    # A clean diagnostic, not a C++ crash (segfault / std::terminate / abort).
    if echo "$out" | grep -Eqi 'std::bad_alloc|terminate|Segmentation|core dumped|Assertion'; then
        echo "FAIL ($label): compiler crashed instead of reporting an error:"
        echo "$out"
        exit 1
    fi
    if ! echo "$out" | grep -qi "$needle"; then
        echo "FAIL ($label): error message missing '$needle'; got:"
        echo "$out"
        exit 1
    fi
    echo "OK ($label): clear compile error"
}

cat > "$TMP/div0.kd" <<'EOF'
const BAD: i64 = 10 / 0;
fn main() -> i64 { BAD }
EOF
expect_error "div_by_zero" "$TMP/div0.kd" "division by zero"

cat > "$TMP/io.kd" <<'EOF'
const fn bad(x: i64) -> i64 { print(x); x }
const V: i64 = bad(5);
fn main() -> i64 { V }
EOF
expect_error "non_evaluable_const_fn" "$TMP/io.kd" "not const-evaluable"

cat > "$TMP/cyclic.kd" <<'EOF'
const A: i64 = B + 1;
const B: i64 = A + 1;
fn main() -> i64 { A }
EOF
expect_error "cyclic_const" "$TMP/cyclic.kd" "cyclic"

cat > "$TMP/runaway.kd" <<'EOF'
const fn loopy(x: i64) -> i64 { loopy(x) + 1 }
const V: i64 = loopy(1);
fn main() -> i64 { V }
EOF
expect_error "runaway_const_fn" "$TMP/runaway.kd" "recursion-depth"

cat > "$TMP/overflow.kd" <<'EOF'
const BIG: i64 = 9223372036854775807 + 1;
fn main() -> i64 { BIG }
EOF
expect_error "overflow_const" "$TMP/overflow.kd" "overflow"

echo "PASS: compile-time constants + const fn evaluation + const-generic array lengths work in JIT + AOT"
