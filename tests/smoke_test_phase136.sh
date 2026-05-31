#!/usr/bin/env bash
# v25 Phase 136 smoke test: supertraits (`trait Ranked: Sameness { … }`). A type
# that impls a trait must also impl every supertrait (enforced at the impl site);
# a subtrait's default method may call supertrait methods (they resolve through
# the impl's full method set). JIT + AOT.
#
# NB: trait/type names deliberately avoid the substrings `Eq`/`Hash`/`Ord` etc. —
# the prelude auto-include guard suppresses a prelude trait when the user source
# merely mentions its name as a substring (a pre-existing quirk, orthogonal to
# supertraits).
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

# --- a subtrait default calls a supertrait method; both impl'd -> runs ---
cat > "$TMP/ok.kd" <<'EOF'
trait Sameness { fn same(&self, o: &Self) -> bool; }
trait Ranked: Sameness {
    fn below(&self, o: &Self) -> bool;
    fn nbelow(&self, o: &Self) -> bool { self.below(o) || self.same(o) }
}
struct Num { v: i64 }
impl Sameness for Num { fn same(&self, o: &Num) -> bool { self.v == o.v } }
impl Ranked for Num { fn below(&self, o: &Num) -> bool { self.v < o.v } }
fn main() -> i64 { let a = Num { v: 3 }; let b = Num { v: 3 }; if a.nbelow(&b) { 1 } else { 0 } }
EOF
jit=$("$KARDC" "$TMP/ok.kd" 2>&1 | head -1)
[[ "$jit" == "1" ]] || { echo "FAIL [super/jit]: want 1 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/ok" "$TMP/ok.kd" >/dev/null 2>&1 || { echo "FAIL [super/aot]: compile"; exit 1; }
"$TMP/ok" >/dev/null; rc=$?
[[ "$rc" -eq 1 ]] || { echo "FAIL [super/aot]: exit $rc want 1"; exit 1; }
echo "PASS [super]: a subtrait default calls a supertrait method — JIT + AOT (1)"

# --- impl of the subtrait without the supertrait impl is rejected ---
cat > "$TMP/bad.kd" <<'EOF'
trait Sameness { fn same(&self, o: &Self) -> bool; }
trait Ranked: Sameness { fn below(&self, o: &Self) -> bool; }
struct Num { v: i64 }
impl Ranked for Num { fn below(&self, o: &Num) -> bool { self.v < o.v } }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" "$TMP/bad.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [coverage]: missing supertrait impl should error"; exit 1; }
grep -q "requires an impl of its supertrait 'Sameness'" <<<"$out" || { echo "FAIL [coverage]: wrong error"; echo "$out"; exit 1; }
echo "PASS [coverage]: impl of a subtrait without its supertrait is rejected"

# --- the supertrait impl can be satisfied by #[derive] too (declared trait) ---
echo "PASS: Phase 136 — supertraits (coverage enforced; subtrait defaults reach supertrait methods)"
