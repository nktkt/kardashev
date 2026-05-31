#!/usr/bin/env bash
# v26 Phase 145 smoke test: the Fn / FnMut / FnOnce closure-trait hierarchy.
# A parameter may be spelled `Fn(A) -> R` / `FnMut(A) -> R` / `FnOnce(A) -> R`
# (a callable bound). Every closure is classified by how it uses its captures:
# Fn (reads only), FnMut (mutates a captured binding), FnOnce (moves a capture
# out). A passed closure must satisfy the bound under Fn < FnMut < FnOnce — an
# `Fn` closure satisfies every bound; an `FnMut` closure is rejected where an
# `Fn` is required. The bound is compile-time only (same fat-pointer ABI), so
# accepted programs run identically on JIT + AOT.
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
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq "$3" ]] || { echo "FAIL [$1/aot]: exit $rc want $3"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) an Fn closure (reads its capture) satisfies an `Fn` bound.
cat > "$TMP/fn.kd" <<'EOF'
fn apply(f: Fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 { let n = 10; apply(|y| y + n, 32) }
EOF
run_eq fn_bound "$TMP/fn.kd" 42 "Fn closure satisfies an Fn(..) bound (42)"

# 2) an FnMut closure (mutates a capture) satisfies an `FnMut` bound.
cat > "$TMP/fnmut.kd" <<'EOF'
fn run3(f: FnMut(i64) -> i64) -> i64 { f(1); f(1); f(1) }
fn main() -> i64 { let mut total = 0; run3(|x| { total = total + x; total }) }
EOF
run_eq fnmut_bound "$TMP/fnmut.kd" 3 "FnMut closure satisfies an FnMut(..) bound (1+1+1=3)"

# 3) Fn < FnMut: a plain Fn closure also satisfies an `FnMut` bound (widening).
cat > "$TMP/widen.kd" <<'EOF'
fn run(f: FnMut(i64) -> i64) -> i64 { f(20) }
fn main() -> i64 { let k = 22; run(|x| x + k) }
EOF
run_eq widen "$TMP/widen.kd" 42 "Fn closure widens to an FnMut(..) bound (42)"

# 4) FnOnce is the top of the lattice: any closure satisfies an `FnOnce` bound.
cat > "$TMP/once.kd" <<'EOF'
fn consume(f: FnOnce(i64) -> i64) -> i64 { f(40) }
fn main() -> i64 { let k = 2; consume(|x| x + k) }
EOF
run_eq fnonce_bound "$TMP/once.kd" 42 "any closure satisfies an FnOnce(..) bound (42)"

# 5) an io-effecting unit-returning bound spelled without `-> R`
#    (`Fn(i64) ! { io }` — the row follows the params, no return type).
cat > "$TMP/unit.kd" <<'EOF'
fn call(f: Fn(i64) ! { io }) ! { io } { f(7) }
fn main() -> i64 ! { io } {
    let base = 5;
    call(|x| { print(x + base); });
    0
}
EOF
out5=$("$KARDC" "$TMP/unit.kd" 2>&1 | head -1)
[[ "$out5" == "12" ]] || { echo "FAIL [unit_ret]: want 12 got '$out5'"; exit 1; }
echo "PASS [unit_ret]: Fn(i64) ! { io } with no -> R reads as -> () (printed 12)"

# 6) REJECT: an FnMut closure where an `Fn` is required.
cat > "$TMP/bad.kd" <<'EOF'
fn apply(f: Fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 { let mut c = 0; apply(|y| { c = c + 1; y + c }, 41) }
EOF
out=$("$KARDC" "$TMP/bad.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [reject]: FnMut-where-Fn should be a type error"; exit 1; }
echo "$out" | grep -qi "FnMut" || { echo "FAIL [reject]: message should mention FnMut; got: $out"; exit 1; }
echo "$out" | grep -qi "is required here" || { echo "FAIL [reject]: message should say required; got: $out"; exit 1; }
echo "PASS [reject]: an FnMut closure is rejected where an Fn(..) is required"

echo "PASS: Phase 145 — Fn/FnMut/FnOnce closure-trait hierarchy + capture classification"
