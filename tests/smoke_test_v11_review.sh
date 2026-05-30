#!/usr/bin/env bash
# Roadmap v11 PRE-MERGE adversarial-review regressions. A multi-agent review of
# the numeric tower found a cluster the green suite missed — all in the const-
# evaluation / const-emission path (a sized/unsigned `const`), plus two parser/
# lexer bugs the new `as` cast and width suffixes introduced. Each is pinned
# here. (Same discipline as tests/smoke_test_v10_review.sh.)
#
#   BUG D (BLOCKER): a narrow/unsigned const flowed into a narrow slot as an
#     i64 immediate -> INVALID LLVM IR (`call i32 @id(i64 7)`) / verifier crash.
#   BUG A: an unsigned const `>>` folded as an ARITHMETIC shift, disagreeing
#     with the runtime logical shift (`(0u32-1) >> 1`: -1 vs 2147483647).
#   BUG B/C: a narrow const arithmetic result was not wrapped to its width
#     (`100i8 + 100i8` -> 200, impossible for i8; runtime wraps to -56).
#   BUG E: a narrow const silently carried an out-of-range value into a slot
#     (`1i32 << 31` -> 2147483648 in an i32).
#   BUG F: a plain-literal narrow/unsigned const was rejected though the same
#     `let` was accepted (`const C: i32 = 100`).
#   PARSE: `expr as Type << ..` / `expr as Type < ..` mis-parsed (the cast's
#     target type greedily ate the `<`/`<<` as a generic-arg list).
#   LEX: a width suffix was absorbed in tuple-index position (`t.0i32` -> t.0).
#
# The unifying fix: const folds now WRAP every result to its expr-type width
# (so unsigned `>>` is logical and narrow results wrap exactly like runtime),
# codegen emits a folded const at its declared width, `checkConstItem` narrows
# a literal like `let`, the cast parses only a bare (numeric) target type, and
# the lexer does not take a suffix after `.`.
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

# Helper: const value == runtime value for the SAME expression, JIT and AOT.
# Usage: const_eq NAME  TYPE  EXPR  EXPECTED
const_eq() {
    local n=$1 ty=$2 expr=$3 want=$4 f="$TMP/$1.kd"
    cat > "$f" <<EOF
const K: $ty = $expr;
fn main() -> i64 ! { io } {
    let r: $ty = $expr;
    print(K as i64);
    print(r as i64);
    0
}
EOF
    local out; out=$("$KARDC" "$f" 2>/dev/null)
    local kval rval; kval=$(sed -n '1p' <<<"$out"); rval=$(sed -n '2p' <<<"$out")
    [[ "$kval" == "$want" && "$rval" == "$want" ]] || {
        echo "FAIL [$n]: const=$kval runtime=$rval want=$want (both)"; exit 1; }
    echo "PASS [$n]: const == runtime == $want"
}

# BUG A — unsigned const logical >> matches runtime.
const_eq A-unsigned-shr u32 '(0u32 - 1u32) >> 1' 2147483647
# BUG B — narrow signed const shift wraps to width.
const_eq B-i32-shl i32 '1i32 << 31' -2147483648
# BUG C — narrow const arithmetic wraps to width.
const_eq C-i8-add i8 '100i8 + 100i8' -56
# unsigned narrow arithmetic wraps too.
const_eq C-u8-add u8 '200u8 + 100u8' 44
# unsigned const division uses unsigned semantics.
const_eq C-u32-div u32 '4000000000u32 / 7u32' 571428571

# BUG D — a narrow/unsigned const into a narrow param: valid IR, runs (was a
# verifier crash / `call i32 @id(i64 ...)`).
cat > "$TMP/d.kd" <<'EOF'
const C: u32 = 7u32;
fn id(x: u32) -> u32 { x }
fn main() -> i64 ! { io } { print(id(C) as i64); 0 }
EOF
out=$("$KARDC" "$TMP/d.kd" 2>&1)
grep -qi "verification failed\|does not match\|error" <<<"$out" && { echo "FAIL [D-narrow-const-param]: $out"; exit 1; }
[[ "$(sed -n '1p' <<<"$out")" == "7" ]] || { echo "FAIL [D]: got '$out'"; exit 1; }
echo "PASS [D-narrow-const-param]: valid IR, prints 7"

# BUG E — a narrow const value never escapes its range silently.
cat > "$TMP/e.kd" <<'EOF'
const BIG: i32 = 1i32 << 31;
fn main() -> i64 ! { io } { let z: i32 = BIG; print(z as i64); 0 }
EOF
_eout=$("$KARDC" "$TMP/e.kd" 2>/dev/null); [[ "$(head -1 <<<"$_eout")" == "-2147483648" ]] || { echo "FAIL [E]: silent out-of-range i32"; exit 1; }
echo "PASS [E-no-silent-out-of-range]: i32 const holds -2147483648"

# BUG F — a plain-literal narrow / unsigned const is accepted (like `let`).
printf 'const C: i32 = 100;\nconst O: u64 = 0xcbf29ce484222325;\nfn main() -> i64 { (C as i64) + ((O >> 60) as i64) }\n' > "$TMP/f.kd"
"$KARDC" "$TMP/f.kd" >/dev/null 2>&1 || { echo "FAIL [F-plain-narrow-const]: rejected"; exit 1; }
echo "PASS [F-plain-narrow-const]: const i32 = 100 / const u64 = 0xcbf... accepted"

# PARSE — `expr as Type << ..` and `< ..` parse as (expr as Type) <op> ..
cat > "$TMP/p1.kd" <<'EOF'
fn main() -> i64 { let x: i64 = 3; ((x as i32) << 2) as i64 }
EOF
[[ "$("$KARDC" "$TMP/p1.kd" 2>/dev/null | tail -1)" == "12" ]] || { echo "FAIL [parse-as-shl]: expected 12"; exit 1; }
cat > "$TMP/p2.kd" <<'EOF'
fn main() -> i64 { let a: i64 = 3; let b: i64 = 5; if a as i32 < b as i32 { 1 } else { 0 } }
EOF
[[ "$("$KARDC" "$TMP/p2.kd" 2>/dev/null | tail -1)" == "1" ]] || { echo "FAIL [parse-as-lt]: expected 1"; exit 1; }
# the unparenthesized shift form must also parse now (type error is fine; a
# PARSE error is the regression).
printf 'fn main() -> i64 { let x: i64 = 3; (x as i32 << 2) as i64 }\n' > "$TMP/p3.kd"
_p3out=$("$KARDC" "$TMP/p3.kd" 2>&1) || true; grep -qi "parse error" <<<"$_p3out" && { echo "FAIL [parse-as-shl-bare]: still a parse error"; exit 1; }
echo "PASS [parse-as-cast-then-shift/compare]: (x as i32) << 2 == 12, (a as i32) < (b as i32)"

# LEX — a width suffix is NOT taken in tuple-index position; a plain `t.0`
# still works and a suffixed index is rejected (not silently dropped).
printf 'fn main() -> i64 { let t: (i64, i64) = (11, 22); t.0 + t.1 }\n' > "$TMP/l1.kd"
[[ "$("$KARDC" "$TMP/l1.kd" 2>/dev/null | tail -1)" == "33" ]] || { echo "FAIL [tuple-index]: t.0+t.1 != 33"; exit 1; }
printf 'fn main() -> i64 { let t: (i64, i64) = (11, 22); t.0i32 }\n' > "$TMP/l2.kd"
"$KARDC" "$TMP/l2.kd" >/dev/null 2>&1 && { echo "FAIL [tuple-index-suffix]: t.0i32 silently accepted"; exit 1; }
echo "PASS [tuple-index-suffix]: t.0/t.1 work; t.0i32 rejected (not silently dropped)"

echo "ALL V11 REVIEW-REGRESSION TESTS PASSED"
