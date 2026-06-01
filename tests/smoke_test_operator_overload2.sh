#!/usr/bin/env bash
# v37 — full operator-trait surface (beyond Phase 184's Add/Sub/Mul/Div).
#
# Binary `%` and the bitwise/shift family (Rem/BitAnd/BitOr/BitXor/Shl/Shr) plus
# the UNARY operators Neg (`-x`) and Not (`!x`) are now overloadable on a user
# struct/enum by implementing the matching prelude trait; primitives keep their
# built-in ops. A missing impl is a clear diagnostic. Differential JIT vs AOT.
# (Index/Deref/custom-Output are deferred — see ROADMAP.)
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

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
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
    echo "$err" | grep -qi "$needle" || { echo "FAIL [$name]: expected '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# Binary % + bitwise/shift on a user type, alongside primitives.
diff_run binops $'2\n8\n13\n6\n16\n16\n2\n12' '
struct M { v: i64 }
impl Rem for M { fn rem(self, r: M) -> M { M { v: self.v % r.v } } }
impl BitAnd for M { fn bitand(self, r: M) -> M { M { v: self.v & r.v } } }
impl BitOr for M { fn bitor(self, r: M) -> M { M { v: self.v | r.v } } }
impl BitXor for M { fn bitxor(self, r: M) -> M { M { v: self.v ^ r.v } } }
impl Shl for M { fn shl(self, r: M) -> M { M { v: self.v << r.v } } }
impl Shr for M { fn shr(self, r: M) -> M { M { v: self.v >> r.v } } }
fn main() -> i64 ! { io } {
    print((M{v:17} % M{v:5}).v);   // 2
    print((M{v:12} & M{v:10}).v);  // 8
    print((M{v:12} | M{v:1}).v);   // 13
    print((M{v:12} ^ M{v:10}).v);  // 6
    print((M{v:1} << M{v:4}).v);   // 16
    print((M{v:64} >> M{v:2}).v);  // 16
    print(17 % 5);                  // 2 (primitive)
    print(8 | 4);                   // 12 (primitive)
    0
}
'

# Unary Neg + Not on user types, alongside primitives.
diff_run unops $'-3\n-7\n254\n-5\n0' '
struct V2 { x: i64, y: i64 }
impl Neg for V2 { fn neg(self) -> V2 { V2 { x: 0 - self.x, y: 0 - self.y } } }
struct Flags { bits: i64 }
impl Not for Flags { fn not(self) -> Flags { Flags { bits: self.bits ^ 255 } } }
fn main() -> i64 ! { io } {
    let n = -V2 { x: 3, y: 7 };
    print(n.x); print(n.y);          // -3, -7
    print((!Flags { bits: 1 }).bits); // 254
    print(-5);                        // -5 (primitive)
    print(0);
    0
}
'

# Operator chaining composes (user Rem then BitOr): 23%10=3, then 3|4=7.
diff_run chain $'7' '
struct M { v: i64 }
impl Rem for M { fn rem(self, r: M) -> M { M { v: self.v % r.v } } }
impl BitOr for M { fn bitor(self, r: M) -> M { M { v: self.v | r.v } } }
fn main() -> i64 ! { io } { print(((M{v:23} % M{v:10}) | M{v:4}).v); 0 }
'

# NEGATIVE: a `%` on a struct WITHOUT impl Rem is diagnosed.
expect_err no_rem 'implement `Rem`' '
struct W { x: i64 }
fn main() -> i64 { (W{x:5} % W{x:2}).x }
'
# NEGATIVE: unary `-` on a struct WITHOUT impl Neg is diagnosed.
expect_err no_neg 'impl Neg' '
struct W { x: i64 }
fn main() -> i64 { (-W{x:5}).x }
'

echo "ALL OPERATOR-SURFACE SMOKE TESTS PASSED"
