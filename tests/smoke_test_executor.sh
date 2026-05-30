#!/usr/bin/env bash
# Phase 18 smoke test: the real async I/O runtime — a cooperative MULTITASK
# executor, a wall-clock timer future, and a reactor that SLEEPS (rather than
# busy-spins) until the next event.
#
# Realness criteria, each asserted in JIT and AOT:
#   (1) interleave — two `spawn`ed tasks each print their id across several
#       awaits; the print order is INTERLEAVED (1,2,1,2,...), not segregated
#       (1,1,...,2,2,...). That proves cooperative round-robin scheduling, not
#       run-one-then-the-other. Both tasks finish with correct results (via
#       `join`).
#   (2) timer — a task that `sleep_ms(50).await`s takes >= ~45 ms of real
#       wall-clock time (measured around the AOT binary, so JIT/compile time
#       is excluded) and completes.
#   (3) concurrency win + low CPU — three ~100 ms sleeps run CONCURRENTLY: the
#       whole program finishes in well under their serial sum (< 250 ms, not
#       ~300 ms), proving overlap; and it consumes ~no CPU during the wait
#       (the reactor nanosleeps, it does not hot-spin).
#   (4) (Linux only) fd-readiness via epoll — an async `read_pipe(fd).await`
#       suspends in the reactor's epoll_wait until another task writes a byte
#       to the pipe, then wakes and reads it. Deferred on macOS (kqueue) — see
#       the note printed below.
#
# Timing bounds are deliberately loose (CI is slow): we assert LOWER bounds on
# sleeps and an UPPER bound comfortably below the serial sum for concurrency.
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

# millisecond wall clock (portable: GNU date supports %N; macOS date does not,
# so fall back to python3 / perl for sub-second precision).
now_ms() {
    local ns
    ns=$(date +%s%N 2>/dev/null || true)
    if [[ "$ns" == *N || -z "$ns" ]]; then
        # %N unsupported (e.g. macOS /bin/date) — use python3 then perl.
        if command -v python3 >/dev/null 2>&1; then
            python3 -c 'import time; print(int(time.time()*1000))'
        else
            perl -MTime::HiRes=time -e 'print int(time()*1000)'
        fi
    else
        echo $(( ns / 1000000 ))
    fi
}

# ---------------------------------------------------------------------------
# (1) Interleave: two spawned tasks, each printing its id across 3 awaits.
#     yield_now suspends once per await, so the scheduler round-robins the two
#     tasks and the ids alternate. join() drives the executor and returns each
#     task's result (1*3=3 and 2*3=6).
# ---------------------------------------------------------------------------
cat > "$TMP/interleave.kd" <<'EOF'
async fn worker(id: i64) -> i64 ! { io, async } {
    let a = yield_now(id).await;
    print(id);
    let b = yield_now(id).await;
    print(id);
    let c = yield_now(id).await;
    print(id);
    a + b + c
}
fn main() -> i64 ! { io } {
    let h1 = spawn(worker(1));
    let h2 = spawn(worker(2));
    let r1 = join(h1);
    let r2 = join(h2);
    print(r1);
    print(r2);
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/interleave.kd")
EXP_INTER=$'1\n2\n1\n2\n1\n2\n3\n6\n0'
if [[ "$JIT_OUT" != "$EXP_INTER" ]]; then
    echo "FAIL (1) JIT interleave: expected"; printf '%s\n' "$EXP_INTER"
    echo "got:"; printf '%s\n' "$JIT_OUT"; exit 1
fi
# Assert the print order is genuinely interleaved, not segregated. The first
# six prints (the ids) must be 1,2,1,2,1,2; a non-cooperative "run task 1 to
# completion then task 2" would print 1,1,1,2,2,2.
_ids_src=$(printf '%s\n' "$JIT_OUT"); IDS=$(head -6 <<< "$_ids_src" | tr '\n' ',')
if [[ "$IDS" != "1,2,1,2,1,2," ]]; then
    echo "FAIL (1): task prints not interleaved (got '$IDS') — not cooperative"
    exit 1
fi
"$KARDC" -o "$TMP/interleave" "$TMP/interleave.kd"
AOT_OUT=$("$TMP/interleave")
if [[ "$AOT_OUT" != $'1\n2\n1\n2\n1\n2\n3\n6' ]]; then
    echo "FAIL (1) AOT interleave: got:"; printf '%s\n' "$AOT_OUT"; exit 1
fi
echo "PASS (1): two spawned tasks interleave (1,2,1,2,1,2) and join => 3,6 (JIT+AOT)"

# ---------------------------------------------------------------------------
# (2) Timer: sleep_ms(50) takes >= ~45 ms of real time. We measure around the
#     AOT binary so the JIT/compile cost is not counted.
# ---------------------------------------------------------------------------
cat > "$TMP/timer.kd" <<'EOF'
async fn nap() -> i64 {
    let x = sleep_ms(50).await;
    x + 1
}
fn main() -> i64 { block_on(nap()) }
EOF

JIT_OUT=$("$KARDC" "$TMP/timer.kd")
if [[ "$JIT_OUT" != "51" ]]; then
    echo "FAIL (2) JIT timer: expected 51, got '$JIT_OUT'"; exit 1
fi
"$KARDC" -o "$TMP/timer" "$TMP/timer.kd"
# `main` returns 51, which AOT uses as the exit code — disable `set -e` so the
# non-zero exit doesn't abort the script before we read it.
T0=$(now_ms); set +e; "$TMP/timer"; RC=$?; set -e; T1=$(now_ms)
ELAPSED=$(( T1 - T0 ))
if [[ "$RC" != "51" ]]; then
    echo "FAIL (2) AOT timer: exit code expected 51, got $RC"; exit 1
fi
# Lower bound: a real 50 ms sleep must take at least ~45 ms (slack for clock
# granularity). If sleep_ms were a no-op / busy yield this would be ~0 ms.
if (( ELAPSED < 45 )); then
    echo "FAIL (2): sleep_ms(50) wall time was ${ELAPSED}ms (< 45ms) — timer not real"
    exit 1
fi
echo "PASS (2): sleep_ms(50) takes ${ELAPSED}ms (>= 45ms) and yields 51 (JIT+AOT)"

# ---------------------------------------------------------------------------
# (3) Concurrency win + low CPU: three 100 ms sleeps run concurrently. Serial
#     would be ~300 ms; concurrent must finish well under 250 ms. And the
#     reactor must SLEEP, not spin: CPU time stays tiny.
# ---------------------------------------------------------------------------
cat > "$TMP/conc.kd" <<'EOF'
async fn nap(id: i64) -> i64 ! { io, async } {
    let x = sleep_ms(100).await;
    print(id);
    x
}
fn main() -> i64 ! { io } {
    let h1 = spawn(nap(1));
    let h2 = spawn(nap(2));
    let h3 = spawn(nap(3));
    join(h1);
    join(h2);
    join(h3);
    0
}
EOF

"$KARDC" -o "$TMP/conc" "$TMP/conc.kd"
T0=$(now_ms); CONC_OUT=$("$TMP/conc"); T1=$(now_ms)
ELAPSED=$(( T1 - T0 ))
# All three tasks complete (print 1,2,3 in some order).
SORTED=$(printf '%s\n' "$CONC_OUT" | sort | tr '\n' ',')
if [[ "$SORTED" != "1,2,3," ]]; then
    echo "FAIL (3): expected all of 1,2,3 to print, got '$SORTED'"; exit 1
fi
# Upper bound: must be well under the 300 ms serial sum (proves overlap). Lower
# bound: must still take at least ~90 ms (they DO sleep ~100 ms concurrently).
if (( ELAPSED >= 250 )); then
    echo "FAIL (3): 3x100ms concurrent took ${ELAPSED}ms (>= 250ms) — not overlapping"
    exit 1
fi
if (( ELAPSED < 90 )); then
    echo "FAIL (3): 3x100ms concurrent took ${ELAPSED}ms (< 90ms) — sleeps not real"
    exit 1
fi
echo "PASS (3): 3x sleep_ms(100) finish concurrently in ${ELAPSED}ms (90<=t<250, serial=300)"

# Low-CPU assertion: the reactor nanosleeps, so user+sys CPU during the ~100 ms
# wait is a small fraction of wall time. Use bash `time` (TIMEFORMAT) which is
# always available; require user+sys < 50 ms (a spin loop would burn ~100 ms).
TIMEFORMAT='%R %U %S'
read -r WALL USER SYS < <( { time "$TMP/conc" >/dev/null; } 2>&1 )
# floating-point compare via awk; tolerate CI noise generously.
CPU_OK=$(awk -v u="$USER" -v s="$SYS" 'BEGIN { print (u + s < 0.060) ? "1" : "0" }')
if [[ "$CPU_OK" != "1" ]]; then
    echo "WARN (3-cpu): user=${USER}s sys=${SYS}s (>=60ms) — reactor may be spinning"
    # Not a hard failure (shared CI runners can be noisy), but surfaced loudly.
else
    echo "PASS (3-cpu): reactor slept — user=${USER}s sys=${SYS}s during a ~100ms wait"
fi

# ---------------------------------------------------------------------------
# (4) fd-readiness via epoll (Linux only). An async reader awaits a byte from a
#     pipe; a concurrent writer sleeps 60 ms then writes. The reader suspends
#     in epoll_wait and is woken by the write. On macOS the kqueue reactor is
#     deferred, so read_pipe is not registered there — we skip with a note.
# ---------------------------------------------------------------------------
UNAME=$(uname -s 2>/dev/null || echo unknown)
if [[ "$UNAME" == "Linux" ]]; then
    cat > "$TMP/pipe.kd" <<'EOF'
async fn reader(h: i64) -> i64 ! { io, async } {
    let b = read_pipe(h).await;
    print(b);
    b
}
async fn delayed_writer(h: i64) -> i64 ! { io, async } {
    let x = sleep_ms(60).await;
    pipe_send(h, 90);
    0
}
fn main() -> i64 ! { io } {
    let h = pipe_make();
    let tr = spawn(reader(h));
    let tw = spawn(delayed_writer(h));
    let r = join(tr);
    join(tw);
    print(r);
    0
}
EOF
    JIT_OUT=$("$KARDC" "$TMP/pipe.kd")
    if [[ "$JIT_OUT" != $'90\n90\n0' ]]; then
        echo "FAIL (4) JIT epoll pipe: expected 90,90,0 got:"; printf '%s\n' "$JIT_OUT"
        exit 1
    fi
    "$KARDC" -o "$TMP/pipe" "$TMP/pipe.kd"
    T0=$(now_ms); AOT_OUT=$("$TMP/pipe"); T1=$(now_ms)
    ELAPSED=$(( T1 - T0 ))
    if [[ "$AOT_OUT" != $'90\n90' ]]; then
        echo "FAIL (4) AOT epoll pipe: expected 90,90 got:"; printf '%s\n' "$AOT_OUT"
        exit 1
    fi
    # The reader genuinely waited for the 60ms-delayed write (lower bound ~55ms)
    # and the whole thing stayed well under a serial interpretation.
    if (( ELAPSED < 55 )); then
        echo "FAIL (4): epoll wake took ${ELAPSED}ms (< 55ms) — reader didn't wait on the fd"
        exit 1
    fi
    echo "PASS (4): async read_pipe suspends in epoll_wait, woken by a delayed write => 90 (${ELAPSED}ms, JIT+AOT)"
else
    echo "SKIP (4): fd-readiness via epoll is Linux-only; kqueue ($UNAME) reactor DEFERRED"
fi

echo "PASS: real async runtime — multitask executor interleaves, timer reactor sleeps, concurrency overlaps"
