#!/usr/bin/env bash
# v18 Phase 110: a DIFFERENTIAL fuzzer for the arithmetic/codegen path. Generates
# many random i64 expression programs (`+ - * ( )` over small literals, bounded
# depth — always type- and borrow-valid, no div so no div-by-zero/truncation
# ambiguity) and checks three oracles agree: the JIT-printed value, the AOT exit
# code (value & 255), and a Python reference evaluating the SAME expression. A
# mismatch is a real miscompile (the class of bug the v17 adversarial review found
# by hand — now hunted systematically). Deterministic: seeded from a fixed base so
# CI failures reproduce.
set -uo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "PASS [fuzz-arith]: SKIPPED (no python3 for the reference oracle)"; exit 0; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

N="${FUZZ_N:-40}"          # number of random programs
SEED="${FUZZ_SEED:-1730}"  # fixed base seed → reproducible
RANDOM=$SEED

# Generate a random expression. $1 = remaining depth. Keeps literals small and
# avoids deep multiplication blow-up so values stay in a sane i64 range (the
# reference + JIT compare the FULL value; AOT compares the low byte).
gen() {
    local depth=$1
    if (( depth <= 0 || RANDOM % 3 == 0 )); then
        echo -n "$(( RANDOM % 9 + 1 ))"           # literal 1..9
        return
    fi
    local op_pick=$(( RANDOM % 3 ))
    local op="+"; (( op_pick == 1 )) && op="-"; (( op_pick == 2 )) && op="*"
    echo -n "("; gen $((depth-1)); echo -n " $op "; gen $((depth-1)); echo -n ")"
}

fails=0
for ((i=0; i<N; i++)); do
    expr=$(gen 4)
    # Reference: evaluate the SAME expression as wrapped 64-bit signed arithmetic.
    ref=$(python3 -c "
v=($expr)
v &= (1<<64)-1
if v >= (1<<63): v -= (1<<64)
print(v)
")
    printf 'fn main() -> i64 { %s }\n' "$expr" > "$TMP/f.kd"
    # JIT: the trailing printed line is main's i64 return (full value).
    jit=$("$KARDC" "$TMP/f.kd" 2>/dev/null | tail -n1)
    if [[ "$jit" != "$ref" ]]; then
        echo "FAIL [fuzz-arith jit]: expr=$expr  ref=$ref  jit=$jit"; fails=$((fails+1)); continue
    fi
    # AOT: exit code is (value & 255), unsigned. (--no-cache bypasses the AOT
    # cache, so no per-iteration cache clear is needed.)
    "$KARDC" --no-cache -o "$TMP/f" "$TMP/f.kd" >/dev/null 2>&1
    "$TMP/f" >/dev/null 2>&1; aot=$?
    want_aot=$(( ( ref % 256 + 256 ) % 256 ))
    if [[ "$aot" -ne "$want_aot" ]]; then
        echo "FAIL [fuzz-arith aot]: expr=$expr  ref=$ref  aot_exit=$aot  want=$want_aot"; fails=$((fails+1))
    fi
done

[[ "$fails" -eq 0 ]] || { echo "FAIL [fuzz-arith]: $fails/$N programs mismatched (seed $SEED)"; exit 1; }
echo "PASS [fuzz-arith]: $N random i64 expression programs — JIT == AOT == python reference (seed $SEED)"
echo "ALL ARITH FUZZ TESTS PASSED"
