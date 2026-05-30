#!/usr/bin/env bash
# Phase 67 smoke test (Roadmap v11 — the numeric tower): DEFINED overflow
# semantics + the f32 single-precision float.
#   1. f32 is a real single-precision type: an f32 fn lowers to LLVM `float`
#      (not `double`), arithmetic uses `fmul/fadd float`, and `1.5f32` is an f32
#      literal. Runs JIT + AOT.
#   2. f32 and f64 are distinct non-coercive types; `as` bridges them with
#      fpext (f32->f64) and fptrunc (f64->f32).
#   3. Integer overflow is DEFINED as two's-complement wrapping at every width
#      (`127i8 + 1 == -128`, `255u8 + 1 == 0`, `16u8 * 16u8 == 0`), identically
#      JIT and AOT.
#   4. Negative narrow-int literals narrow in context (`let x: i8 = -128` is
#      valid — i8::MIN); a negative literal for an unsigned type (`u8 = -1`) is
#      a compile error.
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

rejects() { local n=$1 f=$2 needle=$3 out; set +e; out=$("$KARDC" "$f" 2>&1); set -e
    if "$KARDC" "$f" >/dev/null 2>&1; then echo "FAIL [$n]: compiled, expected error"; exit 1; fi
    grep -qi "$needle" <<<"$out" || { echo "FAIL [$n]: missing '$needle'; got: $out"; exit 1; }
    echo "PASS [$n]: rejected"; }

# 1. f32 fn lowers to LLVM float + runs.
cat > "$TMP/f32.kd" <<'EOF'
fn scale(x: f32, k: f32) -> f32 { x * k }
fn main() -> i64 {
    let a: f32 = 1.5;
    let b: f32 = scale(a, 4.0);   // 6.0 : f32
    let c = b + 0.5f32;           // 6.5 : f32
    c as i64                      // 6
}
EOF
jit=$("$KARDC" "$TMP/f32.kd" 2>/dev/null | tail -1)
[[ "$jit" == "6" ]] || { echo "FAIL [f32/jit]: expected 6 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/f32.kd" 2>/dev/null)
grep -q "define float @scale(float" <<<"$ll" || { echo "FAIL [f32/llvm]: scale not float"; exit 1; }
grep -q "fmul float" <<<"$ll" || { echo "FAIL [f32/llvm]: no fmul float"; exit 1; }
"$KARDC" --no-cache -o "$TMP/f32" "$TMP/f32.kd" >/dev/null 2>&1
set +e; "$TMP/f32" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 6 ]] || { echo "FAIL [f32/aot]: exit $rc expected 6"; exit 1; }
echo "PASS [f32]: f32 fn lowers to LLVM float (not double), JIT+AOT 6"

# 2. f32 <-> f64 casts: fpext + fptrunc (param helpers keep them in the IR).
cat > "$TMP/fc.kd" <<'EOF'
fn narrow(d: f64) -> f32 { d as f32 }
fn widen(s: f32) -> f64 { s as f64 }
fn main() -> i64 { (widen(narrow(3.75)) * 2.0) as i64 }   // 7
EOF
jit=$("$KARDC" "$TMP/fc.kd" 2>/dev/null | tail -1)
[[ "$jit" == "7" ]] || { echo "FAIL [fc/jit]: expected 7 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/fc.kd" 2>/dev/null)
grep -q "fptrunc double" <<<"$ll" || { echo "FAIL [fc/llvm]: no fptrunc"; exit 1; }
grep -q "fpext float" <<<"$ll" || { echo "FAIL [fc/llvm]: no fpext"; exit 1; }
echo "PASS [f32<->f64]: fptrunc + fpext, JIT 7"

# 2b. f32 and f64 don't mix without a cast.
printf 'fn main() -> i64 { let a: f32 = 1.0; let b: f64 = 2.0; let c = a + b; 0 }\n' > "$TMP/fmix.kd"
rejects float-width-mismatch "$TMP/fmix.kd" "same float"

# 3. defined wrapping overflow at every width, JIT + AOT.
cat > "$TMP/wrap.kd" <<'EOF'
fn add8(a: i8, b: i8) -> i8 { a + b }
fn addu8(a: u8, b: u8) -> u8 { a + b }
fn mulu8(a: u8, b: u8) -> u8 { a * b }
fn main() -> i64 {
    let i: i8 = add8(127 as i8, 1 as i8);   // wraps to -128
    let u: u8 = addu8(255u8, 1u8);           // wraps to 0
    let m: u8 = mulu8(16u8, 16u8);           // 256 wraps to 0
    (i as i64) + (u as i64) + (m as i64)     // -128
}
EOF
jit=$("$KARDC" "$TMP/wrap.kd" 2>/dev/null | tail -1)
[[ "$jit" == "-128" ]] || { echo "FAIL [wrap/jit]: expected -128 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/wrap" "$TMP/wrap.kd" >/dev/null 2>&1
set +e; "$TMP/wrap" >/dev/null; rc=$?; set -e
[[ "$rc" -eq $(( (-128) & 255 )) ]] || { echo "FAIL [wrap/aot]: exit $rc expected 128"; exit 1; }
echo "PASS [overflow]: two's-complement wrapping (i8 127+1=-128, u8 255+1=0), JIT+AOT"

# 4. negative narrow-int literals.
cat > "$TMP/neg.kd" <<'EOF'
fn main() -> i64 {
    let a: i8 = -100;     // narrows
    let b: i8 = -128;     // i8::MIN, valid
    (a as i64) + (b as i64)   // -228
}
EOF
jit=$("$KARDC" "$TMP/neg.kd" 2>/dev/null | tail -1)
[[ "$jit" == "-228" ]] || { echo "FAIL [neg/jit]: expected -228 got '$jit'"; exit 1; }
echo "PASS [neg-literal]: let i8 = -100 / -128 narrow correctly"

# 4b. a negative literal for an unsigned type is a compile error.
printf 'fn main() -> i64 { let x: u8 = -1; 0 }\n' > "$TMP/u.kd"
rejects unsigned-negative-literal "$TMP/u.kd" "out of range"

echo "ALL PHASE 67 SMOKE TESTS PASSED"
