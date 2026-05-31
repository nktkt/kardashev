#!/usr/bin/env bash
# v25 Phase 140 smoke test: the standard conversion-trait vocabulary. The prelude
# now provides `From<T>` (static — `Target::from(x)`) and `Into<U>`
# (`x.into()`), generic over source/target like `Iterator<T>`. A type opts in by
# impl'ing either. JIT + AOT.
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

# --- From builds a value, Into consumes one (JIT + AOT) ---
cat > "$TMP/conv.kd" <<'EOF'
struct Celsius { v: i64 }
impl From<i64> for Celsius { fn from(x: i64) -> Self { Celsius { v: x } } }
impl Into<i64> for Celsius { fn into(self) -> i64 { self.v + 10 } }
fn main() -> i64 {
    let c = Celsius::from(20);
    c.into()
}
EOF
jit=$("$KARDC" "$TMP/conv.kd" 2>&1 | head -1)
[[ "$jit" == "30" ]] || { echo "FAIL [conv/jit]: want 30 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/conv" "$TMP/conv.kd" >/dev/null 2>&1 || { echo "FAIL [conv/aot]: compile"; exit 1; }
"$TMP/conv" >/dev/null; rc=$?
[[ "$rc" -eq 30 ]] || { echo "FAIL [conv/aot]: exit $rc want 30"; exit 1; }
echo "PASS [conv]: From::from builds + Into::into consumes — JIT + AOT (30)"

# --- From alone is usable as a named constructor ---
cat > "$TMP/from.kd" <<'EOF'
struct Wrap { v: i64 }
impl From<i64> for Wrap { fn from(x: i64) -> Self { Wrap { v: x * 2 } } }
fn main() -> i64 { let w = Wrap::from(21); w.v }
EOF
got=$("$KARDC" "$TMP/from.kd" 2>&1 | head -1)
[[ "$got" == "42" ]] || { echo "FAIL [from]: want 42 got '$got'"; exit 1; }
echo "PASS [from]: From<i64> works as a named constructor (Wrap::from(21).v = 42)"

# --- a user may shadow From/Into with their own (prelude is suppressed) ---
cat > "$TMP/user.kd" <<'EOF'
trait From<T> { fn from(x: T) -> Self; }
struct Id { v: i64 }
impl From<i64> for Id { fn from(x: i64) -> Self { Id { v: x } } }
fn main() -> i64 { let i = Id::from(7); i.v }
EOF
got=$("$KARDC" "$TMP/user.kd" 2>&1 | head -1)
[[ "$got" == "7" ]] || { echo "FAIL [shadow]: user-defined From should work (want 7 got '$got')"; exit 1; }
echo "PASS [shadow]: a user-defined From suppresses the prelude one"

echo "PASS: Phase 140 — standard conversion vocabulary (From<T> / Into<U>)"
