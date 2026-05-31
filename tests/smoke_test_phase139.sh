#!/usr/bin/env bash
# v25 Phase 139 smoke test: associated consts. `const N: T;` in a trait and
# `const N: T = expr;` in an impl declare an associated constant; it is read as
# `Type::N()` (it desugars to a no-self method, reusing the existing static-
# method path resolution). JIT + AOT.
#
# Scope (honest, documented): access is call-style `Type::N()`; `Self::N()` from
# within a method is a separate gap (static-Self path resolution), not required.
set -uo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# --- an impl associated const, read via Type::N(), drives logic ---
cat > "$TMP/ac.kd" <<'EOF'
trait Bounded { const MAX: i64; fn cap(&self) -> i64; }
struct Gauge { v: i64 }
impl Bounded for Gauge {
    const MAX: i64 = 42;
    fn cap(&self) -> i64 { if self.v > Gauge::MAX() { Gauge::MAX() } else { self.v } }
}
fn main() -> i64 { let g = Gauge { v: 100 }; g.cap() }
EOF
jit=$("$KARDC" "$TMP/ac.kd" 2>&1 | head -1)
[[ "$jit" == "42" ]] || { echo "FAIL [const/jit]: want 42 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/ac" "$TMP/ac.kd" >/dev/null 2>&1 || { echo "FAIL [const/aot]: compile"; exit 1; }
"$TMP/ac" >/dev/null; rc=$?
[[ "$rc" -eq 42 ]] || { echo "FAIL [const/aot]: exit $rc want 42"; exit 1; }
echo "PASS [const]: an associated const (Gauge::MAX() = 42) drives logic — JIT + AOT"

# --- an impl that omits the trait's associated const is rejected ---
cat > "$TMP/miss.kd" <<'EOF'
trait Bounded { const MAX: i64; }
struct Gauge { v: i64 }
impl Bounded for Gauge { }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" "$TMP/miss.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [missing]: omitting the assoc const should error"; exit 1; }
grep -q "missing method 'MAX'" <<<"$out" || { echo "FAIL [missing]: wrong error"; echo "$out"; exit 1; }
echo "PASS [missing]: an impl that omits the trait's associated const is rejected"

# --- a second impl can choose a different value ---
cat > "$TMP/two.kd" <<'EOF'
trait Bounded { const MAX: i64; }
struct Low { v: i64 }
struct High { v: i64 }
impl Bounded for Low { const MAX: i64 = 10; }
impl Bounded for High { const MAX: i64 = 20; }
fn main() -> i64 { Low::MAX() + High::MAX() }
EOF
got=$("$KARDC" "$TMP/two.kd" 2>&1 | head -1)
[[ "$got" == "30" ]] || { echo "FAIL [perimpl]: want 30 got '$got'"; exit 1; }
echo "PASS [perimpl]: each impl supplies its own associated const value (10 + 20 = 30)"

echo "PASS: Phase 139 — associated consts (const N: T in trait/impl, read as Type::N())"
