#!/usr/bin/env bash
# v18 Phase 111: a differential fuzzer for the CONTROL-FLOW codegen path — extends
# the arithmetic fuzzer (Phase 110) with `let` bindings, comparisons (`< == >`),
# and `if/else` branch selection. Each generated program binds two i64 locals and
# returns the value of an `if (<cmp>) { <expr> } else { <expr> }` over them; a
# Python reference evaluates the SAME program (signed 64-bit, branch selection
# mirrored) and must agree with both the JIT-printed value and the AOT exit code
# (value & 255). Hunts miscompiles in comparison lowering, branch selection, and
# local-variable codegen. Seeded for reproducibility.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "PASS [fuzz-control]: SKIPPED (no python3)"; exit 0; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

N="${FUZZ_N:-30}"
SEED="${FUZZ_SEED:-2027}"
RANDOM=$SEED

# An arithmetic expression. $1 = remaining depth. When the global ALLOW_VARS=1
# the leaves may be `a`/`b` (only valid AFTER both are bound — i.e. in the
# cond/then/else, never in the initializers of a and b themselves).
ALLOW_VARS=0
gen() {
    local depth=$1
    if (( depth <= 0 || RANDOM % 3 == 0 )); then
        if (( ALLOW_VARS == 1 )); then
            local pick=$(( RANDOM % 5 ))
            if   (( pick == 0 )); then echo -n "a"; return
            elif (( pick == 1 )); then echo -n "b"; return; fi
        fi
        echo -n "$(( RANDOM % 9 + 1 ))"
        return
    fi
    local op_pick=$(( RANDOM % 3 ))
    local op="+"; (( op_pick == 1 )) && op="-"; (( op_pick == 2 )) && op="*"
    echo -n "("; gen $((depth-1)); echo -n " $op "; gen $((depth-1)); echo -n ")"
}
# A comparison between two sub-expressions.
gen_cmp() {
    local c_pick=$(( RANDOM % 3 ))
    local c="<"; (( c_pick == 1 )) && c="=="; (( c_pick == 2 )) && c=">"
    gen 2; echo -n " $c "; gen 2
}

fails=0
for ((i=0; i<N; i++)); do
    ALLOW_VARS=0; aexpr=$(gen 2); bexpr=$(gen 2)   # initializers: literals only
    ALLOW_VARS=1; cond=$(gen_cmp); thenE=$(gen 2); elseE=$(gen 2)  # may use a,b
    prog="let a = $aexpr ; let b = $bexpr ; if $cond { $thenE } else { $elseE }"
    ref=$(python3 -c "
a=($aexpr); b=($bexpr)
def w(v):
    v &= (1<<64)-1
    return v-(1<<64) if v>=(1<<63) else v
a=w(a); b=w(b)
v = ($thenE) if ($cond) else ($elseE)
print(w(v))
")
    printf 'fn main() -> i64 { %s }\n' "$prog" > "$TMP/f.kd"
    jit=$("$KARDC" "$TMP/f.kd" 2>/dev/null | tail -n1)
    if [[ "$jit" != "$ref" ]]; then
        echo "FAIL [fuzz-control jit]: prog={$prog}  ref=$ref  jit=$jit"; fails=$((fails+1)); continue
    fi
    "$KARDC" --no-cache -o "$TMP/f" "$TMP/f.kd" >/dev/null 2>&1
    "$TMP/f" >/dev/null 2>&1; aot=$?
    want_aot=$(( ( ref % 256 + 256 ) % 256 ))
    if [[ "$aot" -ne "$want_aot" ]]; then
        echo "FAIL [fuzz-control aot]: prog={$prog}  ref=$ref  aot=$aot  want=$want_aot"; fails=$((fails+1))
    fi
done

[[ "$fails" -eq 0 ]] || { echo "FAIL [fuzz-control]: $fails/$N mismatched (seed $SEED)"; exit 1; }
echo "PASS [fuzz-control]: $N random let/cmp/if programs — JIT == AOT == python reference (seed $SEED)"
echo "ALL CONTROL-FLOW FUZZ TESTS PASSED"
