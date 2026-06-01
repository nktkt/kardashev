#!/usr/bin/env bash
# v30 Phase 164: the C backend (`--emit-c`) drops non-escaping heap-owning
# top-level locals (String -> kd__str_drop, Vec -> kd__vec_drop) at function
# exit (RAII). A local is dropped only when ALL its uses are borrows (&s/&mut s)
# — never returned or moved by value — and the fn has no early `return`; an
# escaping/uncertain local is left alone (a sound leak, never a double-free).
#
# Verified two ways: (1) the usual differential gate (LLVM-AOT exit+stdout ==
# emitted-C), and (2) an ASan + LeakSanitizer oracle — the freed locals are
# leak-clean AND the escape cases are NOT double-freed. Skips cleanly if no C
# compiler / no ASan.
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

# Does ASan + LeakSanitizer work here? (Probe with a known leak.)
HAVE_ASAN=0
printf 'int main(){(void)__builtin_malloc(8);return 0;}\n' > "$TMP/probe.c"
if "$CC_BIN" -fsanitize=address -o "$TMP/probe" "$TMP/probe.c" 2>/dev/null; then
    ASAN_OPTIONS=detect_leaks=1 "$TMP/probe" >/dev/null 2>&1 || HAVE_ASAN=1
fi
echo "Using kardc at: $KARDC ; cc: $CC_BIN ; asan-leak: $([ $HAVE_ASAN -eq 1 ] && echo yes || echo no)"

# Differential gate: LLVM-AOT (exit+stdout) == emitted-C.
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    local lo; lo=$("$TMP/llvm" 2>&1); local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C"; head -5 "$TMP/cc"; exit 1; }
    local co; co=$("$TMP/cbin" 2>&1); local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    [[ "$lo" == "$co" ]] || { echo "FAIL [$name]: stdout differs"; exit 1; }
    echo "PASS [$name/diff]: LLVM == C (exit $lrc)"; }

# ASan oracle: the emitted C is leak-clean AND crash-clean. NOTE: a clean
# program legitimately exits with its own nonzero value (e.g. str_len), so we
# do NOT check the exit code — we grep ASan's STDERR for an error report (ASan
# prints to stderr only on a detected leak / use-after-free / double-free).
asan_ok() { local name="$1" src="$2"
    [[ $HAVE_ASAN -eq 0 ]] && { echo "SKIP [$name/asan]: no leak sanitizer"; return; }
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>/dev/null || { echo "FAIL [$name/asan]: --emit-c"; exit 1; }
    "$CC_BIN" -fsanitize=address -fwrapv -O1 -o "$TMP/ab" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name/asan]: cc"; head -5 "$TMP/cc"; exit 1; }
    ASAN_OPTIONS=detect_leaks=1 "$TMP/ab" >/dev/null 2>"$TMP/run" || true
    if grep -qE "ERROR: AddressSanitizer|detected memory leaks|SUMMARY: AddressSanitizer" "$TMP/run"; then
        echo "FAIL [$name/asan]: $(grep -oE 'detected memory leaks|heap-use-after-free|attempting double free|SUMMARY: AddressSanitizer.*' "$TMP/run" | head -1)"
        exit 1
    fi
    echo "PASS [$name/asan]: leak-clean + crash-clean"; }

# 1) String built then only borrowed -> dropped at exit (leak-clean)
cat > "$TMP/a.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut s = string_new();
    string_push_str(&mut s, "hello ");
    string_push_str(&mut s, int_to_string(42));
    print_str(&s);
    str_len(&s)
}
EOF
diff_ok str_local "$TMP/a.kd"; asan_ok str_local "$TMP/a.kd"

# 2) Vec built then only borrowed -> dropped at exit (1000-element churn)
cat > "$TMP/b.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    let mut i = 0;
    while i < 1000 { vec_push(&mut v, i * i); i = i + 1; }
    print(vec_get(&v, 7));
    vec_len(&v)
}
EOF
diff_ok vec_local "$TMP/b.kd"; asan_ok vec_local "$TMP/b.kd"

# 3) String RETURNED from a helper (escapes) -> NOT freed in the helper (the
#    caller owns it); the caller borrows it -> dropped there. No double-free.
cat > "$TMP/c.kd" <<'EOF'
fn mk(n: i64) -> String ! { alloc } { let mut s = string_new(); string_push_str(&mut s, int_to_string(n)); s }
fn main() -> i64 ! { io, alloc } { let r = mk(7); print_str(&r); str_len(&r) }
EOF
diff_ok str_returned "$TMP/c.kd"; asan_ok str_returned "$TMP/c.kd"

# 4) String MOVED into a user fn by value (escapes the caller, callee owns it)
cat > "$TMP/d.kd" <<'EOF'
fn consume(s: String) -> i64 ! { alloc } { str_len(&s) }
fn main() -> i64 ! { alloc } { let mut s = string_new(); string_push_str(&mut s, "abc"); consume(s) }
EOF
diff_ok str_moved "$TMP/d.kd"; asan_ok str_moved "$TMP/d.kd"

# 5) two independent heap locals, both only borrowed -> both dropped
cat > "$TMP/e.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut a = string_new(); string_push_str(&mut a, "x");
    let mut v = vec_new(); vec_push(&mut v, 9); vec_push(&mut v, 8);
    print_str(&a);
    str_len(&a) + vec_len(&v)
}
EOF
diff_ok two_locals "$TMP/e.kd"; asan_ok two_locals "$TMP/e.kd"

echo "PASS: Phase 164 — C backend Drop/RAII (differentially gated + ASan leak/double-free oracle)"
