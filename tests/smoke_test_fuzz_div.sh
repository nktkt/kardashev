#!/usr/bin/env bash
# v19 Phase 113: a differential fuzzer for the DIVISION / MODULO / BITWISE codegen
# — the paths the arithmetic fuzzer (Phase 110) deliberately skipped, and a
# classic miscompile source: signed division TRUNCATES toward zero (not floors),
# and `%` takes the sign of the DIVIDEND (C / Rust / LLVM semantics, NOT Python's
# floor-mod). The reference mirrors C semantics exactly via helper functions, and
# the generator emits the kardashev expression and the Python reference in
# LOCKSTEP (no fragile post-hoc rewrite). Divisors and shift amounts are non-zero
# small literals so the program never traps; dividends may be negative (via
# `-9..9` literals and subtraction), exercising the sign paths of sdiv/srem.
# Covers `+ - * / % & | ^ << >>` (>> is arithmetic on i64). JIT == AOT == ref.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "PASS [fuzz-div]: SKIPPED (no python3)"; exit 0; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

N="${FUZZ_N:-40}"
SEED="${FUZZ_SEED:-6151}"
RANDOM=$SEED

# gen DEPTH sets G_KD (kardashev source) and G_PY (Python reference) for one
# sub-expression, in lockstep. Target ops `/ % << >>` use C-semantics helpers in
# the Python form; `+ - * & | ^` are identical in both (python's bitwise on signed
# ints matches two's-complement when the final result is wrapped to i64).
gen() {
    local depth=$1
    if (( depth <= 0 || RANDOM % 3 == 0 )); then
        local v=$(( RANDOM % 19 - 9 ))           # literal -9..9
        G_KD="$v"; G_PY="$v"; return
    fi
    gen $((depth-1)); local akd="$G_KD" apy="$G_PY"
    local p=$(( RANDOM % 7 ))
    case $p in
        0) gen $((depth-1)); G_KD="($akd + $G_KD)"; G_PY="($apy + $G_PY)";;
        1) gen $((depth-1)); G_KD="($akd - $G_KD)"; G_PY="($apy - $G_PY)";;
        2) gen $((depth-1)); G_KD="($akd * $G_KD)"; G_PY="($apy * $G_PY)";;
        3) local L=$(( RANDOM % 9 + 1 )); G_KD="($akd / $L)"; G_PY="cdiv($apy,$L)";;
        4) local L=$(( RANDOM % 9 + 1 )); G_KD="($akd % $L)"; G_PY="crem($apy,$L)";;
        5) gen $((depth-1)); local b=$(( RANDOM % 3 )); local op="&"; (( b==1 )) && op="|"; (( b==2 )) && op="^"
           G_KD="($akd $op $G_KD)"; G_PY="($apy $op $G_PY)";;
        6) local S=$(( RANDOM % 8 ))
           if (( RANDOM % 2 == 0 )); then G_KD="($akd << $S)"; G_PY="shl($apy,$S)"
           else G_KD="($akd >> $S)"; G_PY="shr($apy,$S)"; fi;;
    esac
}

PYREF='
def w(v):
    v &= (1<<64)-1
    return v-(1<<64) if v>=(1<<63) else v
def cdiv(a,b):
    q=abs(a)//abs(b)
    return w(-q if (a<0)!=(b<0) else q)
def crem(a,b):
    return w(a-cdiv(a,b)*b)
def shl(a,n): return w(a<<n)
def shr(a,n): return w(a>>n)
'

fails=0
for ((i=0; i<N; i++)); do
    gen 3
    ref=$(python3 -c "$PYREF
print(w($G_PY))" 2>/dev/null)
    [[ -z "$ref" ]] && { echo "FAIL [fuzz-div ref]: could not evaluate $G_PY"; fails=$((fails+1)); continue; }
    printf 'fn main() -> i64 { %s }\n' "$G_KD" > "$TMP/f.kd"
    jit=$("$KARDC" "$TMP/f.kd" 2>/dev/null | tail -n1)
    if [[ "$jit" != "$ref" ]]; then
        echo "FAIL [fuzz-div jit]: kd=$G_KD  py=$G_PY  ref=$ref  jit=$jit"; fails=$((fails+1)); continue
    fi
    "$KARDC" --no-cache -o "$TMP/f" "$TMP/f.kd" >/dev/null 2>&1
    "$TMP/f" >/dev/null 2>&1; aot=$?
    want=$(( ( ref % 256 + 256 ) % 256 ))
    if [[ "$aot" -ne "$want" ]]; then
        echo "FAIL [fuzz-div aot]: kd=$G_KD  ref=$ref  aot=$aot  want=$want"; fails=$((fails+1))
    fi
done

[[ "$fails" -eq 0 ]] || { echo "FAIL [fuzz-div]: $fails/$N mismatched (seed $SEED)"; exit 1; }
echo "PASS [fuzz-div]: $N random div/mod/bitwise programs — JIT == AOT == C-semantics reference (seed $SEED)"
echo "ALL DIV/BITWISE FUZZ TESTS PASSED"
