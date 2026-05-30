#!/usr/bin/env bash
# Phase 66 smoke test (Roadmap v11 — the numeric tower): UNSIGNED integers
# u8/u16/u32/u64 + the integer BITWISE operators (& | ^ << >> ~).
#   1. Unsigned arithmetic / comparison / division use unsigned semantics
#      (udiv, icmp u**) — distinct from the signed ops; a u32 holds values past
#      i32::MAX. Runs JIT + AOT.
#   2. All six bitwise operators lower correctly; `>>` is LOGICAL for an
#      unsigned operand (lshr) and ARITHMETIC for a signed one (ashr).
#   3. Bitwise expressions const-fold; a u64 literal past i64::MAX (the FNV
#      offset basis) parses; a real FNV-1a step (xor + wrapping u64 multiply)
#      produces the textbook hash.
#   4. u32 and i32 are distinct (no implicit crossing); a bitwise op on f64 and
#      an out-of-range unsigned literal are compile errors.
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

# 1. unsigned semantics: a u32 past i32::MAX, unsigned division + comparison.
# The op helpers take params so the optimizer can't constant-fold the udiv /
# unsigned-compare away — the LLVM check sees the real instructions.
cat > "$TMP/u.kd" <<'EOF'
fn udiv32(a: u32, b: u32) -> u32 { a / b }
fn ugt8(a: u8, b: u8) -> bool { a > b }
fn main() -> i64 {
    let a: u8 = 200;
    let b: u8 = 100;
    let c: u8 = a - b;            // 100
    let big: u32 = 4000000000;    // > i32::MAX, valid u32
    let half: u32 = udiv32(big, 2u32);  // unsigned div => 2000000000
    let cmp: bool = ugt8(a, b);   // unsigned compare => true
    let r = (c as i64) + (half as i64);
    if cmp { r } else { 0 - r }   // 100 + 2000000000 => 2000000100
}
EOF
jit=$("$KARDC" "$TMP/u.kd" 2>/dev/null | tail -1)
[[ "$jit" == "2000000100" ]] || { echo "FAIL [u/jit]: expected 2000000100 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/u.kd" 2>/dev/null)
grep -q "udiv i32" <<<"$ll" || { echo "FAIL [u/llvm]: no udiv i32"; exit 1; }
grep -q "icmp ugt i8" <<<"$ll" || { echo "FAIL [u/llvm]: no unsigned compare"; exit 1; }
echo "PASS [unsigned]: udiv + unsigned compare, u32 past i32::MAX, JIT 2000000100"

# 2. all six bitwise operators.
cat > "$TMP/bit.kd" <<'EOF'
fn main() -> i64 {
    let x: u32 = 0xF0;
    let y: u32 = 0x0F;
    let a = x | y;        // 0xFF = 255
    let b = x & 0xFFu32;  // 0xF0 = 240
    let c = x ^ y;        // 0xFF = 255
    let d = x << 4;       // 0xF00 = 3840
    let e = x >> 4;       // 0x0F = 15
    let f = ~y & 0xFFu32; // ~0x0F & 0xFF = 0xF0 = 240
    (a as i64) + (b as i64) + (c as i64) + (d as i64) + (e as i64) + (f as i64)
}
EOF
jit=$("$KARDC" "$TMP/bit.kd" 2>/dev/null | tail -1)
[[ "$jit" == "4845" ]] || { echo "FAIL [bit/jit]: expected 4845 got '$jit'"; exit 1; }
echo "PASS [bitwise]: & | ^ << >> ~ all correct (sum 4845)"

# 2b. >> is logical for unsigned (lshr), arithmetic for signed (ashr). Param
# helpers keep the shifts out of the constant folder so the IR check is real.
cat > "$TMP/shr.kd" <<'EOF'
fn ushr(x: u8, n: u8) -> u8 { x >> n }
fn sshr(x: i8, n: i8) -> i8 { x >> n }
fn main() -> i64 {
    let u: u8 = 0x80;       // 128
    let su = ushr(u, 1u8);  // logical => 64
    let s: i8 = -100 as i8; // -100
    let ss = sshr(s, 1 as i8); // arithmetic => -50
    (su as i64) - (ss as i64)   // 64 - (-50) => 114
}
EOF
jit=$("$KARDC" "$TMP/shr.kd" 2>/dev/null | tail -1)
[[ "$jit" == "114" ]] || { echo "FAIL [shr/jit]: expected 114 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/shr.kd" 2>/dev/null)
grep -q "lshr" <<<"$ll" || { echo "FAIL [shr/llvm]: no lshr (unsigned >>)"; exit 1; }
grep -q "ashr" <<<"$ll" || { echo "FAIL [shr/llvm]: no ashr (signed >>)"; exit 1; }
"$KARDC" --no-cache -o "$TMP/shr" "$TMP/shr.kd" >/dev/null 2>&1
set +e; "$TMP/shr" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 114 ]] || { echo "FAIL [shr/aot]: exit $rc expected 114"; exit 1; }
echo "PASS [shift-signedness]: lshr (unsigned) vs ashr (signed), JIT+AOT 114"

# 3. bitwise const-fold.
cat > "$TMP/cbit.kd" <<'EOF'
const MASK: i64 = 0xFF & 0x3C;     // 60
const SHIFTED: i64 = 1 << 10;      // 1024
fn main() -> i64 { MASK + SHIFTED } // 1084
EOF
jit=$("$KARDC" "$TMP/cbit.kd" 2>/dev/null | tail -1)
[[ "$jit" == "1084" ]] || { echo "FAIL [cbit]: expected 1084 got '$jit'"; exit 1; }
echo "PASS [const-bitwise]: 0xFF & 0x3C + (1 << 10) = 1084"

# 3b. a real FNV-1a step over the byte 'a' — low 16 bits of FNV-1a("a") is 0xec8c.
cat > "$TMP/fnv.kd" <<'EOF'
fn main() -> i64 {
    let basis: u64 = 0xcbf29ce484222325;   // > i64::MAX
    let prime: u64 = 0x100000001b3;
    let h: u64 = (basis ^ 0x61) * prime;   // wrapping u64 multiply
    (h & 0xFFFF) as i64
}
EOF
jit=$("$KARDC" "$TMP/fnv.kd" 2>/dev/null | tail -1)
[[ "$jit" == "60556" ]] || { echo "FAIL [fnv]: expected 60556 (0xec8c) got '$jit'"; exit 1; }
echo "PASS [u64-fnv]: large u64 literal + wrapping multiply = FNV-1a('a') low16 = 0xec8c"

# 4. rejects: distinctness, float-bitwise, out-of-range unsigned literal.
printf 'fn main() -> i64 { let a: u32 = 5; let b: i32 = 7; let c = a + b; 0 }\n' > "$TMP/mix.kd"
rejects unsigned-distinct "$TMP/mix.kd" "same integer"
printf 'fn main() -> i64 { let x: f64 = 1.0; let y = x & x; 0 }\n' > "$TMP/fbit.kd"
rejects float-bitwise "$TMP/fbit.kd" "bitwise"
printf 'fn main() -> i64 { let x: u8 = 300; 0 }\n' > "$TMP/oor.kd"
rejects unsigned-out-of-range "$TMP/oor.kd" "out of range"

echo "ALL PHASE 66 SMOKE TESTS PASSED"
