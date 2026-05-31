#!/usr/bin/env bash
# v25 Phase 138 smoke test: coherence / overlap checking. Two impls of the same
# trait for the same type are rejected — whether two explicit `impl Tr for X`,
# or two overlapping blanket impls that each synthesize a concrete `impl Tr for
# X`. A program with one impl per (trait, type) — including a single generic
# impl — is unaffected (the key spells type + trait args, so no false positive).
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

expect_conflict() { # name file
    local out rc
    out=$("$KARDC" "$2" 2>&1); rc=$?
    [[ "$rc" -ne 0 ]] || { echo "FAIL [$1]: should be a coherence error"; exit 1; }
    grep -q "conflicting implementations of trait" <<<"$out" || { echo "FAIL [$1]: wrong error"; echo "$out"; exit 1; }
}

# --- two explicit impls of the same trait for the same type ---
cat > "$TMP/dup.kd" <<'EOF'
trait Painter { fn render(&self) -> i64; }
struct Dog { v: i64 }
impl Painter for Dog { fn render(&self) -> i64 { 1 } }
impl Painter for Dog { fn render(&self) -> i64 { 2 } }
fn main() -> i64 { let d = Dog { v: 0 }; d.render() }
EOF
expect_conflict explicit "$TMP/dup.kd"
echo "PASS [explicit]: two explicit impls of a trait for a type are rejected"

# --- two overlapping blanket impls ---
cat > "$TMP/ov.kd" <<'EOF'
trait Loud { fn shout(&self) -> i64; }
trait Painter { fn render(&self) -> i64; }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { 1 } }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { 2 } }
struct Dog { v: i64 }
impl Loud for Dog { fn shout(&self) -> i64 { self.v } }
fn main() -> i64 { let d = Dog { v: 0 }; d.render() }
EOF
expect_conflict blanket "$TMP/ov.kd"
echo "PASS [blanket]: two overlapping blanket impls are rejected"

# --- a clean program (one impl per (trait,type), incl. a blanket) is unaffected ---
cat > "$TMP/ok.kd" <<'EOF'
trait Loud { fn shout(&self) -> i64; }
trait Painter { fn render(&self) -> i64; }
impl<T: Loud> Painter for T { fn render(&self) -> i64 { self.shout() + 100 } }
struct Widget { v: i64 }
impl Loud for Widget { fn shout(&self) -> i64 { self.v } }
fn main() -> i64 { let w = Widget { v: 5 }; w.render() }
EOF
got=$("$KARDC" "$TMP/ok.kd" 2>&1 | head -1)
[[ "$got" == "105" ]] || { echo "FAIL [clean]: a coherent program should compile (want 105 got '$got')"; exit 1; }
echo "PASS [clean]: one impl per (trait,type) (incl. a blanket) compiles, no false conflict (105)"

echo "PASS: Phase 138 — coherence (conflicting impls rejected; precise per-instantiation key)"
