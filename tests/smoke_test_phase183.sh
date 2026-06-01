#!/usr/bin/env bash
# v34 Phase 183 smoke test — user-defined `#[derive(...)]`.
#
# A library author writes a derive as a `macro_rules! derive_Foo` whose
# matcher destructures the item — e.g. `(struct $name:ident { $($f:ident :
# $t:ty),* })` — and whose body emits an `impl`. Annotating a struct with
# `#[derive(Foo)]` then synthesizes a `derive_Foo! { <the struct> }`
# invocation (right after the struct), which expands into the impl. This
# builds directly on the Phase 182 macro engine — in particular the matcher's
# ability to descend into the literal `{ }` to reach the field repetition.
# Built-in derives (Clone/Eq/Debug/…) still work alongside user ones.
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

# 1. A user derive that sums every field — the matcher's `$($f:ident :
#    $t:ty),*` reaches inside the struct braces; the body folds over the fields.
diff_run derive_field_sum $'42' '
trait FieldSum { fn field_sum(&self) -> i64; }
macro_rules! derive_FieldSum {
    (struct $name:ident { $($field:ident : $ty:ty),* }) => {
        impl FieldSum for $name {
            fn field_sum(&self) -> i64 { 0 $( + self.$field )* }
        }
    };
}
#[derive(FieldSum)]
struct Point { x: i64, y: i64, z: i64 }
fn main() -> i64 ! { io } {
    let p = Point { x: 10, y: 20, z: 12 };
    print(p.field_sum());
    0
}
'

# 2. A user derive on a struct written with a TRAILING comma in its field list
#    (the repetition tolerates the trailing separator).
diff_run derive_trailing_comma $'7\n12' '
trait Count { fn count(&self) -> i64; }
macro_rules! derive_Count {
    (struct $name:ident { $($f:ident : $t:ty),* }) => {
        impl Count for $name { fn count(&self) -> i64 { 0 $( + self.$f )* } }
    };
}
#[derive(Count)]
struct Rec { a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, g: i64, }
fn main() -> i64 ! { io } {
    let r = Rec { a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1 };
    print(r.count());        // 7 fields
    print(r.a + r.g + 10);   // 12
    0
}
'

# 3. A user derive coexists with the BUILT-IN derive(Clone) on the same struct.
diff_run derive_with_builtin $'14\n7' '
trait Doubled { fn doubled(&self) -> i64; }
macro_rules! derive_Doubled {
    (struct $name:ident { $($f:ident : $t:ty),* }) => {
        impl Doubled for $name { fn doubled(&self) -> i64 { (0 $( + self.$f )*) * 2 } }
    };
}
#[derive(Clone, Doubled)]
struct Widget { a: i64, b: i64 }
fn main() -> i64 ! { io, alloc } {
    let v = Widget { a: 3, b: 4 };
    let w = v.clone();       // built-in derive(Clone)
    print(w.doubled());      // user derive(Doubled): (3+4)*2 = 14
    print(v.a + w.b);        // 7
    0
}
'

echo "ALL PHASE 183 SMOKE TESTS PASSED"
