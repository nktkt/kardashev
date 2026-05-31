#!/usr/bin/env bash
# v29 Phase 159: the C backend (`--emit-c`) grows to REFERENCES. `&T` / `&mut T`
# lower to C pointers; `&x` is address-of; `&<temporary>` is a pointer to a C99
# block-scoped compound literal; `*r` dereferences; `r.field` auto-derefs to
# `(*r).field`; a unit-returning (no `-> T`) fn lowers to a 0-returning function.
# Differentially gated: LLVM-AOT exit == emitted-C exit.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[[ -z "$CC_BIN" ]] && { echo "SKIP: no C compiler"; exit 0; }
echo "Using kardc at: $KARDC ; cc: $CC_BIN"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    "$TMP/llvm" >/dev/null 2>&1; local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused in-subset: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C:"; head -5 "$TMP/cc"; exit 1; }
    "$TMP/cbin" >/dev/null 2>&1; local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    echo "PASS [$name]: LLVM == C == $lrc"; }

# 1) read through `&Struct` (auto-deref field access) + mutate through `&mut`
cat > "$TMP/r.kd" <<'EOF'
struct Point { x: i64, y: i64 }
fn read(p: &Point) -> i64 { p.x + p.y }
fn bump(p: &mut Point) { p.x = p.x + 10; }
fn main() -> i64 {
    let mut q = Point { x: 1, y: 2 };
    bump(&mut q);
    bump(&mut q);
    read(&q)
}
EOF
diff_ok ref_struct "$TMP/r.kd"

# 2) `&<temporary>` — a pointer to a struct literal
cat > "$TMP/t.kd" <<'EOF'
struct P { x: i64, y: i64 }
fn diag(p: &P) -> i64 { p.x - p.y }
fn main() -> i64 { diag(&P { x: 50, y: 8 }) }
EOF
diff_ok ref_temp "$TMP/t.kd"

# 3) `&i64` + explicit deref `*r`
cat > "$TMP/i.kd" <<'EOF'
fn add(a: &i64, b: &i64) -> i64 { *a + *b }
fn main() -> i64 { let x = 30; let y = 12; add(&x, &y) }
EOF
diff_ok ref_scalar "$TMP/i.kd"

# 4) a chain: read a field of a struct behind a ref, in nested calls
cat > "$TMP/c.kd" <<'EOF'
struct Acc { total: i64, n: i64 }
fn add_to(a: &mut Acc, v: i64) { a.total = a.total + v; a.n = a.n + 1; }
fn avg(a: &Acc) -> i64 { a.total / a.n }
fn main() -> i64 {
    let mut a = Acc { total: 0, n: 0 };
    add_to(&mut a, 10); add_to(&mut a, 20); add_to(&mut a, 30);
    avg(&a)
}
EOF
diff_ok ref_chain "$TMP/c.kd"

echo "PASS: Phase 159 — C backend references / borrows + &<temporary> (gated vs LLVM)"
