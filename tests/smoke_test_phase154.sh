#!/usr/bin/env bash
# v28 Phase 154: bidirectional type-inference completeness. Two fixes:
#  - struct-literal field values now route through the coercion entry point
#    (coerceOrUnify), so a field gets the same inference/coercions a fn arg
#    does: an unannotated `None` infers its type param from the field, an int
#    literal narrows, etc.
#  - generic enum <-> struct mutual references resolve correctly (a second
#    resolution round materializes a generic-enum struct field's full variants),
#    fixing `struct H { m: Maybe<i64> }` constructed with `Yes(..)` / `None`.
# (The expected-type propagation that supplies const-generic args from a binding
# annotation, Phase 153, is the same machinery.) JIT + AOT.
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
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq $(( $3 & 255 )) ]] || { echo "FAIL [$1/aot]: exit $rc want $(( $3 & 255 ))"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) a generic enum as a struct field (the mutual-reference resolution fix)
cat > "$TMP/g.kd" <<'EOF'
enum Maybe<T> { Yes(T), No }
struct Holder { m: Maybe<i64>, k: i64 }
fn val(h: &Holder) -> i64 { match h.m { Yes(v) => v, No => 0 } }
fn main() -> i64 {
    let a = Holder { m: Yes(5), k: 1 };
    let b = Holder { m: No, k: 2 };
    val(&a) * 10 + val(&b)
}
EOF
run_eq generic_enum_field "$TMP/g.kd" 50 "generic enum struct field: Yes(5)->5, No->0 (50)"

# 2) the prelude Option as a struct field, with None inferred from the field
cat > "$TMP/o.kd" <<'EOF'
struct Cfg { timeout: Option<i64> }
fn get(c: &Cfg) -> i64 { match c.timeout { Some(v) => v, None => -1 } }
fn main() -> i64 {
    let on = Cfg { timeout: Some(30) };
    let off = Cfg { timeout: None };
    get(&on) - get(&off)
}
EOF
run_eq option_field "$TMP/o.kd" 31 "Option struct field: Some(30) - None(-1) = 31"

# 3) int literal narrows to the declared field width (coerceOrUnify in fields)
cat > "$TMP/n.kd" <<'EOF'
struct Packed { a: i32, b: u8 }
fn main() -> i64 {
    let p = Packed { a: 1000, b: 250 };
    (p.a as i64) + (p.b as i64)
}
EOF
run_eq field_narrow "$TMP/n.kd" 1250 "int literals narrow to i32/u8 struct fields (1000+250)"

# 4) the reverse direction still works: an enum payload that is a struct
cat > "$TMP/r.kd" <<'EOF'
struct Pt { x: i64, y: i64 }
enum Shape { Dot, At(Pt) }
fn area(s: &Shape) -> i64 { match s { At(p) => p.x + p.y, Dot => 0 } }
fn main() -> i64 { let s = At(Pt { x: 3, y: 4 }); area(&s) }
EOF
run_eq enum_payload_struct "$TMP/r.kd" 7 "enum payload referencing a struct (reverse direction, 7)"

echo "PASS: Phase 154 — bidirectional inference (struct-field coercion + enum/struct mutual resolution)"
