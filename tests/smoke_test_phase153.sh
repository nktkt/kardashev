#!/usr/bin/env bash
# v28 Phase 153: const-generics beyond i64 — a `const N` parameter may be i64,
# bool, or char. The value is monomorphized by value (distinct instances), a
# value-use has the param's type (i64/bool/char) at the right width, and the
# binding's type annotation supplies the const arg (expected-type propagation —
# a down-payment on Phase 154). JIT + AOT.
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
expect_err() { local out; out=$("$KARDC" "$2" 2>&1)
    [[ $? -ne 0 ]] || { echo "FAIL [$1]: expected error, compiled"; exit 1; }
    echo "$out" | grep -qiE "$3" || { echo "FAIL [$1]: want /$3/, got: $out"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) bool const-generic: two instances (true/false) monomorphize distinctly
cat > "$TMP/b.kd" <<'EOF'
struct Sel<const FLAG: bool> { a: i64, b: i64 }
impl<const FLAG: bool> Sel<FLAG> {
    fn get(&self) -> i64 { if FLAG { self.a } else { self.b } }
}
fn main() -> i64 {
    let s: Sel<true> = Sel { a: 7, b: 9 };
    let t: Sel<false> = Sel { a: 7, b: 9 };
    s.get() * 10 + t.get()
}
EOF
run_eq bool_cg "$TMP/b.kd" 79 "bool const-generic: Sel<true>.get()=7, Sel<false>.get()=9 (79)"

# 2) char const-generic: value-use as char (cast to codepoint)
cat > "$TMP/c.kd" <<'EOF'
struct Delim<const SEP: char> { n: i64 }
impl<const SEP: char> Delim<SEP> {
    fn code(&self) -> i64 { SEP as i64 }
    fn is_slash(&self) -> bool { SEP == '/' }
}
fn main() -> i64 {
    let d: Delim<'/'> = Delim { n: 0 };
    let e: Delim<','> = Delim { n: 0 };
    let slash = if d.is_slash() { 1 } else { 0 };
    d.code() * 1000 + e.code() + slash * 100000
}
EOF
run_eq char_cg "$TMP/c.kd" 147044 "char const-generic: '/'=47, ','=44, is_slash (100000+47000+44)"

# 3) i64 const-generic still works (array-length inference — regression)
cat > "$TMP/i.kd" <<'EOF'
struct Vecn<const N: i64> { data: [i64; N] }
fn main() -> i64 {
    let v = Vecn { data: [10, 20, 30] };
    v.data[0] + v.data[2]
}
EOF
run_eq i64_cg "$TMP/i.kd" 40 "i64 const-generic via array-length inference (regression, 40)"

# 4) an unsupported const-param type (f64) is rejected at the declaration
cat > "$TMP/bad.kd" <<'EOF'
struct Bad<const X: f64> { v: i64 }
fn main() -> i64 { 0 }
EOF
expect_err bad_type "$TMP/bad.kd" "must be .i64., .bool., or .char." "an f64 const-param type is rejected"

echo "PASS: Phase 153 — const-generics beyond i64 (bool/char)"
