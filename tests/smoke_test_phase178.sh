#!/usr/bin/env bash
# v33 Phase 178 smoke test — FFI maturity (scalars + pointers).
#
# `extern "C"` signatures, which were limited to i32/i64/bool/&String, now also
# accept f64 / f32 (C double / float), the full integer width tower (i8..i64 /
# u8..u64), and — via Phase 177's raw pointers — `*const T` / `*mut T` (a C
# pointer). This covers the bulk of real C interop: libm math, and the
# pointer-taking libc/buffer APIs. Verified end to end against REAL libm/libc
# (the AOT path links via clang with -lm).
#
# Documented Phase-178 scope: STRUCT-by-value across `extern "C"`, C function-
# pointer CALLBACKS, and C-header BINDING GENERATION (bindgen) are deferred —
# struct-by-value needs the platform-specific C ABI lowering (sret/byval/register
# splitting), and bindgen needs a C-header parser. The scalar + pointer surface
# (what the majority of FFI uses) is what lands here.
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

# These call REAL libc/libm, so gate on the AOT (clang-linked) binary's stdout.
# (JIT also resolves the host's libc/libm symbols.)
aot_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1 || {
        echo "FAIL [$name]: compile/link failed"; exit 1; }
    local got; got=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$got" == "$expect" ]] || { echo "FAIL [$name]: expected '$expect' got '$got'"; exit 1; }
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

# 1. f64 across extern "C" — call libm sqrt / pow (C double in, C double out).
aot_run extern_f64 $'4\n1024' '
extern "C" fn sqrt(x: f64) -> f64 ! { };
extern "C" fn pow(b: f64, e: f64) -> f64 ! { };
fn main() -> i64 ! { io } { print_f64(sqrt(16.0)); print_f64(pow(2.0, 10.0)); 0 }
'

# 2. Raw pointers across extern "C" — libc memset zeroes the bytes of a local.
aot_run extern_rawptr_memset $'0' '
extern "C" fn memset(p: *mut i64, c: i32, n: i64) -> *mut i64 ! { };
fn main() -> i64 ! { io } {
    let mut x = 9999;
    memset(&mut x as *mut i64, 0, 8);
    print(x);
    0
}
'

# 3. Raw pointers + libc memcpy — copy one i64 into another.
aot_run extern_memcpy $'77' '
extern "C" fn memcpy(dst: *mut i64, src: *const i64, n: i64) -> *mut i64 ! { };
fn main() -> i64 ! { io } {
    let src = 77;
    let mut dst = 0;
    memcpy(&mut dst as *mut i64, &src as *const i64, 8);
    print(dst);
    0
}
'

# 4. The integer width tower across extern "C" — libc abs as `int` (i32).
aot_run extern_int $'5' '
extern "C" fn abs(x: i32) -> i32 ! { };
fn main() -> i64 ! { io } { print(abs(0 - 5) as i64); 0 }
'

# 5. NEGATIVE: a struct passed BY VALUE across extern "C" is still rejected
#    (documents the deferred struct-by-value ABI work).
expect_err extern_struct_byvalue 'not supported' '
struct P { x: i64, y: i64 }
extern "C" fn takes(p: P) -> i64 ! { };
fn main() -> i64 ! { io } { print(takes(P { x: 1, y: 2 })); 0 }
'

echo "ALL PHASE 178 SMOKE TESTS PASSED"
