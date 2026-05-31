#!/usr/bin/env bash
# v23 Phase 129 smoke test: the C-source backend (`--emit-c`), differentially
# gated against the LLVM backend. For each subset program (i64/bool, the full
# operator set, let/mut/assign, if-else as a value, while, recursion + mutual
# recursion, const), the LLVM AOT exit code must EQUAL the exit code of the
# emitted-C-compiled-by-the-system-cc binary. An out-of-subset program (a
# struct) must be REFUSED with a clean error, never miscompiled.
#
# Skips cleanly if no C compiler is present (the LLVM path is unaffected).
set -uo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

CC=""
for c in cc clang gcc; do
    if command -v "$c" >/dev/null 2>&1; then CC="$c"; break; fi
done
if [[ -z "$CC" ]]; then
    echo "SKIP [phase129]: no C compiler (cc/clang/gcc) — the LLVM backend is unaffected"
    exit 0
fi
echo "Using C compiler: $CC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# `-fwrapv` makes C signed overflow wrap two's-complement, matching kardashev's
# defined i64 wrapping (so a fuzzer-style overflow can't diverge via C UB).
pass=0
differential() {
    local name="$1" prog="$2"
    printf '%s' "$prog" > "$TMP/p.kd"

    # LLVM AOT reference.
    if ! "$KARDC" --no-cache -o "$TMP/p_llvm" "$TMP/p.kd" >/dev/null 2>&1; then
        echo "FAIL [$name]: LLVM AOT compile failed"; exit 1
    fi
    "$TMP/p_llvm" >/dev/null 2>&1; local e_llvm=$?

    # C backend.
    if ! "$KARDC" --emit-c "$TMP/p.kd" > "$TMP/p.c" 2>"$TMP/p.err"; then
        echo "FAIL [$name]: --emit-c rejected an in-subset program: $(cat "$TMP/p.err")"; exit 1
    fi
    if ! "$CC" -fwrapv -O2 "$TMP/p.c" -o "$TMP/p_c" 2>"$TMP/cc.err"; then
        echo "FAIL [$name]: cc rejected the emitted C:"; head -5 "$TMP/cc.err"; exit 1
    fi
    "$TMP/p_c" >/dev/null 2>&1; local e_c=$?

    if [[ "$e_llvm" != "$e_c" ]]; then
        echo "FAIL [$name]: LLVM exit $e_llvm != C-backend exit $e_c"
        echo "--- emitted C ---"; cat "$TMP/p.c"; exit 1
    fi
    pass=$((pass + 1))
}

differential "arith"      'fn main() -> i64 { (3 * 4 + 1) - 7 / 2 + (100 % 7) }'
differential "fib-rec"    'fn fib(n: i64) -> i64 { if n < 2 { n } else { fib(n-1) + fib(n-2) } } fn main() -> i64 { fib(15) }'
differential "factorial"  'fn fact(n: i64) -> i64 { if n <= 1 { 1 } else { n * fact(n-1) } } fn main() -> i64 { fact(6) }'
differential "while-sum"  'fn main() -> i64 { let mut i = 0; let mut s = 0; while i < 20 { s = s + i; i = i + 1; } s }'
differential "and-or"     'fn main() -> i64 { let a = 7; let r = if a > 5 && a < 10 { 1 } else { 0 }; let q = if a > 99 || a == 7 { 2 } else { 0 }; (r + q) * 10 }'
differential "shortcirc"  'fn main() -> i64 { let f = false; let r = if f && ((10 / 0) == 0) { 1 } else { 9 }; let t = true; let q = if t || ((10 / 0) == 0) { 8 } else { 1 }; r + q }'
differential "bitwise"    'fn main() -> i64 { (6 & 3) + (5 | 2) + (12 ^ 10) + (1 << 5) + (255 >> 2) + (0 - (~7)) }'
differential "mod-signed" 'fn main() -> i64 { let a = 0 - 17; let b = 0 - 5; (a % 5) + (17 % b) + 100 }'
differential "bool-fn"    'fn neg(b: bool) -> bool { !b } fn main() -> i64 { if neg(false) && !neg(true) { 42 } else { 0 } }'
differential "const"      'const A: i64 = 6; const B: i64 = A * 7; fn main() -> i64 { B }'
differential "mutual-rec" 'fn even(n: i64) -> bool { if n == 0 { true } else { odd(n-1) } } fn odd(n: i64) -> bool { if n == 0 { false } else { even(n-1) } } fn main() -> i64 { if even(20) { 7 } else { 0 } }'
differential "nested-if"  'fn cl(x: i64) -> i64 { if x < 0 { 0 } else { if x > 100 { 100 } else { x } } } fn main() -> i64 { cl(150) - cl(0 - 5) + cl(42) }'
echo "PASS [diff]: $pass programs — LLVM AOT exit == C-backend exit, all matched"

# Out-of-subset: an out-of-subset program must be REFUSED with a clean,
# non-crashing error (the backend never emits wrong C). v29 Phase 157 brought
# STRUCTS into the subset, so the negative now uses an ENUM (a later phase).
cat > "$TMP/bad.kd" <<'EOF'
trait Show { fn show(&self) -> i64; }
fn main() -> i64 { 0 }
EOF
set +e; out=$("$KARDC" --emit-c "$TMP/bad.kd" 2>&1); rc=$?; set -e
[[ "$rc" -eq 0 ]] && { echo "FAIL [reject]: --emit-c should refuse a trait program"; exit 1; }
[[ "$rc" -ge 128 ]] && { echo "FAIL [reject]: --emit-c crashed (signal $((rc-128))) on a trait program"; exit 1; }
grep -qi "outside the C-backend subset" <<< "$out" || { echo "FAIL [reject]: missing subset diagnostic; got: $out"; exit 1; }
echo "PASS [reject]: an out-of-subset (trait) program is refused with a clean error"

echo "PASS: Phase 129 — the --emit-c C backend matches LLVM across the i64/bool subset (differentially gated)"
