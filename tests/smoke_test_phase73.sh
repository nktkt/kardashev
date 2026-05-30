#!/usr/bin/env bash
# Phase 73 smoke test (Roadmap v12 — the real stdlib): integer math helpers
# (i64_abs/min/max/pow), f64 math (f64_sqrt/floor/ceil/abs via LLVM intrinsics),
# and a few more Option/Result inspectors (option_is_some/ok_or, result_is_ok).
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

cat > "$TMP/n.kd" <<'EOF'
fn b(x: bool) -> i64 { if x { 1 } else { 0 } }
fn main() -> i64 ! { io } {
    print(i64_abs(0 - 7));            // 7
    print(i64_min(3, 9));             // 3
    print(i64_max(3, 9));             // 9
    print(i64_pow(2, 10));            // 1024
    print(f64_sqrt(144.0) as i64);    // 12
    print(f64_floor(3.7) as i64);     // 3
    print(f64_ceil(3.2) as i64);      // 4
    print(f64_abs(0.0 - 5.5) as i64); // 5
    print(b(option_is_some(Some(5))));                         // 1
    print(b(option_is_some(None)));                            // 0
    print(result_unwrap_or(option_ok_or(Some(7), 0 - 1), 0));  // 7
    print(result_unwrap_or(option_ok_or(None, 0 - 9), 0));     // 0 (Err->default)
    print(b(result_is_ok(option_ok_or(Some(1), 0))));          // 1
    0
}
EOF
want=$'7\n3\n9\n1024\n12\n3\n4\n5\n1\n0\n7\n0\n1'
got=$("$KARDC" "$TMP/n.kd" 2>/dev/null); got=$(head -13 <<< "$got")
[[ "$got" == "$want" ]] || { echo "FAIL [num/jit]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
echo "PASS [num/jit]: i64 abs/min/max/pow, f64 sqrt/floor/ceil/abs, Option/Result inspectors"

"$KARDC" --no-cache -o "$TMP/n" "$TMP/n.kd" >/dev/null 2>&1
aot=$("$TMP/n")
[[ "$aot" == "$want" ]] || { echo "FAIL [num/aot]:"; diff <(echo "$want") <(echo "$aot"); exit 1; }
echo "PASS [num/aot]: same transcript AOT-compiled (LLVM float intrinsics link)"

echo "ALL PHASE 73 SMOKE TESTS PASSED"
