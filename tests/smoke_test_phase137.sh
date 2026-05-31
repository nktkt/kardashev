#!/usr/bin/env bash
# v25 Phase 137 smoke test: blanket impls. `impl<T: Loud> Painter for T` gives
# Painter to every type that impls Loud. Implemented by expanding the blanket
# into concrete `impl Painter for X` for each eligible user type X (method bodies
# deep-cloned, Self -> X). Covers: a bound-satisfying type gains the method; a
# non-satisfying type does not; an explicit impl beats the blanket. JIT + AOT.
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

# --- a type that impls the bound gains the blanket method (JIT + AOT) ---
cat > "$TMP/ok.kd" <<'EOF'
trait Loud { fn shout(&self) -> i64; }
trait Painter { fn render(&self) -> i64; }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { self.shout() + 100 } }
struct Dog { v: i64 }
impl Loud for Dog { fn shout(&self) -> i64 { self.v } }
fn main() -> i64 { let d = Dog { v: 7 }; d.render() }
EOF
jit=$("$KARDC" "$TMP/ok.kd" 2>&1 | head -1)
[[ "$jit" == "107" ]] || { echo "FAIL [blanket/jit]: want 107 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/ok" "$TMP/ok.kd" >/dev/null 2>&1 || { echo "FAIL [blanket/aot]: compile"; exit 1; }
"$TMP/ok" >/dev/null; rc=$?
[[ "$rc" -eq 107 ]] || { echo "FAIL [blanket/aot]: exit $rc want 107"; exit 1; }
echo "PASS [blanket]: a Loud type gains Painter::render via the blanket (107), JIT + AOT"

# --- a type that does NOT impl the bound does not get the method ---
cat > "$TMP/no.kd" <<'EOF'
trait Loud { fn shout(&self) -> i64; }
trait Painter { fn render(&self) -> i64; }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { self.shout() + 100 } }
struct Cat { v: i64 }
fn main() -> i64 { let c = Cat { v: 1 }; c.render() }
EOF
out=$("$KARDC" "$TMP/no.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [bound]: a non-Loud type should not get render"; exit 1; }
grep -qiE "no impl|method 'render'|unknown" <<<"$out" || { echo "FAIL [bound]: wrong error"; echo "$out"; exit 1; }
echo "PASS [bound]: a type that doesn't satisfy the bound does not gain the method"

# --- an explicit impl beats the blanket ---
cat > "$TMP/ovr.kd" <<'EOF'
trait Loud { fn shout(&self) -> i64; }
trait Painter { fn render(&self) -> i64; }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { self.shout() + 100 } }
struct Fox { v: i64 }
impl Loud for Fox { fn shout(&self) -> i64 { self.v } }
impl Painter for Fox { fn render(&self) -> i64 { self.shout() + 1 } }
fn main() -> i64 { let f = Fox { v: 7 }; f.render() }
EOF
got=$("$KARDC" "$TMP/ovr.kd" 2>&1 | head -1)
[[ "$got" == "8" ]] || { echo "FAIL [override]: explicit impl should win (want 8 got '$got')"; exit 1; }
echo "PASS [override]: an explicit impl beats the blanket (8)"

echo "PASS: Phase 137 — blanket impls (impl<T: Bound> Trait for T)"
