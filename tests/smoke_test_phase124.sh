#!/usr/bin/env bash
# Phase 124 smoke test: `||` short-circuit logical-or.
#   - basic truth table (true||false, false||false, false||true);
#   - short-circuit: when lhs is true the rhs must NOT evaluate (a `10/0` in a
#     dead rhs must not trap);
#   - precedence: `||` binds looser than `&&`, so `t || f && f` is
#     `t || (f && f)` (true) and `f && f || t` is `(f && f) || t` (true) — a
#     tighter-`||` parse would make both false;
#   - the zero-param closure `|| body` is unaffected (it is a closure in
#     primary position, logical-or only in infix position).
# JIT + AOT.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Each passing check sets a distinct bit; a wrong/extra branch perturbs the sum
# diagnosably. Expected total = 1+2+4+8+16+32 = 63. The dead `false || false`
# branch adds 1000 only if `||` wrongly evaluated true.
cat > "$TMP/or.kd" <<'EOF'
fn main() -> i64 {
    let t = true;
    let f = false;
    let r1 = if t || f { 1 } else { 0 };                 // basic true        -> 1
    let r2 = if f || f { 1000 } else { 0 };              // basic false       -> 0
    let r3 = if t || ((10 / 0) == 0) { 2 } else { 0 };   // short-circuit rhs -> 2
    let r4 = if t || f && f { 4 } else { 0 };            // `||` looser: true -> 4
    let r5 = if f && f || t { 8 } else { 0 };            // `||` looser: true -> 8
    let g = || 16;                                       // zero-param closure
    let r6 = g();                                        //                   -> 16
    let r7 = if f || t { 32 } else { 0 };                // basic true        -> 32
    r1 + r2 + r3 + r4 + r5 + r6 + r7                     // 63
}
EOF
jit=$("$KARDC" "$TMP/or.kd")
[[ "$jit" != "63" ]] && { echo "FAIL [or/jit]: expected 63, got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/or" "$TMP/or.kd" >/dev/null
set +e; "$TMP/or" >/dev/null; rc=$?; set -e
[[ "$rc" -ne 63 ]] && { echo "FAIL [or/aot]: exit $rc (expected 63)"; exit 1; }
echo "PASS [or]: || truth table + short-circuit + precedence + closure — JIT + AOT"

# A bool-typed operand mismatch must be rejected (|| is bool->bool).
cat > "$TMP/bad.kd" <<'EOF'
fn main() -> i64 {
    if 1 || true { 0 } else { 1 }
}
EOF
if "$KARDC" "$TMP/bad.kd" >/dev/null 2>&1; then
    echo "FAIL [or/type]: `1 || true` should be a type error (i64 lhs)"; exit 1
fi
echo "PASS [or/type]: non-bool operand to || is rejected"

echo "PASS: Phase 124 — || short-circuit logical-or (precedence below &&, closure intact)"
