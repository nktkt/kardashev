#!/usr/bin/env bash
# Phase 24 smoke test: user-declarable `extern "C"` FFI functions, end to end
# through both the JIT (libc symbols resolved from the host process) and AOT
# (libc linked via clang) paths, plus the effect treatment.
#
# What it proves:
#   1. A libc `int` function called with the value verified CORRECT (not
#      truncated/garbage): `extern "C" fn abs(x: i32) -> i32;` then
#      `abs(0 - 7)` == 7, where `i32` maps to C `int` (LLVM i32) and codegen
#      narrows the i64 arg + sign-extends the i32 result at the boundary. We
#      ALSO exercise the i64-spelled `extern "C" fn abs(x: i64) -> i64;` form
#      (C `long`) to show the documented default mapping works.
#   2. A second extern of a different shape:
#        - `putchar(c: i32) -> i32` writes a byte to stdout (observed).
#        - `strlen(s: &String) -> i64` maps a `&String` to a C pointer (the
#          String's data pointer) and returns the right length (5 for
#          "hello"), proving the pointer mapping.
#        - `getpid() -> i32` is a clean no-arg int fn (present on glibc + macOS).
#   3. The extern call carries the `io` effect: a PURE caller is rejected; an
#      `! { io }` caller is accepted (ties Phase 10a effect checking). An
#      explicit `! { }` row on the extern declares it pure and a pure caller
#      is then accepted.
#   4. JIT and AOT produce identical results.
#
# All libc functions used (abs, putchar, strlen, getpid) exist on BOTH
# ubuntu-latest (glibc) and macOS-latest; none are GNU-/`__`-prefixed.
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
    echo "FAIL: kardc binary not found in runfiles"
    echo "  TEST_SRCDIR=${TEST_SRCDIR:-(unset)}"
    echo "  RUNFILES_DIR=${RUNFILES_DIR:-(unset)}"
    echo "  PWD=$(pwd)"
    find . -name kardc -type f 2>/dev/null | head -5
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---------------------------------------------------------------------------
# Program 1: abs (i32 / C int), strlen (&String -> C pointer), putchar (stdout),
# getpid (clean no-arg int). main returns abs(-7)+strlen("hello")+ok = 7+5+1=13.
# putchar writes 'X' (88) then '\n' (10) BEFORE main's return value is printed.
# ---------------------------------------------------------------------------
cat > "$TMP/ffi.kd" <<'EOF'
extern "C" fn abs(x: i32) -> i32;
extern "C" fn strlen(s: &String) -> i64;
extern "C" fn putchar(c: i32) -> i32;
extern "C" fn getpid() -> i32;

fn main() -> i64 ! { io } {
    let a = abs(0 - 7);          // C int abs: -7 -> 7 (correct, sign-extended)
    let s = "hello";
    let n = strlen(&s);          // &String -> C pointer (data ptr) -> 5
    putchar(88);                 // 'X'
    putchar(10);                 // '\n'
    let p = getpid();            // > 0 on any running process
    let ok = if p > 0 { 1 } else { 0 };
    a + n + ok                   // 7 + 5 + 1 = 13
}
EOF

# JIT: kardc runs main() and prints its i64 return on the final line. putchar
# writes "X\n" first, so we expect:  X<newline>13<newline>.
JIT_OUT=$("$KARDC" "$TMP/ffi.kd")
EXPECTED=$'X\n13'
if [[ "$JIT_OUT" != "$EXPECTED" ]]; then
    echo "FAIL: JIT FFI output mismatch"
    echo "expected:"; printf '%q\n' "$EXPECTED"
    echo "got:";      printf '%q\n' "$JIT_OUT"
    exit 1
fi
echo "JIT: abs(i32)=7 + strlen(&String)=5 + getpid>0 => 13, putchar wrote 'X'"

# AOT: native binary writes 'X\n' to stdout and returns 13 as its exit code.
"$KARDC" -o "$TMP/ffi" "$TMP/ffi.kd"
if [[ ! -x "$TMP/ffi" ]]; then
    echo "FAIL: AOT did not produce an executable"
    exit 1
fi
set +e
AOT_STDOUT=$("$TMP/ffi")
rc=$?
set -e
if [[ "$rc" -ne 13 ]]; then
    echo "FAIL: AOT FFI executable returned exit code $rc (expected 13)"
    exit 1
fi
if [[ "$AOT_STDOUT" != "X" ]]; then
    echo "FAIL: AOT FFI stdout was '$AOT_STDOUT' (expected 'X' from putchar)"
    exit 1
fi
echo "AOT: exit code 13 and putchar wrote 'X' to stdout"

# ---------------------------------------------------------------------------
# Program 2: the task's verbatim i64-spelled abs form (C long) -> 7.
# ---------------------------------------------------------------------------
cat > "$TMP/abs64.kd" <<'EOF'
extern "C" fn abs(x: i64) -> i64;
fn main() -> i64 ! { io } { abs(0 - 7) }
EOF
JIT_ABS=$("$KARDC" "$TMP/abs64.kd")
if [[ "$JIT_ABS" != "7" ]]; then
    echo "FAIL: JIT i64-spelled abs(0-7) returned '$JIT_ABS' (expected 7)"
    exit 1
fi
"$KARDC" -o "$TMP/abs64" "$TMP/abs64.kd"
set +e
"$TMP/abs64"; rc=$?
set -e
if [[ "$rc" -ne 7 ]]; then
    echo "FAIL: AOT i64-spelled abs(0-7) exit code $rc (expected 7)"
    exit 1
fi
echo "JIT+AOT: i64-spelled (C long) abs(0-7) == 7"

# ---------------------------------------------------------------------------
# Effect treatment (Phase 10a tie-in).
# ---------------------------------------------------------------------------
# Negative: a PURE fn calling an extern (which carries `io`) must be rejected.
cat > "$TMP/pure_leak.kd" <<'EOF'
extern "C" fn getpid() -> i32;
fn leaky() -> i64 { getpid() }
fn main() -> i64 ! { io } { leaky() }
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/pure_leak.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: a pure fn calling an extern (io) should be rejected"
    exit 1
fi
if ! echo "$ERR_OUT" | grep -q 'effect `io`'; then
    echo 'FAIL: expected an `io` effect-undeclared diagnostic, got:'
    echo "$ERR_OUT"
    exit 1
fi
echo "Negative: pure fn calling an extern (io) correctly rejected"

# Positive: an `! { io }` fn calling the same extern is accepted and runs.
cat > "$TMP/io_ok.kd" <<'EOF'
extern "C" fn getpid() -> i32;
fn safe() -> i64 ! { io } { let p = getpid(); if p > 0 { 1 } else { 0 } }
fn main() -> i64 ! { io } { safe() }
EOF
IO_OUT=$("$KARDC" "$TMP/io_ok.kd")
if [[ "$IO_OUT" != "1" ]]; then
    echo "FAIL: io-declaring caller of extern returned '$IO_OUT' (expected 1)"
    exit 1
fi
echo "Positive: an \`! { io }\` fn may call an extern (getpid > 0 => 1)"

# An explicit `! { }` row declares a pure extern; a pure caller is then OK.
cat > "$TMP/pure_extern.kd" <<'EOF'
extern "C" fn abs(x: i32) -> i32 ! { };
fn caller() -> i64 { abs(0 - 5) }
fn main() -> i64 { caller() }
EOF
PURE_OUT=$("$KARDC" "$TMP/pure_extern.kd")
if [[ "$PURE_OUT" != "5" ]]; then
    echo "FAIL: pure-declared extern abs(0-5) returned '$PURE_OUT' (expected 5)"
    exit 1
fi
echo "Positive: an extern with an explicit \`! { }\` row is pure; pure caller OK"

# ---------------------------------------------------------------------------
# Error surface: a non-"C" ABI string is rejected with a clear message.
# ---------------------------------------------------------------------------
cat > "$TMP/bad_abi.kd" <<'EOF'
extern "Rust" fn abs(x: i64) -> i64;
fn main() -> i64 { 0 }
EOF
set +e
ABI_ERR=$("$KARDC" "$TMP/bad_abi.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: a non-\"C\" ABI extern should be rejected"
    exit 1
fi
if ! echo "$ABI_ERR" | grep -q 'unsupported ABI'; then
    echo 'FAIL: expected an `unsupported ABI` diagnostic, got:'
    echo "$ABI_ERR"
    exit 1
fi
echo "Negative: non-\"C\" ABI extern correctly rejected"

echo "PASS: extern \"C\" FFI (i32/i64 ints, &String pointer, putchar stdout) "\
"works in JIT + AOT, and the io effect is tracked through extern calls"
