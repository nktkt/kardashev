#!/usr/bin/env bash
# Phase 11 smoke test: dyn Trait + vtable-based dynamic dispatch, end to end
# through JIT + AOT.
#
#   - ONE `describe(s: &dyn Shape)` call site dispatches to two different
#     impls at runtime (Sq -> 25, Rect -> 12). Proves genuine runtime
#     dispatch through the trait object's vtable, not monomorphized copies.
#   - `Box<dyn Shape>` carries a heap-owned trait object; `.area()` dispatches
#     through its vtable.
#   - A multi-method trait calls the 2nd vtable slot and gets the right method
#     (slot indexing is correct).
#   - dyn-safety negative: a trait with a no-`self` (static) method is rejected
#     when used as `dyn`.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- Acceptance: one call site, two runtime impls (25 + 12 == 37). ---
cat > "$TMP/dyn.kd" <<'EOF'
trait Shape { fn area(&self) -> i64; }
struct Sq { side: i64 }
struct Rect { w: i64, h: i64 }
impl Shape for Sq   { fn area(&self) -> i64 { self.side * self.side } }
impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } }

fn describe(s: &dyn Shape) -> i64 { s.area() }

fn main() -> i64 {
    let sq = Sq { side: 5 };
    let r  = Rect { w: 3, h: 4 };
    let a = describe(&sq);
    let b = describe(&r);
    a + b
}
EOF

# kardc with no -o JIT-runs main and prints its i64 return value.
JIT_OUT=$("$KARDC" "$TMP/dyn.kd" | tail -n 1)
if [[ "$JIT_OUT" != "37" ]]; then
    echo "FAIL: JIT dyn dispatch expected 37 (25+12), got: $JIT_OUT"
    exit 1
fi
echo "JIT: describe(&sq)=25, describe(&r)=12, sum=37 (one call site, two impls)"

# AOT: main's return value is the process exit code.
"$KARDC" -o "$TMP/dyn_prog" "$TMP/dyn.kd"
set +e
"$TMP/dyn_prog"
AOT_RC=$?
set -e
if [[ "$AOT_RC" -ne 37 ]]; then
    echo "FAIL: AOT dyn dispatch expected exit 37, got: $AOT_RC"
    exit 1
fi
echo "AOT: dyn dispatch exits 37"

# --- Box<dyn Shape>: heap-owned trait object (36). ---
cat > "$TMP/box.kd" <<'EOF'
trait Shape { fn area(&self) -> i64; }
struct Sq { side: i64 }
impl Shape for Sq { fn area(&self) -> i64 { self.side * self.side } }
fn main() -> i64 {
    let b: Box<dyn Shape> = Box::new(Sq { side: 6 });
    b.area()
}
EOF
BOX_JIT=$("$KARDC" "$TMP/box.kd" | tail -n 1)
if [[ "$BOX_JIT" != "36" ]]; then
    echo "FAIL: Box<dyn Shape> JIT expected 36, got: $BOX_JIT"
    exit 1
fi
"$KARDC" -o "$TMP/box_prog" "$TMP/box.kd"
set +e
"$TMP/box_prog"
BOX_RC=$?
set -e
if [[ "$BOX_RC" -ne 36 ]]; then
    echo "FAIL: Box<dyn Shape> AOT expected exit 36, got: $BOX_RC"
    exit 1
fi
echo "Box<dyn Shape>: Box::new(Sq{side:6}).area() == 36 (JIT + AOT)"

# --- Multi-method trait: 2nd slot dispatches correctly. ---
cat > "$TMP/multi.kd" <<'EOF'
trait Animal { fn legs(&self) -> i64; fn sound(&self) -> i64; }
struct Dog { x: i64 }
struct Bird { y: i64 }
impl Animal for Dog  { fn legs(&self) -> i64 { 4 } fn sound(&self) -> i64 { 100 } }
impl Animal for Bird { fn legs(&self) -> i64 { 2 } fn sound(&self) -> i64 { 200 } }
fn legs_of(a: &dyn Animal) -> i64 { a.legs() }
fn sound_of(a: &dyn Animal) -> i64 { a.sound() }
fn main() -> i64 {
    let d = Dog { x: 0 };
    let b = Bird { y: 0 };
    legs_of(&d) + sound_of(&d) + legs_of(&b) + sound_of(&b)
}
EOF
MULTI_JIT=$("$KARDC" "$TMP/multi.kd" | tail -n 1)
if [[ "$MULTI_JIT" != "306" ]]; then
    echo "FAIL: multi-method dyn JIT expected 306, got: $MULTI_JIT"
    exit 1
fi
echo "Multi-method: legs (slot 0) + sound (slot 1) dispatch correctly == 306"

# --- dyn-safety negative: a no-self (static) trait method rejected as dyn. ---
cat > "$TMP/unsafe.kd" <<'EOF'
trait Maker { fn make() -> i64; }
struct S { x: i64 }
fn use_it(m: &dyn Maker) -> i64 { 0 }
fn main() -> i64 { 0 }
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/unsafe.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: a non-dyn-safe trait used as dyn should be rejected"
    exit 1
fi
if ! echo "$ERR_OUT" | grep -q 'not dyn-safe'; then
    echo "FAIL: expected a dyn-safety diagnostic, got:"
    echo "$ERR_OUT"
    exit 1
fi
echo "Negative: a no-self (static) trait method is rejected when used as dyn"

echo "PASS: dyn Trait vtable dispatch works in JIT + AOT (one call site, multiple runtime impls; Box<dyn>; multi-method slots; dyn-safety enforced)"
