#!/usr/bin/env bash
# kardashev benchmark harness. For each workload, AOT-compiles the kardashev
# program (kardc -O2) and the equivalent C reference (clang -O2), runs both
# best-of-3, checks they produce the SAME result, and reports kardashev's runtime
# as a ratio to C. Turns "performance unmeasured" into reproducible numbers.
# Usage: bench/run.sh   (from the repo root; needs kardc + clang + /usr/bin/time)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
KARDC=""
for c in "$ROOT/compiler/kardc" "$ROOT/build.local/kardc"; do
    [[ -x "$c" ]] && { KARDC="$c"; break; }
done
[[ -z "$KARDC" ]] && { echo "kardc not found (build it: make -f Makefile.local kardc)"; exit 1; }
CLANG="$(command -v clang || true)"
[[ -z "$CLANG" ]] && { echo "clang not found (needed for the C reference)"; exit 1; }
TIME=/usr/bin/time
[[ -x "$TIME" ]] || { echo "/usr/bin/time not found (GNU time needed for wall-clock)"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/kardashev" 2>/dev/null

# best-of-3 wall-clock seconds for a binary; leaves its stdout in $TMP/o.
best() {
    local bin="$1" t="999" x
    for r in 1 2 3; do
        x=$( { "$TIME" -f "%e" "$bin" >"$TMP/o" 2>"$TMP/t"; } 2>&1; tail -n1 "$TMP/t" )
        t=$(python3 -c "print('$x' if float('$x') < float('$t') else '$t')")
    done
    echo "$t"
}

printf "%-10s %14s %14s %9s %s\n" "workload" "kardashev(-O2)" "C(clang -O2)" "ratio" "result"
printf "%-10s %14s %14s %9s %s\n" "--------" "--------------" "------------" "-----" "------"
fails=0
for w in fib loop collatz; do
    "$KARDC" --no-cache -O2 -o "$TMP/${w}_k" "$HERE/${w}.kd" >/dev/null 2>&1 || { echo "$w: kardc build FAILED"; fails=$((fails+1)); continue; }
    "$CLANG" -O2 "$HERE/${w}.c" -o "$TMP/${w}_c" 2>/dev/null || { echo "$w: clang build FAILED"; fails=$((fails+1)); continue; }
    kt=$(best "$TMP/${w}_k"); ko="$(cat "$TMP/o")"
    ct=$(best "$TMP/${w}_c"); co="$(cat "$TMP/o")"
    ratio=$(python3 -c "print(f'{$kt/$ct:.2f}x' if $ct > 0 else 'n/a')")
    if [[ "$ko" == "$co" ]]; then res="ok ($ko)"; else res="MISMATCH k=$ko c=$co"; fails=$((fails+1)); fi
    printf "%-10s %13ss %13ss %9s %s\n" "$w" "$kt" "$ct" "$ratio" "$res"
done
echo ""
[[ "$fails" -eq 0 ]] && echo "all workloads correct (kardashev == C)" || { echo "$fails problem(s)"; exit 1; }
