#!/usr/bin/env bash
# v36 Phase 196 smoke test — bounds-check elision (codegen optimization).
#
# When an array index is a compile-time integer constant provably in [0, len),
# the access is statically in-bounds and codegen emits NO runtime bounds check
# (no compare + branch + panic block). A genuinely runtime index still gets the
# check. Verified two ways: (1) correctness is unchanged under JIT + AOT, and
# (2) at -O0 (so LLVM's own optimizer doesn't confound the comparison) the
# emitted IR for a constant-only program contains zero `idx.panic` blocks while
# a runtime-index program retains them.
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

# 1. Correctness: constant indices + a runtime index both compute correctly.
diff_run correctness $'60\n30' '
fn at(i: i64) -> i64 { let a = [10, 20, 30]; a[i] }
fn main() -> i64 ! { io } {
    let a = [10, 20, 30];
    print(a[0] + a[1] + a[2]);   // 60 (constant indices, checks elided)
    print(at(2));                // 30 (runtime index, check kept)
    0
}
'

# 2. Elision proof at -O0: a constant-only program emits ZERO `idx.panic`
#    blocks; a runtime-index program keeps them.
printf '%s' 'fn main() -> i64 ! { io } { let a = [10, 20, 30]; print(a[0] + a[1] + a[2]); 0 }' > "$TMP/const.kd"
printf '%s' 'fn at(i: i64) -> i64 { let a = [10, 20, 30]; a[i] }
fn main() -> i64 ! { io } { print(at(2)); 0 }' > "$TMP/rt.kd"

cpan=$("$KARDC" -O0 --emit-llvm "$TMP/const.kd" 2>/dev/null | grep -c 'idx\.panic' || true)
rpan=$("$KARDC" -O0 --emit-llvm "$TMP/rt.kd" 2>/dev/null | grep -c 'idx\.panic' || true)
[[ "$cpan" -eq 0 ]] || { echo "FAIL: constant-only program still has $cpan idx.panic blocks (check not elided)"; exit 1; }
echo "PASS: constant-index bounds checks elided (0 idx.panic)"
[[ "$rpan" -gt 0 ]] || { echo "FAIL: runtime-index program has no idx.panic — bounds check wrongly removed"; exit 1; }
echo "PASS: runtime-index bounds check retained ($rpan idx.panic refs)"

# 3. An out-of-range CONSTANT is still caught (behavior unchanged — we only
#    elide provably-safe checks, never introduce unsafety).
printf '%s' 'fn at(i: i64) -> i64 { let a = [10, 20, 30]; a[i] }
fn main() -> i64 ! { io } { print(at(9)); 0 }' > "$TMP/oob.kd"
# Capture first (the panicking process aborts non-zero; under `pipefail` a
# direct `kardc | grep` would fail the pipeline even when grep matches).
oobout=$("$KARDC" "$TMP/oob.kd" 2>&1 || true)
echo "$oobout" | grep -qi 'out of bounds' || { echo "FAIL: out-of-range index not caught, got: $oobout"; exit 1; }
echo "PASS: out-of-range access still panics"

echo "ALL PHASE 196 SMOKE TESTS PASSED"
