#!/usr/bin/env bash
# v34 Phase 185 smoke test — richer comptime / const-fn (imperative loops).
#
# A `const fn` may now use the imperative `let mut … ; while … { … }` style with
# variable reassignment, all evaluated at COMPILE time. This lets iterative
# algorithms (factorial, fibonacci, sum) compute `const` values and drive
# array lengths without recursion. Non-termination is caught by the global
# step budget (no compiler hang); field/index assignment in a const context is
# a clear error. Differentially gated JIT vs AOT.
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

diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || {
        echo "FAIL [$name]: expected error containing '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# 1. Iterative factorial + fibonacci as const fns, folded into const items.
diff_run fact_fib $'120\n55' '
const fn fact(n: i64) -> i64 {
    let mut r: i64 = 1;
    let mut i: i64 = 1;
    while i <= n { r = r * i; i = i + 1; }
    r
}
const fn fib(n: i64) -> i64 {
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    let mut k: i64 = 0;
    while k < n { let t = a + b; a = b; b = t; k = k + 1; }
    a
}
const F: i64 = fact(5);
const G: i64 = fib(10);
fn main() -> i64 ! { io } { print(F); print(G); 0 }
'

# 2. A const-fn loop drives an ARRAY LENGTH — only possible if it is evaluated
#    at compile time (array sizes must be constants). sq(3) = 9.
diff_run const_arraylen $'7' '
const fn sq(n: i64) -> i64 {
    let mut r: i64 = 0; let mut i: i64 = 0;
    while i < n { r = r + n; i = i + 1; }
    r
}
fn main() -> i64 ! { io } {
    let a: [i64; sq(3)] = [7, 7, 7, 7, 7, 7, 7, 7, 7];
    print(a[8]);
    0
}
'

# 3. An early `return` inside a const loop short-circuits the whole fn (finds
#    the first n whose triangular sum exceeds 10 → n = 5: 1+2+3+4+5 = 15).
diff_run const_return_in_loop $'5' '
const fn first_over_10() -> i64 {
    let mut sum: i64 = 0;
    let mut n: i64 = 0;
    let mut ans: i64 = 0;
    while n < 100 { n = n + 1; sum = sum + n; if sum > 10 { return n; } else {} }
    ans
}
const N: i64 = first_over_10();
fn main() -> i64 ! { io } { print(N); 0 }
'

# 4. NEGATIVE: a non-terminating const loop trips the step budget — the
#    compiler reports an error instead of hanging.
expect_err nonterminating 'step budget' '
const fn bad() -> i64 { let mut i: i64 = 0; while i >= 0 { i = i + 1; } i }
const X: i64 = bad();
fn main() -> i64 { X }
'

# 5. NEGATIVE: field/index assignment is not const-evaluable (only simple
#    variables can be reassigned in a const context).
expect_err field_assign 'simple-variable assignment' '
struct P { x: i64 }
const fn f() -> i64 { let mut p = P { x: 1 }; p.x = 5; p.x }
const Y: i64 = f();
fn main() -> i64 { Y }
'

echo "ALL PHASE 185 SMOKE TESTS PASSED"
