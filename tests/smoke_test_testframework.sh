#!/usr/bin/env bash
# v37 — real test framework: assert macros + --filter + --format=json.
#
# assert! / assert_eq! / assert_ne! are prelude macros (over the Phase 182
# engine) that `return` a non-zero code on failure, integrating with the
# `test_*() -> i64` runner convention (0 = ok). `kardc --test` reports per-test
# pass/fail and exits 0 iff all pass; `--filter <substr>` runs a subset;
# `--format=json` emits a machine-readable report.
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

cat > "$TMP/t.kd" <<'EOF'
fn test_math() -> i64 { assert_eq!(2 + 2, 4); assert!(10 > 3); assert_ne!(1, 2); 0 }
fn test_fails() -> i64 { assert_eq!(1, 2); 0 }
fn test_strings() -> i64 ! { alloc } { assert!(str_len(&"abc") == 3); 0 }
EOF
cat > "$TMP/allpass.kd" <<'EOF'
fn test_a() -> i64 { assert_eq!(1, 1); 0 }
fn test_b() -> i64 { assert!(true); 0 }
EOF

# 1. Mixed run: 2 pass, 1 fail; exit code non-zero.
out=$("$KARDC" --test "$TMP/t.kd" 2>&1) || rc=$?; rc=${rc:-0}
echo "$out" | grep -q 'test test_math ... ok' || { echo "FAIL: test_math not ok"; echo "$out"; exit 1; }
echo "$out" | grep -q 'test test_fails ... FAILED' || { echo "FAIL: test_fails not FAILED"; exit 1; }
echo "$out" | grep -q 'test result: 2 passed, 1 failed' || { echo "FAIL: wrong summary"; exit 1; }
[[ "$rc" -ne 0 ]] || { echo "FAIL: expected non-zero exit on a failing test"; exit 1; }
echo "PASS: mixed run reports + non-zero exit"

# 2. exit 0 iff all pass.
rc=0; "$KARDC" --test "$TMP/allpass.kd" >/dev/null 2>&1 || rc=$?
[[ "$rc" -eq 0 ]] || { echo "FAIL: all-pass run should exit 0 (got $rc)"; exit 1; }
echo "PASS: exit 0 when all pass"

# 3. --filter runs only matching tests.
out=$("$KARDC" --test --filter math "$TMP/t.kd" 2>&1) || true
echo "$out" | grep -q 'running 1 test' || { echo "FAIL: --filter math should select 1; got: $out"; exit 1; }
echo "$out" | grep -q 'test test_math ... ok' || { echo "FAIL: filtered test missing"; exit 1; }
echo "$out" | grep -q 'test_fails' && { echo "FAIL: --filter leaked an unmatched test"; exit 1; }
echo "PASS: --filter selects a subset"

# 4. --format=json emits a schema-checked report.
js=$("$KARDC" --test --format=json "$TMP/t.kd" 2>&1) || true
echo "$js" | grep -qE '"tests":\[' || { echo "FAIL: no json tests array; got: $js"; exit 1; }
echo "$js" | grep -q '"name":"test_fails","result":"failed","code":1' || { echo "FAIL: json missing failed entry; got: $js"; exit 1; }
echo "$js" | grep -q '"passed":2,"failed":1' || { echo "FAIL: json summary wrong; got: $js"; exit 1; }
echo "PASS: --format=json schema"

echo "ALL TEST-FRAMEWORK SMOKE TESTS PASSED"
