#!/usr/bin/env bash
# Phase 63 smoke test (Roadmap v11 — the numeric tower): sized SIGNED machine
# integers i8/i16/i32 (i64 is the default).
#   1. An i32 fn (params + return + arithmetic) lowers to `i32` in LLVM (not
#      i64) and runs JIT + AOT.
#   2. The lattice is NON-coercive: an i32 binding can't take an i64 value (no
#      implicit widening), and an out-of-range literal for a narrow width is a
#      clear error.
#   3. An unsuffixed literal defaults to i64 but NARROWS to i32 in context.
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

# 1. an i32 fn lowers to i32 + runs.
cat > "$TMP/i32.kd" <<'EOF'
fn add(a: i32, b: i32) -> i32 { a + b }
fn dec(a: i32) -> i32 { a - 1 }
fn main() -> i64 {
    let x: i32 = 20;
    let y: i32 = add(x, 10);       // 30
    let z: i32 = dec(y);           // 29
    let n = 13;                    // default i64
    n + 0
}
EOF
jit=$("$KARDC" "$TMP/i32.kd" 2>/dev/null | tail -1)
[[ "$jit" == "13" ]] || { echo "FAIL [i32/jit]: expected 13 got '$jit'"; exit 1; }
ll=$("$KARDC" --emit-llvm "$TMP/i32.kd" 2>/dev/null)
grep -q "define i32 @add(i32" <<<"$ll" || { echo "FAIL [i32/llvm]: add not lowered to i32"; exit 1; }
grep -q "add i32" <<<"$ll" || { echo "FAIL [i32/llvm]: no 'add i32'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/i32" "$TMP/i32.kd" >/dev/null 2>&1
set +e; "$TMP/i32" >/dev/null; rc=$?; set -e
[[ "$rc" -eq 13 ]] || { echo "FAIL [i32/aot]: exit $rc expected 13"; exit 1; }
echo "PASS [i32-sized]: i32 fn lowers to i32 (not i64), JIT 13, AOT 13"

# 2a. no implicit widening — i32 binding can't take an i64 value.
printf 'fn id64(x: i64) -> i64 { x }\nfn main() -> i64 { let w: i32 = id64(5); 0 }\n' > "$TMP/widen.kd"
rejects no-implicit-widening "$TMP/widen.kd" "i32"

# 2b. mixed-width arithmetic is a type error.
printf 'fn main() -> i64 { let a: i32 = 5; let b: i64 = 7; let c = a + b; 0 }\n' > "$TMP/mixed.kd"
rejects mixed-width-arith "$TMP/mixed.kd" "same integer"

# 2c. an out-of-range literal for a narrow width is rejected.
printf 'fn main() -> i64 { let x: i8 = 200; 0 }\n' > "$TMP/range.kd"
rejects literal-out-of-range "$TMP/range.kd" "out of range"

echo "ALL PHASE 63 SMOKE TESTS PASSED"
