#!/usr/bin/env bash
# v19 Phase 114: when codegen reports a real error, the diagnostics must show only
# that error — NOT cascading "module verification failed" noise from the
# placeholder IR each error path keeps emitting. Asserts a known-bad program
# (`&<temporary>`, an unsupported borrow place) reports the real error and does
# NOT print a verifier cascade; and that a VALID program still compiles + runs
# (the verifier still guards the error-free path).
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/bad.kd" <<'EOF'
enum E { A(i64) }
fn val(e: &E) -> i64 { match e { A(v) => *v } }
fn main() -> i64 { val(&A(10)) }
EOF
set +e; out=$("$KARDC" "$TMP/bad.kd" 2>&1); rc=$?; set -e
[[ "$rc" -ne 0 ]] || { echo "FAIL [diag]: bad program compiled (rc 0)"; exit 1; }
grep -q "operand must be a binding" <<< "$out" || { echo "FAIL [diag]: real error missing; got: $out"; exit 1; }
if grep -qi "module verification failed" <<< "$out"; then
    echo "FAIL [diag]: cascading 'module verification failed' noise leaked through:"; echo "$out"; exit 1
fi
echo "PASS [diag]: a codegen error reports the REAL diagnostic, no verifier cascade"

cat > "$TMP/ok.kd" <<'EOF'
fn main() -> i64 { 6 * 7 }
EOF
got=$("$KARDC" "$TMP/ok.kd" 2>/dev/null | tail -n1); rc=$?
[[ "$rc" -eq 0 && "$got" == "42" ]] || { echo "FAIL [diag]: valid program broke (rc=$rc out=$got)"; exit 1; }
echo "PASS [diag]: a valid program still compiles + runs (verifier still guards the error-free path)"
echo "ALL DIAGNOSTICS SMOKE TESTS PASSED"
