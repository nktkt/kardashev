#!/usr/bin/env bash
# Phase 74 CAPSTONE (Roadmap v12 — the real stdlib): a CSV statistics
# aggregator in kardashev (examples/csvstats). It READS data — the thing v11
# could not do — grouping `category,value` rows and reporting per-category
# count + sum + the running global max, in sorted order.
#   1. Builds the real examples/csvstats/main.kd, JIT + AOT. The sorted report
#      is `apple 14 3 / banana 6 2 / cherry 2 1`, and the witness 10307022
#      encodes total=22, gmax=7, 3 categories, 1 malformed row skipped.
#   2. Exercises the whole v12 line: parse_int (with an Option-driven skip of a
#      malformed row), str_split, HashMap aggregation, i64_max, sort, and
#      int_to_string + str_concat formatting.
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

SRC=""
for cand in \
    "${TEST_SRCDIR:-}/_main/examples/csvstats/main.kd" \
    "${TEST_SRCDIR:-}/kardashev/examples/csvstats/main.kd" \
    "${RUNFILES_DIR:-}/_main/examples/csvstats/main.kd" \
    "${RUNFILES_DIR:-}/kardashev/examples/csvstats/main.kd" \
    "examples/csvstats/main.kd"; do
    if [[ -f "$cand" ]]; then SRC="$cand"; break; fi
done
[[ -z "$SRC" ]] && { echo "FAIL: examples/csvstats/main.kd not found"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# JIT: the sorted report + the witness as the final (return) line.
want=$'apple 14 3\nbanana 6 2\ncherry 2 1\n10307022'
got=$("$KARDC" "$SRC" 2>/dev/null)
[[ "$got" == "$want" ]] || { echo "FAIL [csv/jit]:"; diff <(echo "$want") <(echo "$got"); exit 1; }
echo "PASS [csv/jit]: sorted per-category report + witness 10307022"

# AOT: the three report lines + exit code = 10307022 & 255 = 206.
"$KARDC" --no-cache -o "$TMP/csv" "$SRC" >/dev/null 2>&1
set +e; aout=$("$TMP/csv"); rc=$?; set -e
[[ "$rc" -eq 206 ]] || { echo "FAIL [csv/aot]: exit $rc expected 206"; exit 1; }
[[ "$aout" == $'apple 14 3\nbanana 6 2\ncherry 2 1' ]] || { echo "FAIL [csv/aot]: report mismatch; got: $aout"; exit 1; }
echo "PASS [csv/aot]: same report, exit 206 (= witness & 255)"

echo "ALL PHASE 74 SMOKE TESTS PASSED"
