#!/usr/bin/env bash
# Phase 19 smoke test: OS threads (pthread) + Mutex + compile-time data-race
# freedom (Send).
#
# Asserted, each through JIT and AOT unless noted:
#   (1) thread_spawn / thread_join — two threads compute different values and
#       join returns both correctly; combine them.
#   (2) Mutex mutual exclusion — two threads each increment a SHARED
#       Mutex-guarded counter 100000 times; after joining both, the counter is
#       EXACTLY 200000. Run several times: it must be deterministic with the
#       lock. (Reaching exactly 2N also proves BOTH threads genuinely ran — if
#       only one ran we'd see 100000 — and that no updates were lost to a race,
#       which is the proof of mutual exclusion.)
#   (3) Send (compile-time rejection) — a thread closure that captures a local
#       BY REFERENCE (`let mut n; thread_spawn(|| { n = n + 1; ... })`) is a
#       COMPILE error; the by-VALUE equivalent compiles and runs.
#   (4) Mutex handle is shareable across threads by value (the (2) program
#       passes the same i64 handle into both closures).
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
# (1) thread_spawn / thread_join: two threads compute distinct values; join
#     returns each; combine.
# ---------------------------------------------------------------------------
cat > "$TMP/spawn.kd" <<'EOF'
fn compute_a() -> i64 { 10 + 11 }       // 21
fn compute_b() -> i64 { 100 + 23 }      // 123
fn main() -> i64 ! { io, share } {
    let a = thread_spawn(compute_a);
    let b = thread_spawn(compute_b);
    let ra = thread_join(a);
    let rb = thread_join(b);
    print(ra);          // 21
    print(rb);          // 123
    print(ra + rb);     // 144
    0
}
EOF

JIT_OUT=$("$KARDC" "$TMP/spawn.kd"); JIT_OUT=$(head -3 <<< "$JIT_OUT")
if [[ "$JIT_OUT" != $'21\n123\n144' ]]; then
    echo "FAIL (1) JIT spawn/join: got:"; printf '%s\n' "$JIT_OUT"; exit 1
fi
"$KARDC" -o "$TMP/spawn" "$TMP/spawn.kd"
AOT_OUT=$("$TMP/spawn")
if [[ "$AOT_OUT" != $'21\n123\n144' ]]; then
    echo "FAIL (1) AOT spawn/join: got:"; printf '%s\n' "$AOT_OUT"; exit 1
fi
echo "PASS (1): two threads compute 21 + 123 => 144, both joined (JIT+AOT)"

# Also prove it works with capturing closures (by-value i64 capture of `base`).
cat > "$TMP/spawn_cl.kd" <<'EOF'
fn main() -> i64 ! { io, share } {
    let base = 1000;
    let a = thread_spawn(|| base + 21);   // 1021
    let b = thread_spawn(|| base + 23);   // 1023
    print(thread_join(a) + thread_join(b));   // 2044
    0
}
EOF
JIT_OUT=$("$KARDC" "$TMP/spawn_cl.kd"); JIT_OUT=$(head -1 <<< "$JIT_OUT")
if [[ "$JIT_OUT" != "2044" ]]; then
    echo "FAIL (1b) JIT closure spawn: expected 2044, got '$JIT_OUT'"; exit 1
fi
"$KARDC" -o "$TMP/spawn_cl" "$TMP/spawn_cl.kd"
if [[ "$("$TMP/spawn_cl")" != "2044" ]]; then
    echo "FAIL (1b) AOT closure spawn"; exit 1
fi
echo "PASS (1b): by-value-capturing closures spawn + join => 2044 (JIT+AOT)"

# ---------------------------------------------------------------------------
# (2) Mutex mutual exclusion: 2 threads x 100000 increments of a SHARED
#     Mutex counter == exactly 200000. Deterministic with the lock.
# ---------------------------------------------------------------------------
cat > "$TMP/mutex.kd" <<'EOF'
fn bump(counter: i64, n: i64) -> i64 ! { io } {
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
    let n = 100000;
    // The SAME i64 Mutex handle is captured by value into both closures, so
    // both threads share one underlying lock + cell.
    let t1 = thread_spawn(|| bump(counter, n));
    let t2 = thread_spawn(|| bump(counter, n));
    thread_join(t1);
    thread_join(t2);
    print(mutex_get(counter));   // exactly 200000
    0
}
EOF

# JIT: run several times — must be deterministic 200000 every time.
for run in 1 2 3 4 5; do
    OUT=$("$KARDC" "$TMP/mutex.kd"); OUT=$(head -1 <<< "$OUT")
    if [[ "$OUT" != "200000" ]]; then
        echo "FAIL (2) JIT mutex run $run: expected 200000, got '$OUT'"; exit 1
    fi
done
# AOT: build once, run several times — same determinism.
"$KARDC" -o "$TMP/mutex" "$TMP/mutex.kd"
for run in 1 2 3 4 5; do
    OUT=$("$TMP/mutex")
    if [[ "$OUT" != "200000" ]]; then
        echo "FAIL (2) AOT mutex run $run: expected 200000, got '$OUT'"; exit 1
    fi
done
echo "PASS (2): 2 threads x 100000 shared Mutex increments == 200000, deterministic over 5+5 runs (JIT+AOT)"

# ---------------------------------------------------------------------------
# (3) Send: a thread closure that captures by REFERENCE must be rejected at
#     compile time; the by-VALUE equivalent must compile + run.
# ---------------------------------------------------------------------------
cat > "$TMP/byref.kd" <<'EOF'
fn main() -> i64 ! { io, share } {
    let mut n = 0;
    let t = thread_spawn(|| { n = n + 1; n });   // by-ref capture: must reject
    thread_join(t);
    0
}
EOF
set +e
ERR=$("$KARDC" "$TMP/byref.kd" 2>&1)
RC=$?
set -e
if [[ $RC -eq 0 ]]; then
    echo "FAIL (3): by-ref capture into thread_spawn compiled (should be rejected)"
    echo "$ERR"; exit 1
fi
if ! grep -qi "by-reference capture across a thread" <<< "$ERR"; then
    echo "FAIL (3): rejection message did not mention the thread-boundary Send rule:"
    printf '%s\n' "$ERR"; exit 1
fi
echo "PASS (3a): by-ref capture into thread_spawn rejected at compile time"

# Also confirm the AOT path rejects it (front-end runs before object emit).
set +e
"$KARDC" -o "$TMP/byref" "$TMP/byref.kd" >/dev/null 2>&1
RC=$?
set -e
if [[ $RC -eq 0 ]]; then
    echo "FAIL (3): by-ref capture compiled to a binary (should be rejected)"; exit 1
fi
if [[ -e "$TMP/byref" ]]; then
    echo "FAIL (3): a binary was produced despite the compile error"; exit 1
fi
echo "PASS (3b): rejection also blocks AOT object/binary emission"

# By-value equivalent: capture `n` by value (read-only) — compiles + runs.
cat > "$TMP/byval.kd" <<'EOF'
fn main() -> i64 ! { io, share } {
    let n = 41;
    let t = thread_spawn(|| n + 1);   // by-value Copy capture: fine
    print(thread_join(t));            // 42
    0
}
EOF
JIT_OUT=$("$KARDC" "$TMP/byval.kd"); JIT_OUT=$(head -1 <<< "$JIT_OUT")
if [[ "$JIT_OUT" != "42" ]]; then
    echo "FAIL (3c) JIT by-value: expected 42, got '$JIT_OUT'"; exit 1
fi
"$KARDC" -o "$TMP/byval" "$TMP/byval.kd"
if [[ "$("$TMP/byval")" != "42" ]]; then
    echo "FAIL (3c) AOT by-value"; exit 1
fi
echo "PASS (3c): by-value capture compiles + runs => 42 (JIT+AOT)"

echo "ALL THREAD TESTS PASSED"
