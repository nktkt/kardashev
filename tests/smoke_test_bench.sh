#!/usr/bin/env bash
# Phase 120 (Roadmap v21 — "prove it"): a correctness guard for the benchmark
# suite (bench/). For each workload it AOT-compiles the kardashev program and the
# C reference and asserts they produce the SAME result (a wrong-output regression
# in codegen would diverge) within a generous timeout (a catastrophic slowdown /
# hang would time out). The PERFORMANCE ratios live in BENCHMARKS.md and are NOT
# asserted here (CI timing is too variable). Skips if clang is unavailable.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
BENCH=""
for d in "${TEST_SRCDIR:-}/_main/bench" "${TEST_SRCDIR:-}/kardashev/bench" \
         "${RUNFILES_DIR:-}/_main/bench" "${RUNFILES_DIR:-}/kardashev/bench" "bench"; do
    if [[ -f "$d/fib.kd" ]]; then BENCH="$d"; break; fi
done
[[ -z "$BENCH" ]] && { echo "FAIL: bench/ not found"; exit 1; }
CLANG="$(command -v clang || true)"
[[ -z "$CLANG" ]] && { echo "PASS [bench]: SKIPPED (no clang for the C reference)"; exit 0; }
echo "Using kardc at: $KARDC ; bench at: $BENCH"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
TO="timeout 60"; command -v timeout >/dev/null 2>&1 || TO=""

for w in fib loop collatz; do
    "$KARDC" --no-cache -O2 -o "$TMP/${w}_k" "$BENCH/${w}.kd" >/dev/null 2>&1 || { echo "FAIL [bench/$w]: kardc -O2 build failed"; exit 1; }
    "$CLANG" -O2 "$BENCH/${w}.c" -o "$TMP/${w}_c" 2>/dev/null || { echo "FAIL [bench/$w]: clang build failed"; exit 1; }
    ko=$($TO "$TMP/${w}_k" 2>/dev/null) || { echo "FAIL [bench/$w]: kardashev binary failed/timed out"; exit 1; }
    co=$($TO "$TMP/${w}_c" 2>/dev/null) || { echo "FAIL [bench/$w]: C binary failed/timed out"; exit 1; }
    [[ "$ko" == "$co" ]] || { echo "FAIL [bench/$w]: kardashev=$ko != C=$co (codegen correctness regression)"; exit 1; }
    echo "PASS [bench/$w]: kardashev == C ($ko)"
done
echo "ALL BENCHMARK CORRECTNESS TESTS PASSED (see BENCHMARKS.md for the perf ratios)"
