#!/usr/bin/env bash
# Phase 60 smoke test (Roadmap v10): the EFFECT-SUBSET RULE — a trait impl
# method's effects must be a SUBSET of the trait method's declared effects, so
# dyn/generic effect attribution is sound by construction.
#   1. A super-effecting impl (impl declares `io` the trait method doesn't) is
#      a COMPILE ERROR.
#   2. A subset impl (impl pure, trait permits `io`) compiles + runs.
#   3. `Drop` is exempt (statically-resolved drop glue, never dyn-dispatched):
#      a `Drop` impl whose `drop` does `io` compiles.
#   4. The prelude is made honest: `Eq::eq` / `Iterator::next` / `Display::
#      to_string` declare `! { alloc }`, so a generic `<T: Eq>` call attributes
#      alloc — and a pure (Range) for-loop stays pure (concrete attribution).
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

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rejects() { # name file needle
    local n=$1 f=$2 needle=$3 out
    set +e; out=$("$KARDC" "$f" 2>&1); set -e
    if "$KARDC" "$f" >/dev/null 2>&1; then
        echo "FAIL [$n]: expected REJECTION, but it compiled"; exit 1
    fi
    if [[ -n "$needle" ]] && ! grep -qi "$needle" <<<"$out"; then
        echo "FAIL [$n]: rejected but missing '$needle'; got: $out"; exit 1
    fi
    echo "PASS [$n]: rejected as expected"
}

runs() { # name file expected
    local n=$1 f=$2 want=$3 got
    got=$("$KARDC" "$f" 2>/dev/null | tail -1)
    [[ "$got" == "$want" ]] || { echo "FAIL [$n]: expected $want got '$got'"; exit 1; }
    echo "PASS [$n]: JIT=$got"
}

# 1. super-effecting impl: a pure trait method, an `io` impl.
cat > "$TMP/super.kd" <<'EOF'
trait Greet { fn greet(&self) -> i64; }
struct S {}
impl Greet for S { fn greet(&self) -> i64 ! { io } { print(1); 0 } }
fn main() -> i64 { 0 }
EOF
rejects super-effecting "$TMP/super.kd" "subset of the trait"

# 2. subset impl: trait permits io, impl is pure (fewer effects → allowed).
cat > "$TMP/subset.kd" <<'EOF'
trait Greet { fn greet(&self) -> i64 ! { io }; }
struct S {}
impl Greet for S { fn greet(&self) -> i64 { 7 } }
fn main() -> i64 { let s = S {}; s.greet() }
EOF
runs subset-ok "$TMP/subset.kd" 7

# 3. Drop is exempt — drop glue is statically resolved, never dyn-dispatched.
cat > "$TMP/drop.kd" <<'EOF'
trait Drop { fn drop(&mut self); }
struct Noisy { id: i64 }
impl Drop for Noisy { fn drop(&mut self) ! { io } { print(self.id); } }
fn main() -> i64 ! { io } { let n = Noisy { id: 5 }; 0 }
EOF
"$KARDC" "$TMP/drop.kd" >/dev/null 2>&1 \
    || { echo "FAIL [drop-exempt]: a Drop impl doing io must compile"; exit 1; }
echo "PASS [drop-exempt]: Drop impl with io compiles (drop glue is static)"

# 4a. a generic `<T: Eq>` call attributes the trait's alloc — must be declared.
cat > "$TMP/eqgen.kd" <<'EOF'
fn same<T: Eq>(a: T, b: T) -> bool ! { alloc } { a.eq(&b) }
fn main() -> i64 ! { alloc } {
    if same(int_to_string(5), int_to_string(5)) { 1 } else { 0 }
}
EOF
runs eq-generic-alloc "$TMP/eqgen.kd" 1

# 4b. a pure Range for-loop stays pure (concrete next() attribution).
printf 'fn main() -> i64 { let mut s = 0; for i in 0..5 { s = s + i; } s }\n' \
    > "$TMP/pureloop.kd"
runs pure-range-loop "$TMP/pureloop.kd" 10

echo "ALL PHASE 60 SMOKE TESTS PASSED"
