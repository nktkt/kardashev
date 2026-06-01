#!/usr/bin/env bash
# v31 Phase 167 smoke test — real `Send` / `Sync` MARKER TRAITS.
#
# Send and Sync are now first-class DECLARABLE marker traits (zero methods, no
# vtable, no runtime cost). Membership is:
#   * auto-derived STRUCTURALLY (a type with no explicit impl gets the
#     structural answer — the proven Phase 77/78 rule, unchanged);
#   * manually GRANTABLE via `impl Send for T {}` (the witness for an opaque /
#     handle type whose structural answer is too conservative — e.g. an Arc);
#   * OPT-OUT-able via `impl !Send for T {}` (the principled replacement for
#     hand-coded special cases).
# The three live enforcement sites (chan_send value, mutex_new cell, by-value
# thread_spawn capture) now consult this oracle, so an explicit impl wins over
# the structural rule. This test proves: (a) every prior accept/reject is
# preserved (regression), (b) opt-out and opt-in override both work, (c) the
# `char` Send gap is fixed, (d) marker/negative-impl misuse is rejected.
#
# Sync's structural rule is also wired into the oracle here; its first
# enforcement site (a `&T` crossing a thread) arrives with scoped threads in a
# later v31 phase, so this test exercises the shared oracle via the Send path.
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

# helper: expect a compile error whose message matches a regex.
expect_reject() { # $1=file $2=regex $3=label
    if "$KARDC" "$1" >/dev/null 2>&1; then
        echo "FAIL [$3]: compiled but expected a compile error"; exit 1
    fi
    local out; set +e; out=$("$KARDC" "$1" 2>&1); set -e
    grep -qiE "$2" <<<"$out" || {
        echo "FAIL [$3]: error did not match /$2/:"; echo "$out"; exit 1; }
    echo "PASS [$3]"
}
# helper: expect successful compile (typecheck) of a program.
expect_ok() { # $1=file $2=label
    "$KARDC" "$1" >/dev/null 2>&1 || {
        echo "FAIL [$2]: expected to compile, but:"; "$KARDC" "$1" 2>&1; exit 1; }
    echo "PASS [$2]"
}

# ---------------------------------------------------------------------------
# 1. REGRESSION (positive): the canonical thread + shared-Mutex program still
#    compiles AND runs identically on the JIT and AOT paths. The Mutex<i64>
#    handle (Send) is captured by value into both spawned closures.
# ---------------------------------------------------------------------------
cat > "$TMP/mutex.kd" <<'EOF'
fn bump(counter: Mutex<i64>, n: i64) -> i64 ! { io } {
    let mut i = 0;
    while i < n {
        mutex_lock(counter);
        mutex_set(counter, mutex_get(counter) + 1);
        mutex_unlock(counter);
        i = i + 1;
    }
    0
}
fn main() -> i64 ! { alloc, io, share } {
    let counter = mutex_new(0);
    let t1 = thread_spawn(|| bump(counter, 5000));
    let t2 = thread_spawn(|| bump(counter, 5000));
    thread_join(t1);
    thread_join(t2);
    print(mutex_get(counter));   // exactly 10000
    0
}
EOF
jit=$("$KARDC" "$TMP/mutex.kd" 2>/dev/null); jit=$(head -1 <<< "$jit")
[[ "$jit" == "10000" ]] || { echo "FAIL [mutex/jit]: expected 10000 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/mutex" "$TMP/mutex.kd" >/dev/null 2>&1
set +e; aout=$("$TMP/mutex"); arc=$?; set -e
[[ "$arc" -eq 0 && "$aout" == "10000" ]] || { echo "FAIL [mutex/aot]: exit $arc out '$aout'"; exit 1; }
echo "PASS [regression-mutex]: shared Mutex<i64> across threads == 10000 (JIT+AOT)"

# 1b. REGRESSION (positive): sending owned Send values on a channel compiles.
cat > "$TMP/chanok.kd" <<'EOF'
struct Point { x: i64, y: i64 }
fn main() -> i64 ! { alloc, share } {
    let (txi, rxi) = channel();
    chan_send(&txi, 42);                       // i64: Send
    let (txs, rxs) = channel();
    chan_send(&txs, "hello");                  // String: Send
    let (txp, rxp) = channel();
    chan_send(&txp, Point { x: 1, y: 2 });     // struct of Send fields: Send
    0
}
EOF
expect_ok "$TMP/chanok.kd" "regression-chan-send-ok"

# ---------------------------------------------------------------------------
# 2. REGRESSION (negative): the structural rejects are preserved THROUGH the
#    oracle (a type with no marker impl falls through to the structural rule).
#    An Rc (non-atomic refcount) is still not Send.
# ---------------------------------------------------------------------------
cat > "$TMP/rcsend.kd" <<'EOF'
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(&tx, rc_new(7));
    0
}
EOF
expect_reject "$TMP/rcsend.kd" "Rc.*not .?Send|not .?Send" "regression-rc-not-send"

# ---------------------------------------------------------------------------
# 3. NEW — OPT-OUT: `impl !Send for T {}` removes a structurally-Send type from
#    Send. A Widget is structurally Send (one i64 field); opting out makes
#    sending it a compile error that names Send.
# ---------------------------------------------------------------------------
cat > "$TMP/optout.kd" <<'EOF'
struct Widget { x: i64 }
impl !Send for Widget { }
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(&tx, Widget { x: 5 });
    0
}
EOF
expect_reject "$TMP/optout.kd" "not .?Send" "opt-out-negative-impl"

# 3b. The SAME program without the negative impl compiles (structural Send) —
#     proves the opt-out is what made the difference, not the struct itself.
cat > "$TMP/optout_off.kd" <<'EOF'
struct Widget { x: i64 }
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(&tx, Widget { x: 5 });
    0
}
EOF
expect_ok "$TMP/optout_off.kd" "opt-out-off-structural-send"

# ---------------------------------------------------------------------------
# 4. NEW — OPT-IN OVERRIDE: `impl Send for Rc<i64> {}` GRANTS Send to a type the
#    built-in structural rule rejects. The explicit impl wins, so sending an Rc
#    now compiles. (This is exactly the Arc story: an opaque handle whose
#    structural answer is wrong, made Send by an explicit, audited impl.)
# ---------------------------------------------------------------------------
cat > "$TMP/optin.kd" <<'EOF'
impl Send for Rc<i64> { }
fn main() -> i64 ! { alloc, share } {
    let (tx, rx) = channel();
    chan_send(&tx, rc_new(7));
    0
}
EOF
expect_ok "$TMP/optin.kd" "opt-in-override-grants-send"

# ---------------------------------------------------------------------------
# 5. NEW — `char` Send gap fixed. char is a Copy scalar (v27) but previously
#    fell through to NOT-Send, wrongly rejecting chan_send(char)/Mutex<char>.
# ---------------------------------------------------------------------------
cat > "$TMP/char.kd" <<'EOF'
fn main() -> i64 ! { alloc, io, share } {
    let (tx, rx) = channel();
    chan_send(&tx, 'a');          // char on a channel: now Send
    let m = mutex_new('z');       // Mutex<char>: cell is Send
    mutex_lock(m);
    let c = mutex_get(m);
    mutex_unlock(m);
    c as i64                      // 'z' == 122 — observable as the exit code
}
EOF
cout=$("$KARDC" "$TMP/char.kd" 2>/dev/null); cout=$(head -1 <<< "$cout")
[[ "$cout" == "122" ]] || { echo "FAIL [char-send/jit]: expected 122 got '$cout'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/char" "$TMP/char.kd" >/dev/null 2>&1
set +e; "$TMP/char"; crc=$?; set -e
[[ "$crc" -eq 122 ]] || { echo "FAIL [char-send/aot]: exit $crc (expected 122)"; exit 1; }
echo "PASS [char-send-fixed]: char is Send — chan_send(char) + Mutex<char> compile & run (JIT+AOT)"

# ---------------------------------------------------------------------------
# 6. NEW — misuse rejections.
# ---------------------------------------------------------------------------
# 6a. a negative impl is only allowed for the marker traits.
cat > "$TMP/neg_nonmarker.kd" <<'EOF'
struct W { x: i64 }
impl !Clone for W { }
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/neg_nonmarker.kd" "negative impls are only allowed" "neg-impl-nonmarker"

# 6b. a marker trait must have no methods.
cat > "$TMP/marker_method.kd" <<'EOF'
trait Send { fn f(&self) -> i64; }
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/marker_method.kd" "marker trait .?Send.? must have no methods" "marker-with-method"

# 6c. conflicting positive + negative marker impls for the same type.
cat > "$TMP/conflict.kd" <<'EOF'
struct W { x: i64 }
impl Send for W { }
impl !Send for W { }
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/conflict.kd" "conflicting .impl Send. and .impl !Send." "conflicting-marker-impls"

# 6d. a negative impl must be `for` a type.
cat > "$TMP/no_for.kd" <<'EOF'
struct W { x: i64 }
impl !W { }
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/no_for.kd" "must be .?for.? a type" "neg-impl-no-for"

# 6e. a negative impl must have an empty body.
cat > "$TMP/neg_body.kd" <<'EOF'
struct W { x: i64 }
impl !Send for W { fn g(&self) -> i64 { 0 } }
fn main() -> i64 { 0 }
EOF
expect_reject "$TMP/neg_body.kd" "must have an empty body" "neg-impl-nonempty-body"

echo "ALL PHASE 167 SMOKE TESTS PASSED"
