#!/usr/bin/env bash
# v28 Phase 155: generic associated types (GATs). A trait associated type may
# carry its own generic params — `type Out<T>;` — bound in an impl as
# `type Out<T> = Pair<T, T>;`, and projected in a (concrete-Self) impl method
# signature as `Self::Out<i64>`, which resolves to `Pair<i64, i64>`. JIT + AOT.
# (A GAT projection on a bounded generic param, `C::Out<i64>`, is a documented
# follow-on — it needs the args threaded through monomorphization.)
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
run_eq() { local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq $(( $3 & 255 )) ]] || { echo "FAIL [$1/aot]: exit $rc want $(( $3 & 255 ))"; exit 1; }
    echo "PASS [$1]: $4"; }
expect_err() { local out; out=$("$KARDC" "$2" 2>&1)
    [[ $? -ne 0 ]] || { echo "FAIL [$1]: expected error, compiled"; exit 1; }
    echo "$out" | grep -qiE "$3" || { echo "FAIL [$1]: want /$3/, got: $out"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) a GAT bound to a two-param struct, projected as Self::Out<i64> -> Pair<i64,i64>
cat > "$TMP/g.kd" <<'EOF'
struct Pair<A, B> { a: A, b: B }
trait Wrap { type Out<T>; fn make(&self, x: i64) -> Self::Out<i64>; }
struct Gen { n: i64 }
impl Wrap for Gen {
    type Out<T> = Pair<T, T>;
    fn make(&self, x: i64) -> Self::Out<i64> { Pair { a: x, b: x + 1 } }
}
fn main() -> i64 { let g = Gen { n: 0 }; let p = g.make(20); p.a * 10 + p.b }
EOF
run_eq gat_pair "$TMP/g.kd" 221 "type Out<T> = Pair<T,T>; Self::Out<i64> -> Pair<i64,i64> (20,21 -> 221)"

# 2) a GAT bound to a one-param wrapper, used through a let annotation
cat > "$TMP/w.kd" <<'EOF'
struct Wrapper<T> { v: T }
trait Make { type W<T>; fn one(&self) -> Self::W<i64>; }
struct M {}
impl Make for M { type W<T> = Wrapper<T>; fn one(&self) -> Self::W<i64> { Wrapper { v: 99 } } }
fn main() -> i64 { let m = M {}; let w: Wrapper<i64> = m.one(); w.v }
EOF
run_eq gat_wrapper "$TMP/w.kd" 99 "type W<T> = Wrapper<T>; Self::W<i64> -> Wrapper<i64> (99)"

# 3) a plain (Phase 21b) associated type still works alongside the GAT machinery
cat > "$TMP/p.kd" <<'EOF'
trait Container { type Item; fn first(&self) -> Self::Item; }
struct IntBox { v: i64 }
impl Container for IntBox { type Item = i64; fn first(&self) -> Self::Item { self.v } }
fn main() -> i64 { let b = IntBox { v: 42 }; b.first() }
EOF
run_eq plain_assoc "$TMP/p.kd" 42 "a non-generic associated type (Phase 21b) still resolves (42)"

# 4) a GAT projection supplying the wrong number of args is a compile error
cat > "$TMP/m.kd" <<'EOF'
struct Pair<A, B> { a: A, b: B }
trait Wrap { type Out<T>; fn make(&self) -> Self::Out<i64, bool>; }
struct Gen {}
impl Wrap for Gen { type Out<T> = Pair<T, T>; fn make(&self) -> Self::Out<i64, bool> { Pair { a: 1, b: 2 } } }
fn main() -> i64 { 0 }
EOF
expect_err gat_arity "$TMP/m.kd" "expects 1 type argument|generic associated type" "GAT arity mismatch is rejected"

echo "PASS: Phase 155 — generic associated types (GATs)"
