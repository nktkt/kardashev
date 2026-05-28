#!/usr/bin/env bash
# Phase 12 smoke test: the REAL cooperative async runtime. Proves the future
# model genuinely suspends and resumes, rather than running bodies eagerly.
#
# Three realness criteria, each asserted in JIT and AOT:
#   (a) multi-state resume with locals preserved across >=2 suspensions
#       returns the right value computed from those locals;
#   (b) the Pending path is actually taken and observable — the global poll
#       counter exceeds the number of awaits (a Pending was re-polled);
#   (c) no eager whole-body execution — a side effect before the first await
#       fires exactly once even though the poll fn runs many times.
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

# ---------------------------------------------------------------------------
# (a) The headline acceptance program: two suspensions, result from locals.
#     `yield_now(v)` returns Pending on its first poll and Ready(v) on its
#     second, so each `.await` genuinely suspends `work` once. `a` is bound
#     across the second suspension and combined with `b` => 30.
# ---------------------------------------------------------------------------
cat > "$TMP/accept.kd" <<'EOF'
async fn work() -> i64 {
    let a = yield_now(10).await;
    let b = yield_now(20).await;
    a + b
}
fn main() -> i64 { block_on(work()) }
EOF

JIT_OUT=$("$KARDC" "$TMP/accept.kd")
if [[ "$JIT_OUT" != "30" ]]; then
    echo "FAIL (a) JIT: expected 30, got '$JIT_OUT'"; exit 1
fi
"$KARDC" -o "$TMP/accept" "$TMP/accept.kd"
# `main` returns 30, which JIT prints and AOT uses as the exit code.
set +e; "$TMP/accept"; AOT_RC=$?; set -e
if [[ "$AOT_RC" != "30" ]]; then
    echo "FAIL (a) AOT: exit code expected 30, got $AOT_RC"; exit 1
fi
echo "PASS (a): work() awaits 2 suspending futures; locals survive => 30 (JIT+AOT)"

# ---------------------------------------------------------------------------
# (b) Observe the Pending path. Two awaits, each backed by a yield_now that
#     suspends once => 4 polls total (each yield polled Pending then Ready).
#     poll_count() (> number of awaits = 2) proves at least one Pending was
#     returned and re-polled. We print result then poll_count.
# ---------------------------------------------------------------------------
cat > "$TMP/pending.kd" <<'EOF'
async fn work() -> i64 {
    let a = yield_now(10).await;
    let b = yield_now(20).await;
    a + b
}
fn main() -> i64 ! { io } {
    let r = block_on(work());
    print(r);
    print(poll_count());
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/pending.kd")
EXP_B=$'30\n4\n0'
if [[ "$JIT_OUT" != "$EXP_B" ]]; then
    echo "FAIL (b) JIT: expected '$EXP_B', got '$JIT_OUT'"; exit 1
fi
POLLS=$(printf '%s\n' "$JIT_OUT" | sed -n '2p')
if (( POLLS <= 2 )); then
    echo "FAIL (b): poll_count=$POLLS not > 2 awaits — Pending path not taken"
    exit 1
fi
"$KARDC" -o "$TMP/pending" "$TMP/pending.kd"
AOT_OUT=$("$TMP/pending")
if [[ "$AOT_OUT" != $'30\n4' ]]; then
    echo "FAIL (b) AOT: expected '30','4', got '$AOT_OUT'"; exit 1
fi
echo "PASS (b): poll_count=$POLLS > 2 awaits — Pending observed and re-polled (JIT+AOT)"

# ---------------------------------------------------------------------------
# (c) No eager execution. `print(7)` precedes the first await. Even though the
#     poll fn is entered multiple times (each yield suspends, executor
#     re-polls), the side effect must fire EXACTLY ONCE: resumption continues
#     from the suspension point, never from the top.
# ---------------------------------------------------------------------------
cat > "$TMP/once.kd" <<'EOF'
async fn work() -> i64 ! { io, async } {
    print(7);
    let a = yield_now(10).await;
    let b = yield_now(20).await;
    a + b
}
fn main() -> i64 ! { io } {
    let r = block_on(work());
    print(r);
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/once.kd")
EXP_C=$'7\n30\n0'
if [[ "$JIT_OUT" != "$EXP_C" ]]; then
    echo "FAIL (c) JIT: expected '$EXP_C', got '$JIT_OUT'"; exit 1
fi
SEVENS=$(printf '%s\n' "$JIT_OUT" | grep -c '^7$')
if (( SEVENS != 1 )); then
    echo "FAIL (c): pre-await print(7) fired $SEVENS times (expected exactly 1)"
    echo "  => body re-executed from the top on resume (eager / non-resumable)"
    exit 1
fi
"$KARDC" -o "$TMP/once" "$TMP/once.kd"
AOT_OUT=$("$TMP/once")
if [[ "$AOT_OUT" != $'7\n30' ]]; then
    echo "FAIL (c) AOT: expected '7','30', got '$AOT_OUT'"; exit 1
fi
echo "PASS (c): pre-await side effect fires exactly once across $POLLS polls (JIT+AOT)"

# ---------------------------------------------------------------------------
# Bonus: a single-await async fn (the Phase 6 shape) still works under the new
# model, and an async fn with ZERO awaits finishes Ready on the first poll.
# ---------------------------------------------------------------------------
cat > "$TMP/single.kd" <<'EOF'
async fn inc(n: i64) -> i64 { n + 1 }
async fn one_await(n: i64) -> i64 {
    let x = yield_now(n).await;
    x + 100
}
fn main() -> i64 {
    let a = block_on(inc(41));
    let b = block_on(one_await(5));
    a + b
}
EOF
JIT_OUT=$("$KARDC" "$TMP/single.kd")
if [[ "$JIT_OUT" != "147" ]]; then
    echo "FAIL (single/zero-await): expected 147, got '$JIT_OUT'"; exit 1
fi
"$KARDC" -o "$TMP/single" "$TMP/single.kd"
set +e; "$TMP/single"; SINGLE_RC=$?; set -e
if [[ "$SINGLE_RC" != "147" ]]; then
    echo "FAIL (single/zero-await) AOT: exit code expected 147, got $SINGLE_RC"
    exit 1
fi
echo "PASS (bonus): single-await and zero-await async fns work (42 + 105 = 147)"

# ---------------------------------------------------------------------------
# Negative: `.await` outside an async fn is rejected (it suspends the enclosing
# future, which only a future has).
# ---------------------------------------------------------------------------
cat > "$TMP/bad.kd" <<'EOF'
fn main() -> i64 { yield_now(1).await }
EOF
if "$KARDC" "$TMP/bad.kd" >/dev/null 2>&1; then
    echo "FAIL: .await outside an async fn was accepted"; exit 1
fi
echo "PASS (negative): .await outside an async fn is rejected"

echo "PASS: real async runtime — suspend/resume, Pending observed, no eager exec"
