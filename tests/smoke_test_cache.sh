#!/usr/bin/env bash
# Phase 14b smoke test: the AOT incremental compile cache.
#
# Exercises the content-addressed object cache wired into `kardc -o`:
#   1. First compile of a program           -> stderr "cache miss", exe runs.
#   2. Identical source to a NEW output path -> stderr "cache hit", same key,
#                                               same result (object reused).
#   3. One character changed                 -> stderr "cache miss" (a DIFFERENT
#                                               key), new result.
#   4. `--no-cache`                          -> neither hit nor miss; bypassed.
#
# We point XDG_CACHE_HOME at a throwaway dir so the test is hermetic and never
# touches the developer's real ~/.cache.
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
    echo "FAIL: kardc binary not found in runfiles"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Hermetic cache location.
export XDG_CACHE_HOME="$TMP/xdg-cache"

# fib(10) == 55; fib(11) == 89. Both < 128 so they fit in a process exit code.
write_prog() {
    cat > "$TMP/prog.kd" <<EOF
fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } }
fn main() -> i64 { fib($1) }
EOF
}

run_exit() {
    set +e
    "$1"
    local rc=$?
    set -e
    echo "$rc"
}

# --- 1. First compile: expect a cache MISS. ---
write_prog 10
ERR1=$("$KARDC" -o "$TMP/a" "$TMP/prog.kd" 2>&1 1>/dev/null)
if ! grep -q 'cache miss' <<< "$ERR1"; then
    echo "FAIL: first compile did not report 'cache miss'"
    echo "stderr: $ERR1"
    exit 1
fi
RC=$(run_exit "$TMP/a")
if [[ "$RC" -ne 55 ]]; then
    echo "FAIL: first build returned $RC (expected 55)"
    exit 1
fi
# Capture the key printed on the miss for the hit comparison below.
KEY1=$(grep -o 'cache miss [0-9a-f]*' <<< "$ERR1" | awk '{print $3}')

# --- 2. Identical source, NEW output path: expect a cache HIT. ---
ERR2=$("$KARDC" -o "$TMP/b" "$TMP/prog.kd" 2>&1 1>/dev/null)
if ! grep -q 'cache hit' <<< "$ERR2"; then
    echo "FAIL: second compile of identical source did not report 'cache hit'"
    echo "stderr: $ERR2"
    exit 1
fi
KEY2=$(grep -o 'cache hit [0-9a-f]*' <<< "$ERR2" | awk '{print $3}')
if [[ -n "$KEY1" && "$KEY1" != "$KEY2" ]]; then
    echo "FAIL: hit key ($KEY2) != miss key ($KEY1) for identical source"
    exit 1
fi
RC=$(run_exit "$TMP/b")
if [[ "$RC" -ne 55 ]]; then
    echo "FAIL: cache-hit build returned $RC (expected 55)"
    exit 1
fi
# A cache hit must still produce a runnable, correct executable.
if [[ ! -x "$TMP/b" ]]; then
    echo "FAIL: cache hit did not produce an executable"
    exit 1
fi

# --- 3. Change one character: expect a cache MISS with a DIFFERENT key. ---
write_prog 11
ERR3=$("$KARDC" -o "$TMP/c" "$TMP/prog.kd" 2>&1 1>/dev/null)
if ! grep -q 'cache miss' <<< "$ERR3"; then
    echo "FAIL: compile after source change did not report 'cache miss'"
    echo "stderr: $ERR3"
    exit 1
fi
KEY3=$(grep -o 'cache miss [0-9a-f]*' <<< "$ERR3" | awk '{print $3}')
if [[ -n "$KEY1" && "$KEY1" == "$KEY3" ]]; then
    echo "FAIL: changed source produced the SAME cache key ($KEY3) — no invalidation"
    exit 1
fi
RC=$(run_exit "$TMP/c")
if [[ "$RC" -ne 89 ]]; then
    echo "FAIL: changed build returned $RC (expected 89 = fib(11))"
    exit 1
fi

# --- 4. --no-cache: expect NO hit/miss line, but a correct build. ---
ERR4=$("$KARDC" --no-cache -o "$TMP/d" "$TMP/prog.kd" 2>&1 1>/dev/null)
if grep -qE 'cache (hit|miss)' <<< "$ERR4"; then
    echo "FAIL: --no-cache still consulted the cache"
    echo "stderr: $ERR4"
    exit 1
fi
RC=$(run_exit "$TMP/d")
if [[ "$RC" -ne 89 ]]; then
    echo "FAIL: --no-cache build returned $RC (expected 89)"
    exit 1
fi

echo "PASS: AOT compile cache: miss -> hit (object reuse) -> miss-on-change -> --no-cache bypass"
