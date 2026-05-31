#!/usr/bin/env bash
# v25 Phase 135 smoke test: default trait methods. A trait method may carry a
# `{ body }`; an impl that doesn't override it inherits the default (the body is
# deep-cloned into the impl, with Self bound to the impl type). Covers: a default
# calling an abstract method, an override beating the default, chained defaults,
# and that a genuinely-abstract method is still required. JIT + AOT.
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

run_eq() { # name file want  (errexit stays off — the script captures rc itself)
    local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?
    [[ "$rc" -eq "$3" ]] || { echo "FAIL [$1/aot]: exit $rc want $3"; exit 1; }
}

# --- a default method calls an abstract one (Self bound to the impl type) ---
cat > "$TMP/def.kd" <<'EOF'
trait Greeter {
    fn base(&self) -> i64;
    fn greet(&self) -> i64 { self.base() + 100 }
}
struct Widget { v: i64 }
impl Greeter for Widget { fn base(&self) -> i64 { self.v } }
fn main() -> i64 { let w = Widget { v: 5 }; w.greet() }
EOF
run_eq default "$TMP/def.kd" 105
echo "PASS [default]: an inherited default method runs (105), JIT + AOT"

# --- an impl that overrides the default uses its own body ---
cat > "$TMP/ovr.kd" <<'EOF'
trait Greeter {
    fn base(&self) -> i64;
    fn greet(&self) -> i64 { self.base() + 100 }
}
struct Gizmo { v: i64 }
impl Greeter for Gizmo {
    fn base(&self) -> i64 { self.v }
    fn greet(&self) -> i64 { self.base() + 1 }
}
fn main() -> i64 { let g = Gizmo { v: 5 }; g.greet() }
EOF
run_eq override "$TMP/ovr.kd" 6
echo "PASS [override]: an explicit method overrides the default (6)"

# --- a default calling another default (chained) ---
cat > "$TMP/chain.kd" <<'EOF'
trait Chain {
    fn one(&self) -> i64;
    fn two(&self) -> i64 { self.one() * 2 }
    fn three(&self) -> i64 { self.two() + 1 }
}
struct Gadget { n: i64 }
impl Chain for Gadget { fn one(&self) -> i64 { self.n } }
fn main() -> i64 { let g = Gadget { n: 10 }; g.three() }
EOF
run_eq chained "$TMP/chain.kd" 21
echo "PASS [chained]: a default calling another default resolves (21)"

# --- a genuinely-abstract (bodyless) method is still required ---
cat > "$TMP/abs.kd" <<'EOF'
trait T { fn need(&self) -> i64; fn d(&self) -> i64 { 1 } }
struct Thing {}
impl T for Thing { }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" "$TMP/abs.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [abstract]: missing abstract method should error"; exit 1; }
grep -q "missing method 'need'" <<<"$out" || { echo "FAIL [abstract]: wrong error"; echo "$out"; exit 1; }
echo "PASS [abstract]: a bodyless method is still required of the impl"

echo "PASS: Phase 135 — default trait methods (inherit / override / chain), JIT + AOT"
