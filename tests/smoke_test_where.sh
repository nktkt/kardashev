#!/usr/bin/env bash
# Phase 21b smoke test: `where` clauses + associated types. End to end through
# JIT and AOT.
#
#   1. `where` EQUIVALENCE: a fn written with `where C: Container<U>` (using a
#      Phase-21a generic trait) compiles + runs identically to the same fn with
#      the inline `<U, C: Container<U>>` bound — same JIT value, same AOT exit,
#      and byte-for-byte-identical emitted LLVM IR (the `where` desugars onto
#      the very same bound machinery, so downstream is indistinguishable).
#   2. MULTI-CONSTRAINT `where`: `where C: Container, S: Show` over two distinct
#      bounded params, both exercised in one call.
#   3. NEGATIVE: a `where` constraint naming an unknown generic param is a
#      compile error (clear diagnostic, non-zero exit).
#   4. ASSOCIATED TYPES (Self::Item): `trait Container { type Item; fn get(&self)
#      -> Self::Item; }` with TWO impls choosing different `Item` (i64 + bool);
#      `.get()` on each yields the right-typed value.
#   5. ASSOCIATED TYPES (C::Item at a bounded call site): `fn first<C: Container>
#      (c: C) -> C::Item { c.get() }` used at BOTH impls — the projection
#      resolves to each impl's chosen associated type per monomorphic instance.
#   6. `where` + associated types combined in one fn.
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

# Run a program through both JIT (stdout, last line) and AOT (exit code) and
# assert both yield the expected i64. `main` returns the value.
run_case() {
    local name="$1" src="$2" want="$3"
    printf '%s' "$src" > "$TMP/$name.kd"

    local jit
    jit=$("$KARDC" "$TMP/$name.kd" | tail -n 1)
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL [$name]: JIT printed '$jit', expected '$want'"
        exit 1
    fi

    "$KARDC" --no-cache -o "$TMP/$name" "$TMP/$name.kd"
    set +e
    "$TMP/$name" > /dev/null
    local code=$?
    set -e
    if [[ "$code" != "$want" ]]; then
        echo "FAIL [$name]: AOT exit code $code, expected $want"
        exit 1
    fi
    echo "PASS [$name]: JIT + AOT == $want"
}

# 1. `where` EQUIVALENCE. The inline-bounded and where-bounded heads use a
# Phase-21a generic trait `Container<U>`; both must run to 77, AND the emitted
# LLVM IR must be byte-for-byte identical (proves the `where` desugars onto the
# exact same bound machinery — no separate code path downstream).
INLINE_SRC='
trait Container<T> { fn first(&self) -> T; }
struct IntBox { v: i64 }
impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }
fn head<U, C: Container<U>>(c: C) -> U { c.first() }
fn main() -> i64 { let ib = IntBox { v: 77 }; head(ib) }'
WHERE_SRC='
trait Container<T> { fn first(&self) -> T; }
struct IntBox { v: i64 }
impl Container<i64> for IntBox { fn first(&self) -> i64 { self.v } }
fn head<U, C>(c: C) -> U where C: Container<U> { c.first() }
fn main() -> i64 { let ib = IntBox { v: 77 }; head(ib) }'
run_case where_inline "$INLINE_SRC" 77
run_case where_clause "$WHERE_SRC" 77
printf '%s' "$INLINE_SRC" > "$TMP/wi.kd"
printf '%s' "$WHERE_SRC" > "$TMP/ww.kd"
"$KARDC" --emit-llvm "$TMP/wi.kd" 2>/dev/null > "$TMP/wi.ll"
"$KARDC" --emit-llvm "$TMP/ww.kd" 2>/dev/null > "$TMP/ww.ll"
if ! diff -q "$TMP/wi.ll" "$TMP/ww.ll" > /dev/null; then
    echo "FAIL [where_equiv_ir]: where-bounded IR differs from inline-bounded IR"
    diff "$TMP/wi.ll" "$TMP/ww.ll" | head -40
    exit 1
fi
echo "PASS [where_equiv_ir]: where-bounded emits byte-for-byte-identical IR to inline-bounded"

# 2. MULTI-CONSTRAINT `where`: two distinct bounded params in one clause.
# IntBox.get() == 40, Tag.show() == 2 -> 42.
run_case where_multi '
trait Getter { fn get(&self) -> i64; }
trait Show { fn show(&self) -> i64; }
struct IntBox { v: i64 }
impl Getter for IntBox { fn get(&self) -> i64 { self.v } }
struct Tag { t: i64 }
impl Show for Tag { fn show(&self) -> i64 { self.t } }
fn combine<G, S>(g: G, s: S) -> i64 where G: Getter, S: Show {
    g.get() + s.show()
}
fn main() -> i64 {
    let ib = IntBox { v: 40 };
    let t = Tag { t: 2 };
    combine(ib, t)
}' 42

# 3. NEGATIVE: a `where` constraint naming an unknown generic param is rejected
# at compile time with a clear diagnostic.
cat > "$TMP/where_unknown.kd" <<'EOF'
trait Show { fn show(&self) -> i64; }
fn f<T>(x: T) -> i64 where U: Show { 0 }
fn main() -> i64 { 0 }
EOF
set +e
WU_OUT=$("$KARDC" "$TMP/where_unknown.kd" 2>&1)
WU_RC=$?
set -e
if [[ "$WU_RC" -eq 0 ]]; then
    echo "FAIL [where_unknown_rejected]: a where clause on an unknown param should be rejected"
    exit 1
fi
if ! grep -qi 'unknown generic parameter' <<< "$WU_OUT"; then
    echo "FAIL [where_unknown_rejected]: expected an 'unknown generic parameter' diagnostic, got:"
    echo "$WU_OUT"
    exit 1
fi
echo "PASS [where_unknown_rejected]: where clause on an unknown generic param is rejected"

# 4. ASSOCIATED TYPES via `Self::Item`: two impls picking different Item types
# (i64 + bool). `bb.get()` (bool) gates `ib.get()` (i64 == 42). A correct
# per-impl Self::Item resolution returns 42.
run_case assoc_self_item '
trait Container { type Item; fn get(&self) -> Self::Item; }
struct IntBox { v: i64 }
struct BoolBox { b: bool }
impl Container for IntBox { type Item = i64; fn get(&self) -> Self::Item { self.v } }
impl Container for BoolBox { type Item = bool; fn get(&self) -> Self::Item { self.b } }
fn main() -> i64 {
    let ib = IntBox { v: 42 };
    let bb = BoolBox { b: true };
    let x = ib.get();
    let flag = bb.get();
    if flag { x } else { 0 }
}' 42

# 5. ASSOCIATED TYPES via `C::Item` at a bounded call site: `first<C: Container>
# (c: C) -> C::Item` used at BOTH impls. The bool path (first(bb)) gates the
# i64 path (first(ib) == 42); each call's result type resolves to that impl's
# associated type at its monomorphic instance.
run_case assoc_c_item '
trait Container { type Item; fn get(&self) -> Self::Item; }
struct IntBox { v: i64 }
struct BoolBox { b: bool }
impl Container for IntBox { type Item = i64; fn get(&self) -> Self::Item { self.v } }
impl Container for BoolBox { type Item = bool; fn get(&self) -> Self::Item { self.b } }
fn first<C: Container>(c: C) -> C::Item { c.get() }
fn main() -> i64 {
    let ib = IntBox { v: 42 };
    let bb = BoolBox { b: true };
    let x = first(ib);
    let flag = first(bb);
    if flag { x } else { 0 }
}' 42

# 6. `where` + associated types together: a `where C: Container, S: Show` fn
# whose body calls a Self::Item-returning method. 40 + 2 == 42.
run_case where_plus_assoc '
trait Container { type Item; fn get(&self) -> Self::Item; }
trait Show { fn show(&self) -> i64; }
struct IntBox { v: i64 }
impl Container for IntBox { type Item = i64; fn get(&self) -> Self::Item { self.v } }
struct Tag { t: i64 }
impl Show for Tag { fn show(&self) -> i64 { self.t } }
fn combine<C, S>(c: C, s: S) -> i64 where C: Container, S: Show {
    c.get() + s.show()
}
fn main() -> i64 {
    let ib = IntBox { v: 40 };
    let t = Tag { t: 2 };
    combine(ib, t)
}' 42

echo "PASS: where clauses + associated types work in JIT + AOT (where==inline incl. identical IR; multi-constraint where; unknown-param rejected; Self::Item at i64+bool; C::Item at a bounded call site; where+assoc combined)"
