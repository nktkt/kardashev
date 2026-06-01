#!/usr/bin/env bash
# v34 Phase 182 smoke test — declarative (`macro_rules!`) macros.
#
# Macros are expanded at the TOKEN level before parsing: each
# `macro_rules! name { (matcher) => { body }; ... }` defines rules, and every
# `name!( … )` / `name![ … ]` / `name!{ … }` invocation is rewritten into the
# body of the first matching rule. Covered here: simple substitution, multiple
# rules selected by arity, fragment metavariables ($x:expr / $x:ident),
# one level of repetition `$( … ),*` in both matcher and body, recursive
# macros, item-position expansion (a macro that defines functions), and a
# negative (no matching rule). Differentially gated JIT vs AOT.
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

# 1. Simple expr-substitution macro; the argument is spliced in twice.
diff_run square $'49' '
macro_rules! square { ($x:expr) => { ($x) * ($x) }; }
fn main() -> i64 ! { io } { print(square!(7)); 0 }
'

# 2. Multiple rules selected by arity — a single-arg fragment must NOT swallow
#    a comma-separated second argument.
diff_run pick_arity $'5\n3' '
macro_rules! pick {
    ($a:expr) => { $a };
    ($a:expr, $b:expr) => { if $a < $b { $a } else { $b } };
}
fn main() -> i64 ! { io } { print(pick!(5)); print(pick!(8, 3)); 0 }
'

# 3. Recursion + repetition in the matcher AND a nested re-invocation: a
#    variadic sum reduces one element at a time.
diff_run variadic_sum $'15' '
macro_rules! sum {
    ($first:expr) => { $first };
    ($first:expr, $($rest:expr),*) => { $first + sum!($($rest),*) };
}
fn main() -> i64 ! { io } { print(sum!(1, 2, 3, 4, 5)); 0 }
'

# 4. ITEM-position expansion + an `ident` fragment: the macro defines functions.
diff_run item_macro $'42\n7' '
macro_rules! getter { ($name:ident, $val:expr) => { fn $name() -> i64 { $val } }; }
getter!(answer, 42)
getter!(lucky, 7)
fn main() -> i64 ! { io } { print(answer()); print(lucky()); 0 }
'

# 5. Repetition in the BODY: expand to a sequence of statements that build a
#    Vec, one push per argument.
diff_run vec_builder $'3\n40' '
macro_rules! vec_of {
    ($($x:expr),*) => {{ let mut v = vec_new(); $( vec_push(&mut v, $x); )* v }};
}
fn main() -> i64 ! { io, alloc } {
    let v = vec_of!(10, 20, 30);
    print(vec_len(&v));
    print(vec_get(&v, 0) + vec_get(&v, 2));
    0
}
'

# 6. NEGATIVE: an invocation that no rule matches is a clear error.
expect_err no_rule 'no macro rule matched' '
macro_rules! one { ($a:expr) => { $a }; }
fn main() -> i64 { one!(1, 2) }
'

echo "ALL PHASE 182 SMOKE TESTS PASSED"
