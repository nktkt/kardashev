#!/usr/bin/env bash
# v38 — object-safety (dyn-safety) completeness.
#
# A trait used as `dyn Trait` must be object-safe: every method has a `self`
# receiver, and no method returns `Self` by value or takes a `Self`-by-value
# (non-receiver) parameter (the concrete size is erased through the trait
# object). Object-safe traits dispatch correctly through `&dyn`; non-object-safe
# uses are rejected with a diagnostic NAMING the offending method.
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

# Object-safe trait: multiple impls dispatched through &dyn.
diff_run dispatch $'1\n2\n3' '
trait Speak { fn say(&self) -> i64; }
struct Dog {}
struct Cat {}
struct Fox {}
impl Speak for Dog { fn say(&self) -> i64 { 1 } }
impl Speak for Cat { fn say(&self) -> i64 { 2 } }
impl Speak for Fox { fn say(&self) -> i64 { 3 } }
fn ask(s: &dyn Speak) -> i64 { s.say() }
fn main() -> i64 ! { io } { print(ask(&Dog{})); print(ask(&Cat{})); print(ask(&Fox{})); 0 }
'

# Object-safe with multiple methods.
diff_run multi $'20' '
trait Shape { fn area(&self) -> i64; fn perimeter(&self) -> i64; }
struct Rect { w: i64, h: i64 }
impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } fn perimeter(&self) -> i64 { 2 * (self.w + self.h) } }
fn describe(s: &dyn Shape) -> i64 { s.area() + s.perimeter() }
fn main() -> i64 ! { io } { print(describe(&Rect{w:4,h:2})); 0 }   // 8 + 12 = 20
'

# NEGATIVE: a static (no-self) method makes the trait non-object-safe.
expect_err no_self 'not dyn-safe' '
trait Factory { fn create() -> i64; fn use_self(&self) -> i64; }
struct F {}
impl Factory for F { fn create() -> i64 { 0 } fn use_self(&self) -> i64 { 1 } }
fn go(f: &dyn Factory) -> i64 { f.use_self() }
fn main() -> i64 { 0 }
'
# NEGATIVE: returning Self by value.
expect_err self_ret 'returns .Self. by value' '
trait Maker { fn make(&self) -> Self; }
struct Widget { x: i64 }
impl Maker for Widget { fn make(&self) -> Widget { Widget { x: self.x } } }
fn go(m: &dyn Maker) -> i64 { 0 }
fn main() -> i64 { 0 }
'
# NEGATIVE: a Self-by-value (non-receiver) parameter.
expect_err self_param 'Self.-by-value parameter' '
trait Cmp2 { fn eq2(&self, other: Self) -> bool; }
struct Widget { x: i64 }
impl Cmp2 for Widget { fn eq2(&self, other: Widget) -> bool { self.x == other.x } }
fn go(c: &dyn Cmp2) -> i64 { 0 }
fn main() -> i64 { 0 }
'

echo "ALL OBJECT-SAFETY SMOKE TESTS PASSED"
