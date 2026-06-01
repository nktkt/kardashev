#!/usr/bin/env bash
# v30 Phase 165: the C backend (`--emit-c`) grows to CLOSURES + fn VALUES. A
# closure lowers to a hoisted top-level fn `__cl_<n>(void* env, args)` over a
# generated `struct __clenv_<n>` of its SCALAR (i64/bool) by-value captures
# (free vars computed by the backend itself — there's no typecheck info). A fn
# value is the fat pointer `struct kdfn<arity> { int64_t (*fn)(void*, ...); void*
# env; }`. A top-level fn used as a value gets a thunk (null env). A call through
# a fn value (`f(x)` where f is a fn-typed param, or `(g)(x)`) unpacks the fat
# pointer and calls `fn(env, args)`.
#
# Soundness: the env is a STACK compound literal, so a fn value MUST NOT escape
# its function. A closure in return position — or a fn returning `fn(..)->..` —
# is REFUSED (its env would dangle; an earlier draft confirmed ASan
# stack-use-after-scope on exactly this). FnMut (mutated capture), non-scalar
# captures, and channel captures are also refused.
#
# Verified by the differential gate (LLVM-AOT exit+stdout == emitted-C) AND, when
# available, an ASan + LeakSanitizer + stack-use-after-return oracle. Skips with
# no C compiler.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[[ -z "$CC_BIN" ]] && { echo "SKIP: no C compiler"; exit 0; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

HAVE_ASAN=0
printf 'int main(){(void)__builtin_malloc(8);return 0;}\n' > "$TMP/probe.c"
if "$CC_BIN" -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" 2>/dev/null; then
    ASAN_OPTIONS=detect_leaks=1 "$TMP/probe" >/dev/null 2>&1 || HAVE_ASAN=1
fi
echo "Using kardc at: $KARDC ; cc: $CC_BIN ; asan: $([ $HAVE_ASAN -eq 1 ] && echo yes || echo no)"

diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    local lo; lo=$("$TMP/llvm" 2>&1); local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C"; head -5 "$TMP/cc"; exit 1; }
    local co; co=$("$TMP/cbin" 2>&1); local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    [[ "$lo" == "$co" ]] || { echo "FAIL [$name]: stdout differs"; exit 1; }
    if [[ $HAVE_ASAN -eq 1 ]]; then
        "$CC_BIN" -fsanitize=address -fwrapv -O1 -o "$TMP/ab" "$TMP/out.c" 2>/dev/null
        ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1" "$TMP/ab" >/dev/null 2>"$TMP/run" || true
        grep -qE "ERROR: AddressSanitizer|detected memory leaks" "$TMP/run" && { echo "FAIL [$name/asan]: $(grep -oE 'stack-use-after-(return|scope)|detected memory leaks|heap-use-after-free' "$TMP/run" | head -1)"; exit 1; }
    fi
    echo "PASS [$name]: LLVM == C (exit $lrc)$([ $HAVE_ASAN -eq 1 ] && echo ' + asan-clean')"; }

# 1) a capturing closure passed to a higher-order fn
cat > "$TMP/a.kd" <<'EOF'
fn apply(f: fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 { let n = 10; apply(|y| y + n, 32) }
EOF
diff_ok cap_hof "$TMP/a.kd"

# 2) a zero-capture closure
cat > "$TMP/b.kd" <<'EOF'
fn apply(f: fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 { apply(|y| y * 2, 21) }
EOF
diff_ok zero_cap "$TMP/b.kd"

# 3) a top-level fn used as a value
cat > "$TMP/c.kd" <<'EOF'
fn inc(x: i64) -> i64 { x + 1 }
fn apply(f: fn(i64) -> i64, x: i64) -> i64 { f(x) }
fn main() -> i64 { apply(inc, 41) }
EOF
diff_ok fn_value "$TMP/c.kd"

# 4) a let-bound closure called later (same-block stack env)
cat > "$TMP/d.kd" <<'EOF'
fn main() -> i64 { let n = 7; let f = |y| y + n; (f)(35) }
EOF
diff_ok let_closure "$TMP/d.kd"

# 5) a closure called twice inside the HOF + a 2-arg closure capturing
cat > "$TMP/e.kd" <<'EOF'
fn twice(f: fn(i64) -> i64, x: i64) -> i64 { f(f(x)) }
fn apply2(g: fn(i64, i64) -> i64, a: i64, b: i64) -> i64 { g(a, b) }
fn main() -> i64 { let k = 3; let c = 100; twice(|y| y + k, 0) + apply2(|x, y| x + y + c, 30, 12) }
EOF
diff_ok twice_arity2 "$TMP/e.kd"

# 6) SOUNDNESS: a fn returning a closure (stack env would dangle) is refused
cat > "$TMP/r.kd" <<'EOF'
fn mk(n: i64) -> fn(i64) -> i64 { |y| y + n }
fn main() -> i64 { let f = mk(10); (f)(32) }
EOF
out=$("$KARDC" --emit-c "$TMP/r.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [escape_refused]: returning a closure should be refused"; exit 1; }
echo "$out" | grep -qi "escapes its function\|outside the C-backend subset" || { echo "FAIL [escape_refused]: missing diagnostic: $out"; exit 1; }
echo "PASS [escape_refused]: a returned (escaping) closure is refused"

# 7) SOUNDNESS: an FnMut closure (mutates a captured binding) is refused
cat > "$TMP/m.kd" <<'EOF'
fn run3(f: fn(i64) -> i64) -> i64 { f(1); f(1); f(1) }
fn main() -> i64 { let mut total = 0; run3(|x| { total = total + x; total }) }
EOF
out=$("$KARDC" --emit-c "$TMP/m.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [fnmut_refused]: an FnMut closure should be refused"; exit 1; }
echo "$out" | grep -qi "FnMut\|outside the C-backend subset" || { echo "FAIL [fnmut_refused]: missing diagnostic: $out"; exit 1; }
echo "PASS [fnmut_refused]: an FnMut (mutated-capture) closure is refused"

echo "PASS: Phase 165 — C backend closures + fn values (differential + ASan-gated)"
