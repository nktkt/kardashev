#!/usr/bin/env bash
# Phase 64 smoke test (Roadmap v11 — the numeric tower): integer-literal
# WIDTH SUFFIXES (`5i32`) and RADIX prefixes (`0xFF`, `0b1010`).
#   1. A suffixed literal `5i32` IS an i32 with no annotation; it lowers to an
#      LLVM i32 and runs JIT + AOT.
#   2. Hex `0xFF` and binary `0b1010` literals parse to the right value (default
#      i64), in both expression and `match`-pattern position; a suffix combines
#      with a radix (`0xFFi32`).
#   3. The suffix is HONEST about the lattice: an out-of-range suffixed literal
#      (`200i8`) is a compile error, a suffixed/var width mismatch (`5i32 + i64`)
#      is a compile error, and an unsigned suffix (`5u8`, Phase 66) is rejected
#      with a clear "later phase" message rather than silently mis-typed.
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

# 1. a suffixed literal is an i32 directly (no annotation) and lowers to i32.
cat > "$TMP/sfx.kd" <<'EOF'
fn add(a: i32, b: i32) -> i32 { a + b }
fn main() -> i64 {
    let x = 5i32;          // i32 from the suffix alone
    let y = add(x, 3i32);  // 8 : i32
    let h = 0xFF;          // 255 : default i64
    let b = 0b1010;        // 10  : default i64
    h + b                  // 265 : i64
}
EOF
jit=$("$KARDC" "$TMP/sfx.kd" 2>/dev/null | tail -1)
[[ "$jit" == "265" ]] || { echo "FAIL [sfx/jit]: expected 265 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/sfx.kd" 2>/dev/null)
grep -q "define i32 @add(i32" <<<"$ll" || { echo "FAIL [sfx/llvm]: add not i32"; exit 1; }
"$KARDC" --no-cache -o "$TMP/sfx" "$TMP/sfx.kd" >/dev/null 2>&1
set +e; "$TMP/sfx" >/dev/null; rc=$?; set -e
[[ "$rc" -eq $((265 & 255)) ]] || { echo "FAIL [sfx/aot]: exit $rc expected $((265 & 255))"; exit 1; }
echo "PASS [suffix-i32]: 5i32 is i32 (lowers to i32), JIT 265"

# 2. hex / binary values, incl. a radix+suffix combo (0xFFi32), all add up.
cat > "$TMP/radix.kd" <<'EOF'
fn main() -> i64 {
    let a = 0xFF;          // 255
    let b = 0b1010;        // 10
    let c = 0x10;          // 16
    let d = 0b1;           // 1
    a + b + c + d          // 282
}
EOF
jit=$("$KARDC" "$TMP/radix.kd" 2>/dev/null | tail -1)
[[ "$jit" == "282" ]] || { echo "FAIL [radix/jit]: expected 282 got '$jit'"; exit 1; }
echo "PASS [radix]: 0xFF/0b1010/0x10/0b1 -> 282"

# 2b. radix + suffix combine, and a suffixed hex lowers to its width.
cat > "$TMP/combo.kd" <<'EOF'
fn use32(x: i32) -> i32 { x }
fn main() -> i64 {
    let m = 0xFFi32;       // 255 : i32
    let n = use32(m);      // 255 : i32
    let p: i32 = n;
    0
}
EOF
"$KARDC" "$TMP/combo.kd" >/dev/null 2>&1 || { echo "FAIL [combo]: 0xFFi32 did not compile"; exit 1; }
grep -q "define i32 @use32(i32" <<<"$("$KARDC" --emit-llvm "$TMP/combo.kd" 2>/dev/null)" \
    || { echo "FAIL [combo/llvm]: use32 not i32"; exit 1; }
echo "PASS [combo]: 0xFFi32 is i32"

# 2c. hex / binary literals work as match patterns.
cat > "$TMP/pat.kd" <<'EOF'
fn classify(n: i64) -> i64 {
    match n {
        0xFF => 1,
        0b10 => 2,
        _ => 0,
    }
}
fn main() -> i64 { classify(255) + classify(2) * 10 }
EOF
jit=$("$KARDC" "$TMP/pat.kd" 2>/dev/null | tail -1)
[[ "$jit" == "21" ]] || { echo "FAIL [pat]: expected 21 got '$jit'"; exit 1; }
echo "PASS [pat]: hex/binary match patterns (255->1, 2->2) = 21"

# 3a. an out-of-range suffixed literal is a compile error.
printf 'fn main() -> i64 { let x = 200i8; 0 }\n' > "$TMP/oor.kd"
rejects suffix-out-of-range "$TMP/oor.kd" "out of range"

# 3b. a suffixed-width vs i64-var mismatch is a type error (non-coercive).
printf 'fn main() -> i64 { let a = 5i32; let b: i64 = 7; let c = a + b; 0 }\n' > "$TMP/mix.kd"
rejects suffix-width-mismatch "$TMP/mix.kd" "same integer"

# 3c. an unsigned suffix is rejected honestly (lands in Phase 66).
printf 'fn main() -> i64 { let x = 5u8; 0 }\n' > "$TMP/uns.kd"
rejects unsigned-suffix-deferred "$TMP/uns.kd" "unsigned"

echo "ALL PHASE 64 SMOKE TESTS PASSED"
