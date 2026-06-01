#!/usr/bin/env bash
# v33 Phase 181 smoke test — overflow-checked + wrapping integer arithmetic.
#
# kardashev's DEFAULT integer-overflow policy is 2's-complement WRAP (the AOT
# path compiles under `-fwrapv`; the JIT matches). Phase 181 adds explicit
# control:
#   checked_add/sub/mul/div(a, b) -> Option<i64>   None on signed overflow
#                                                  (or div-by-zero, INT_MIN/-1)
#   wrapping_add/sub/mul(a, b)    -> i64           the explicit wrapping op
# Overflow is detected without the `*.with.overflow` LLVM intrinsics (portable
# sign-bit identities for add/sub, a 128-bit widen-and-compare for mul).
# Differentially gated JIT vs AOT.
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

# checked_add: in-range -> Some, MAX+1 -> None.
diff_run checked_add $'42\n-1' '
fn main() -> i64 ! { io } {
    let max = 9223372036854775807;
    match checked_add(40, 2) { Some(v) => print(v), None => print(0 - 1), }
    match checked_add(max, 1) { Some(v) => print(v), None => print(0 - 1), }
    0
}
'

# checked_sub: in-range -> Some, MIN-1 -> None.
diff_run checked_sub $'8\n-1' '
fn main() -> i64 ! { io } {
    let min = 0 - 9223372036854775807 - 1;
    match checked_sub(10, 2) { Some(v) => print(v), None => print(0 - 1), }
    match checked_sub(min, 1) { Some(v) => print(v), None => print(0 - 1), }
    0
}
'

# checked_mul: in-range -> Some, MAX*2 -> None.
diff_run checked_mul $'30\n-1' '
fn main() -> i64 ! { io } {
    let max = 9223372036854775807;
    match checked_mul(6, 5)   { Some(v) => print(v), None => print(0 - 1), }
    match checked_mul(max, 2) { Some(v) => print(v), None => print(0 - 1), }
    0
}
'

# checked_div: normal -> Some, /0 -> None, INT_MIN / -1 -> None.
diff_run checked_div $'5\n-1\n-1' '
fn main() -> i64 ! { io } {
    let min = 0 - 9223372036854775807 - 1;
    match checked_div(20, 4)  { Some(v) => print(v), None => print(0 - 1), }
    match checked_div(10, 0)  { Some(v) => print(v), None => print(0 - 1), }
    match checked_div(min, 0 - 1) { Some(v) => print(v), None => print(0 - 1), }
    0
}
'

# wrapping_*: MAX+1 wraps to INT_MIN; MIN-1 wraps to MAX; MAX*2 wraps to -2.
diff_run wrapping $'-9223372036854775808\n9223372036854775807\n-2' '
fn main() -> i64 ! { io } {
    let max = 9223372036854775807;
    let min = 0 - 9223372036854775807 - 1;
    print(wrapping_add(max, 1));
    print(wrapping_sub(min, 1));
    print(wrapping_mul(max, 2));
    0
}
'

echo "ALL PHASE 181 SMOKE TESTS PASSED"
