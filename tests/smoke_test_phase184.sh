#!/usr/bin/env bash
# v34 Phase 184 smoke test — operator overloading.
#
# A user type opts into an arithmetic operator by implementing the matching
# prelude trait — `Add` / `Sub` / `Mul` / `Div` (homogeneous, by-value:
# `fn add(self, rhs: Self) -> Self`). `a + b` on that type desugars (in the
# typechecker) to `<type>::add(a, b)`; primitives keep built-in arithmetic. A
# binary op on a struct WITHOUT the matching impl is a clear type error.
# Differentially gated JIT vs AOT.
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

diff_run() {
    local name="$1" expect="$2" src="$3"
    local n; n=$(printf '%s\n' "$expect" | wc -l | tr -d ' ')
    printf '%s' "$src" > "$TMP/$name.kd"
    local jit; jit=$("$KARDC" "$TMP/$name.kd" 2>/dev/null | head -n "$n") || true
    [[ "$jit" == "$expect" ]] || { echo "FAIL [$name/jit]: expected '$expect' got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd" >/dev/null 2>&1
    local aot; aot=$("$TMP/$name" 2>/dev/null | head -n "$n") || true
    [[ "$aot" == "$expect" ]] || { echo "FAIL [$name/aot]: expected '$expect' got '$aot'"; exit 1; }
    echo "PASS: $name"
}
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || {
        echo "FAIL [$name]: expected error containing '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# 1. Add + Mul on a 2-vector; primitive arithmetic still works alongside.
diff_run vec_add_mul $'11\n22\n6\n20\n8' '
struct Vec2 { x: i64, y: i64 }
impl Add for Vec2 { fn add(self, rhs: Vec2) -> Vec2 { Vec2 { x: self.x + rhs.x, y: self.y + rhs.y } } }
impl Mul for Vec2 { fn mul(self, rhs: Vec2) -> Vec2 { Vec2 { x: self.x * rhs.x, y: self.y * rhs.y } } }
fn main() -> i64 ! { io } {
    let c = Vec2 { x: 1, y: 2 } + Vec2 { x: 10, y: 20 };
    print(c.x); print(c.y);
    let d = Vec2 { x: 3, y: 4 } * Vec2 { x: 2, y: 5 };
    print(d.x); print(d.y);
    print(5 + 3);
    0
}
'

# 2. Sub + Div, and operator chaining ((a - b) - c via repeated +).
diff_run vec_sub_div_chain $'1\n2\n100' '
struct Money { cents: i64 }
impl Sub for Money { fn sub(self, r: Money) -> Money { Money { cents: self.cents - r.cents } } }
impl Div for Money { fn div(self, r: Money) -> Money { Money { cents: self.cents / r.cents } } }
impl Add for Money { fn add(self, r: Money) -> Money { Money { cents: self.cents + r.cents } } }
fn main() -> i64 ! { io } {
    print((Money { cents: 10 } - Money { cents: 9 }).cents);            // 1
    print((Money { cents: 12 } / Money { cents: 6 }).cents);            // 2
    print((Money { cents: 30 } + Money { cents: 30 } + Money { cents: 40 }).cents); // 100
    0
}
'

# 3. Operator methods are PURE — the prelude operator traits declare no effects,
#    so (per the effect-subset rule) an `impl Add` body must be pure too. A
#    chain of struct `+` composes naturally.
diff_run pure_operator $'60' '
struct N { v: i64 }
impl Add for N { fn add(self, r: N) -> N { N { v: self.v + r.v } } }
fn main() -> i64 ! { io } {
    let total = N { v: 10 } + N { v: 20 } + N { v: 30 };
    print(total.v);
    0
}
'

# 4. NEGATIVE: a binary op on a struct WITHOUT the matching impl is rejected.
expect_err no_impl 'implement `Add`' '
struct Widget { x: i64 }
fn main() -> i64 { let a = Widget { x: 1 }; let b = Widget { x: 2 }; (a + b).x }
'

echo "ALL PHASE 184 SMOKE TESTS PASSED"
