#!/usr/bin/env bash
# v35 Phase 190 smoke test — the error-handling ecosystem.
#
#   - `Error` trait (fn message(&self) -> String) implemented on a user enum.
#   - Generic Result combinators: result_is_err / result_ok / result_err
#     (work for any T/E, unlike the older i64-only result_* helpers).
#   - `?`-with-`From`: a `?` on a Result<_, E1> inside a fn returning
#     Result<_, E2> auto-converts the error via `E2::from(e1)` when an
#     `impl From<E1> for E2` exists; a same-type `?` still works; a mismatch
#     with no `From` impl is a clean type error.
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
expect_err() {
    local name="$1" needle="$2" src="$3"
    printf '%s' "$src" > "$TMP/$name.kd"
    local err; err=$("$KARDC" "$TMP/$name.kd" 2>&1 >/dev/null || true)
    echo "$err" | grep -qi "$needle" || {
        echo "FAIL [$name]: expected error containing '$needle', got: $err"; exit 1; }
    echo "PASS (negative): $name"
}

# 1. `?`-with-From: IoErr propagated out of a fn returning Result<_, AppErr>
#    is converted via `impl From<IoErr> for AppErr`.
diff_run try_from $'105\n2\n5' '
enum IoErr { Io }
enum AppErr { App, FromIo }
impl From<IoErr> for AppErr { fn from(x: IoErr) -> AppErr { AppErr::FromIo } }
fn reads(ok: bool) -> Result<i64, IoErr> { if ok { Ok(5) } else { Err(IoErr::Io) } }
fn app(ok: bool) -> Result<i64, AppErr> { let v = reads(ok)?; Ok(v + 100) }
fn tag(r: Result<i64, AppErr>) -> i64 { match r { Ok(x) => x, Err(e) => match e { App => 1, FromIo => 2 } } }
fn same() -> Result<i64, IoErr> { let v = reads(true)?; Ok(v) }   // same-type ? unaffected
fn main() -> i64 ! { io } {
    print(tag(app(true)));                                       // 105
    print(tag(app(false)));                                      // converted -> 2
    match same() { Ok(x) => print(x), Err(e) => print(-1) }      // 5
    0
}
'

# 2. The Error trait on a user enum + generic Result combinators.
diff_run error_trait $'0\n7\n3' '
enum MyErr { Bad, Worse }
impl Error for MyErr { fn message(&self) -> String ! { alloc } { match self { Bad => "bad", Worse => "worse" } } }
fn try_it(ok: bool) -> Result<i64, MyErr> { if ok { Ok(7) } else { Err(MyErr::Bad) } }
fn main() -> i64 ! { io, alloc } {
    let r = try_it(true);
    if result_is_err(&r) { print(1); } else { print(0); }                          // 0
    match result_ok(r) { Some(x) => print(x), None => print(-1) }                  // 7
    match result_err(try_it(false)) { Some(e) => print(str_len(&e.message())), None => print(-1) }  // len("bad")=3
    0
}
'

# 3. NEGATIVE: mismatched Err types with no `From` impl is a clean error.
expect_err no_from 'no .impl From' '
enum E1 { A }
enum E2 { B }
fn g() -> Result<i64, E1> { Err(E1::A) }
fn h() -> Result<i64, E2> { let v = g()?; Ok(v) }
fn main() -> i64 { 0 }
'

echo "ALL PHASE 190 SMOKE TESTS PASSED"
