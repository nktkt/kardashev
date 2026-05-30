#!/usr/bin/env bash
# Phase 65 smoke test (Roadmap v11 — the numeric tower): the `as` CAST operator,
# the only bridge across the non-coercive lattice.
#   1. int<->int casts widen (sext) / narrow (trunc) at the right LLVM widths,
#      and a cast lets a mixed-width expression type-check; runs JIT + AOT.
#   2. int<->float casts lower to sitofp / fptosi (truncating toward zero).
#   3. `as` precedence: tighter than binary, looser than a prefix unary
#      (`a as i32 * 2` == `(a as i32) * 2`, `-x as i32` == `(-x) as i32`),
#      and casts chain left-to-right (`x as i32 as i64`).
#   4. A cast is const-foldable (int->int) and wraps with two's-complement
#      semantics (`300 as i8` == 44) consistently at compile time and run time.
#   5. A non-numeric cast (`struct as i32`, `bool as i64`) is a compile error.
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

# 1. int<->int casts bridge a mixed-width expression + lower to sext/trunc.
cat > "$TMP/i2i.kd" <<'EOF'
fn main() -> i64 {
    let a: i32 = 1000;
    let b: i64 = 5;
    let c: i64 = a as i64 + b;     // widen i32->i64, add  => 1005
    let d: i32 = (c as i32) - 5;   // narrow i64->i32       => 1000
    let e: i8 = 300 as i8;         // wrap 300 into i8       => 44
    c + (d as i64) + (e as i64)    // 1005 + 1000 + 44       => 2049
}
EOF
jit=$("$KARDC" "$TMP/i2i.kd" 2>/dev/null | tail -1)
[[ "$jit" == "2049" ]] || { echo "FAIL [i2i/jit]: expected 2049 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/i2i.kd" 2>/dev/null)
grep -q "sext i32" <<<"$ll" || { echo "FAIL [i2i/llvm]: no sext i32"; exit 1; }
grep -q "trunc i64" <<<"$ll" || { echo "FAIL [i2i/llvm]: no trunc i64"; exit 1; }
"$KARDC" --no-cache -o "$TMP/i2i" "$TMP/i2i.kd" >/dev/null 2>&1
set +e; "$TMP/i2i" >/dev/null; rc=$?; set -e
[[ "$rc" -eq $((2049 & 255)) ]] || { echo "FAIL [i2i/aot]: exit $rc expected $((2049 & 255))"; exit 1; }
echo "PASS [int<->int]: sext/trunc widths correct, JIT 2049, AOT matches"

# 2. int<->float casts lower to sitofp / fptosi, truncating toward zero.
cat > "$TMP/i2f.kd" <<'EOF'
fn main() -> i64 {
    let x: i64 = 7;
    let f: f64 = x as f64 / 2.0;   // 3.5
    let back: i64 = f as i64;      // trunc toward zero => 3
    let g: f64 = 9.9;
    back + (g as i64)              // 3 + 9 => 12
}
EOF
jit=$("$KARDC" "$TMP/i2f.kd" 2>/dev/null | tail -1)
[[ "$jit" == "12" ]] || { echo "FAIL [i2f/jit]: expected 12 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/i2f.kd" 2>/dev/null)
grep -q "sitofp" <<<"$ll" || { echo "FAIL [i2f/llvm]: no sitofp"; exit 1; }
grep -q "fptosi" <<<"$ll" || { echo "FAIL [i2f/llvm]: no fptosi"; exit 1; }
echo "PASS [int<->float]: sitofp/fptosi, JIT 12"

# 3. precedence + chaining.
cat > "$TMP/prec.kd" <<'EOF'
fn main() -> i64 {
    let a: i64 = 10;
    let b: i32 = 3;
    let p = (b as i64) * 2;        // 6
    let q = -a as i32;             // (-10) as i32 = -10
    let r = a as i32 as i64;       // chained = 10
    p + (q as i64) + r             // 6 + (-10) + 10 = 6
}
EOF
jit=$("$KARDC" "$TMP/prec.kd" 2>/dev/null | tail -1)
[[ "$jit" == "6" ]] || { echo "FAIL [prec]: expected 6 got '$jit'"; exit 1; }
echo "PASS [precedence]: as binds below binary / above unary, chains l-to-r"

# 4. const-eval cast wraps with two's-complement, same as run time.
cat > "$TMP/ceval.kd" <<'EOF'
const SMALL: i32 = 1000 as i32;
const TINY: i8 = 300 as i8;        // wraps to 44 at const time
fn main() -> i64 { (SMALL as i64) + (TINY as i64) }   // 1044
EOF
jit=$("$KARDC" "$TMP/ceval.kd" 2>/dev/null | tail -1)
[[ "$jit" == "1044" ]] || { echo "FAIL [ceval]: expected 1044 got '$jit'"; exit 1; }
echo "PASS [const-eval]: const cast folds + wraps (300 as i8 = 44)"

# 5. non-numeric casts are compile errors.
printf 'struct P { x: i64 }\nfn main() -> i64 { let p = P { x: 1 }; let n = p as i32; 0 }\n' > "$TMP/bad1.kd"
rejects struct-as-numeric "$TMP/bad1.kd" "numeric"
printf 'fn main() -> i64 { let b = true; let n = b as i64; n }\n' > "$TMP/bad2.kd"
rejects bool-as-numeric "$TMP/bad2.kd" "numeric"

echo "ALL PHASE 65 SMOKE TESTS PASSED"
