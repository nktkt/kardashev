#!/usr/bin/env bash
# v39 — raw-pointer arithmetic + write (FFI maturity slice; retires Phase 177
# deferrals). `ptr_offset(p, n)` advances a `*const T`/`*mut T` by n ELEMENTS
# (GEP by pointee type); `ptr_write(p, v)` stores through a `*mut T`. Both are
# unchecked and require `unsafe` (like a raw deref). Differential JIT vs AOT.
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

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
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
    echo "$err" | grep -qi "$needle" || { echo "FAIL [$name]: expected '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# Pointer arithmetic over an array: ptr_offset reads the right elements.
diff_run ptr_read $'10\n30\n40' '
fn main() -> i64 ! { io } {
    let a = [10, 20, 30, 40];
    let base = &a[0] as *const i64;
    print(unsafe { *ptr_offset(base, 0) });
    print(unsafe { *ptr_offset(base, 2) });
    print(unsafe { *ptr_offset(base, 3) });
    0
}
'

# ptr_write stores through *mut T, with ptr_offset addressing.
diff_run ptr_write $'7\n8\n100' '
fn main() -> i64 ! { io } {
    let mut b = [0, 0, 0];
    let bp = &mut b[0] as *mut i64;
    let w = unsafe { ptr_write(bp, 7); ptr_write(ptr_offset(bp, 1), 8); ptr_write(ptr_offset(bp, 2), 100); 0 };
    print(unsafe { *bp });
    print(unsafe { *ptr_offset(bp, 1) });
    print(unsafe { *ptr_offset(bp, 2) });
    0
}
'

# Sum an array via a moving raw pointer (a C-style loop).
diff_run ptr_sum $'100' '
fn main() -> i64 ! { io } {
    let a = [10, 20, 30, 40];
    let base = &a[0] as *const i64;
    let mut i = 0;
    let mut s = 0;
    while i < 4 { s = s + unsafe { *ptr_offset(base, i) }; i = i + 1; }
    print(s);
    0
}
'

# NEGATIVE: ptr_offset requires unsafe.
expect_err offset_safe 'unsafe' '
fn main() -> i64 { let a=[1,2]; let p = &a[0] as *const i64; *ptr_offset(p, 1) }
'
# NEGATIVE: ptr_write requires a *mut (not *const).
expect_err write_const 'mut' '
fn main() -> i64 { let a=[1,2]; let p = &a[0] as *const i64; let w = unsafe { ptr_write(p, 9); 0 }; w }
'

echo "ALL FFI-PTRARITH SMOKE TESTS PASSED"
