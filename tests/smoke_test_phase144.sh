#!/usr/bin/env bash
# v26 Phase 144 smoke test: top-level type aliases `type Name = Target;`,
# resolved to their target everywhere a TypeRef appears (typecheck AND codegen).
# Covers a scalar alias, an alias to a compound type, an alias behind `&`, a
# chained alias, and a cyclic alias error. JIT + AOT.
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
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq "$3" ]] || { echo "FAIL [$1/aot]: exit $rc want $3"; exit 1; }; }

cat > "$TMP/a.kd" <<'EOF'
type Meters = i64;
fn dist(a: Meters, b: Meters) -> Meters { a + b }
fn main() -> i64 { dist(20, 22) }
EOF
run_eq scalar "$TMP/a.kd" 42
echo "PASS [scalar]: type Meters = i64 used in a signature (42), JIT + AOT"

cat > "$TMP/b.kd" <<'EOF'
type Pair = (i64, i64);
fn first(p: Pair) -> i64 { match p { (a, b) => a } }
fn main() -> i64 { first((42, 7)) }
EOF
run_eq compound "$TMP/b.kd" 42
echo "PASS [compound]: type Pair = (i64, i64) (42)"

cat > "$TMP/c.kd" <<'EOF'
type Id = i64;
type Key = Id;
fn get(k: &Key) -> i64 { *k }
fn main() -> i64 { let k: Key = 9; get(&k) }
EOF
run_eq chained "$TMP/c.kd" 9
echo "PASS [chained]: a chained alias behind & resolves (9)"

cat > "$TMP/cyc.kd" <<'EOF'
type Loop1 = Loop2;
type Loop2 = Loop1;
fn main() -> i64 { let x: Loop1 = 1; x }
EOF
out=$("$KARDC" "$TMP/cyc.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [cyclic]: a cyclic alias should error"; exit 1; }
grep -q "cyclic type alias" <<<"$out" || { echo "FAIL [cyclic]: wrong error"; echo "$out"; exit 1; }
echo "PASS [cyclic]: a cyclic type alias is rejected"

echo "PASS: Phase 144 — type aliases (type Name = Target)"
