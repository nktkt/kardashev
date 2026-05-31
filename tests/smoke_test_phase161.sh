#!/usr/bin/env bash
# v29 Phase 161: wire the fuzzers as a randomized C-vs-LLVM ORACLE. Generates
# many random programs over the C-backend subset (i64 arithmetic + comparisons +
# `&&`/`||` + nested if/else + helper functions + recursion + `while` loops) and
# asserts the LLVM-AOT exit code EQUALS the `--emit-c`-compiled exit code. The
# LLVM backend is already gated against a Python reference by the arith/control/
# div fuzzers, so C == LLVM transitively means C == reference. Deterministic
# (fixed seed) + reproducible. Skips cleanly if no C compiler is present.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[[ -z "$CC_BIN" ]] && { echo "SKIP [phase161]: no C compiler"; exit 0; }
echo "Using kardc at: $KARDC ; cc: $CC_BIN"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

N="${FUZZ_N:-50}"
SEED="${FUZZ_SEED:-2906}"
RANDOM=$SEED

# Generate a random i64 expression of bounded depth over a set of helper params
# (a b c, all i64) + small literals. Operators: + - * and a nested if/else with a
# comparison guard + a `&&`/`||` combinator — exactly the C-backend's i64 subset
# (division/modulo omitted to avoid a div-by-zero divergence between a kardashev
# panic and C UB; those paths are covered by the deterministic phase129 smoke).
# gen <depth> <allowCall>: <allowCall>=1 may emit `f(...)` (used in main's body);
# =0 never does (used for f's OWN body, so f is non-recursive — no stack overflow).
gen() {
    local depth=$1 allowCall=$2
    if (( depth <= 0 )); then
        local r=$((RANDOM % 5))
        case $r in
            0) echo -n "a";; 1) echo -n "b";; 2) echo -n "c";;
            *) echo -n "$((RANDOM % 19 - 9))";;
        esac
        return
    fi
    local choices=4; (( allowCall )) && choices=5
    local k=$((RANDOM % choices))
    if (( k == 0 )); then
        local ops=("+" "-" "*"); local op=${ops[$((RANDOM % 3))]}
        echo -n "("; gen $((depth-1)) $allowCall; echo -n " $op "; gen $((depth-1)) $allowCall; echo -n ")"
    elif (( k == 1 )); then
        local cs=("<" "<=" ">" ">=" "==" "!="); local cmp=${cs[$((RANDOM % 6))]}
        echo -n "(if ("; gen $((depth-1)) $allowCall; echo -n " $cmp "; gen $((depth-1)) $allowCall
        echo -n ") { "; gen $((depth-1)) $allowCall; echo -n " } else { "; gen $((depth-1)) $allowCall; echo -n " })"
    elif (( k == 2 )); then
        local bo=("&&" "||"); local b=${bo[$((RANDOM % 2))]}
        echo -n "(if (("; gen $((depth-1)) $allowCall; echo -n " < "; gen $((depth-1)) $allowCall
        echo -n ") $b ("; gen $((depth-1)) $allowCall; echo -n " > "; gen $((depth-1)) $allowCall
        echo -n ")) { "; gen $((depth-1)) $allowCall; echo -n " } else { "; gen $((depth-1)) $allowCall; echo -n " })"
    elif (( k == 3 )); then
        echo -n "(0 - "; gen $((depth-1)) $allowCall; echo -n ")"
    else
        echo -n "f("; gen $((depth-1)) $allowCall; echo -n ", "; gen $((depth-1)) $allowCall; echo -n ", "; gen $((depth-1)) $allowCall; echo -n ")"
    fi
}

fails=0
for ((i=0; i<N; i++)); do
    body=$(gen 4 1)
    helper=$(gen 3 0)
    # `f` is a small pure helper (no recursion via f-in-f: gen at depth 3 may emit
    # f(...) — bounded recursion is fine and exercises the call path; the loop
    # below caps depth, so f terminates).
    cat > "$TMP/p.kd" <<EOF
fn f(a: i64, b: i64, c: i64) -> i64 { $helper }
fn main() -> i64 {
    let a = $((RANDOM % 13 - 6));
    let b = $((RANDOM % 13 - 6));
    let c = $((RANDOM % 13 - 6));
    let mut acc = 0;
    let mut i = 0;
    while i < 4 { acc = acc + ($body); i = i + 1; }
    acc
}
EOF
    if ! "$KARDC" --no-cache -o "$TMP/llvm" "$TMP/p.kd" >/dev/null 2>&1; then
        echo "FAIL [gen]: LLVM AOT rejected a generated program (seed $SEED, iter $i)"; cat "$TMP/p.kd"; exit 1
    fi
    "$TMP/llvm" >/dev/null 2>&1; lrc=$?
    if ! "$KARDC" --emit-c "$TMP/p.kd" > "$TMP/p.c" 2>"$TMP/e"; then
        echo "FAIL [emit-c]: refused an in-subset generated program: $(cat "$TMP/e")"; cat "$TMP/p.kd"; exit 1
    fi
    if ! "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/p.c" 2>"$TMP/cc"; then
        echo "FAIL [cc]: rejected generated C:"; head -5 "$TMP/cc"; cat "$TMP/p.c"; exit 1
    fi
    "$TMP/cbin" >/dev/null 2>&1; crc=$?
    if [[ "$lrc" -ne "$crc" ]]; then
        echo "FAIL [oracle]: LLVM exit $lrc != C exit $crc"; cat "$TMP/p.kd"; fails=$((fails+1))
    fi
done

[[ "$fails" -eq 0 ]] || { echo "FAIL [phase161]: $fails/$N programs diverged (seed $SEED)"; exit 1; }
echo "PASS [phase161]: $N random programs — LLVM-AOT exit == --emit-c exit (seed $SEED)"
