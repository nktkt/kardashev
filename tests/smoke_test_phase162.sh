#!/usr/bin/env bash
# v30 Phase 162: the C backend (`--emit-c`) grows to `String` + heap strings. A
# faithful C `struct kdstr { char* data; int64_t len; int64_t cap; }` runtime is
# emitted (only when the program uses String), with cap==0 = borrowed/read-only
# literal (copy-on-write on mutation), mirroring the LLVM backend's IR builtins
# exactly: string_new / str_len / str_char_at (OOB -> -1) / str_push_byte /
# string_push_str / str_eq / str_substring (clamped) / int_to_string /
# print_str / print_no_nl / println / print. Differentially gated: the LLVM-AOT
# (exit code AND stdout) must equal the emitted-C binary's. Skips with no cc.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[[ -z "$CC_BIN" ]] && { echo "SKIP: no C compiler"; exit 0; }
echo "Using kardc at: $KARDC ; cc: $CC_BIN"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Differential on BOTH exit code and stdout (String programs print).
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    local lo; lo=$("$TMP/llvm" 2>&1); local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused in-subset: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C:"; head -5 "$TMP/cc"; exit 1; }
    local co; co=$("$TMP/cbin" 2>&1); local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    [[ "$lo" == "$co" ]] || { echo "FAIL [$name]: stdout differs; LLVM=[$lo] C=[$co]"; exit 1; }
    echo "PASS [$name]: LLVM == C (exit $lrc)"; }

# 1) string literal: read length + print
cat > "$TMP/a.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } { let s = "hello"; print_str(&s); str_len(&s) }
EOF
diff_ok lit_print "$TMP/a.kd"

# 2) heap string built byte-by-byte (string_new + str_push_byte)
cat > "$TMP/b.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut s = string_new();
    str_push_byte(&mut s, 72); str_push_byte(&mut s, 105); str_push_byte(&mut s, 33);
    print_str(&s); str_len(&s)
}
EOF
diff_ok push_byte "$TMP/b.kd"

# 3) string_push_str (copy-on-write of a literal, then concat) + int_to_string
cat > "$TMP/c.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let mut s = string_new();
    string_push_str(&mut s, "n=");
    string_push_str(&mut s, int_to_string(42));
    print_str(&s); str_len(&s)
}
EOF
diff_ok push_str_int "$TMP/c.kd"

# 4) str_eq + str_substring (clamped) + str_char_at (OOB -> -1)
cat > "$TMP/d.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let s = "hello world";
    let sub = str_substring(&s, 6, 5);
    print_str(&sub);
    let eq = if str_eq(&sub, &"world") { 100 } else { 0 };
    let oob = str_char_at(&s, 999);
    eq + str_len(&sub) + (0 - oob)
}
EOF
diff_ok eq_sub_oob "$TMP/d.kd"

# 5) String as a fn param + return value (greet)
cat > "$TMP/e.kd" <<'EOF'
fn greet(name: String) -> String ! { alloc } {
    let mut out = string_new();
    string_push_str(&mut out, "Hi ");
    string_push_str(&mut out, name);
    out
}
fn main() -> i64 ! { io, alloc } { let g = greet("Bob"); print_str(&g); str_len(&g) }
EOF
diff_ok param_return "$TMP/e.kd"

# 6) str_char_at loop (read each byte) + print_no_nl
cat > "$TMP/f.kd" <<'EOF'
fn main() -> i64 ! { io, alloc } {
    let s = "ABC";
    print_no_nl(&s); print_no_nl(&"!");
    let mut total = 0; let mut i = 0;
    while i < str_len(&s) { total = total + str_char_at(&s, i); i = i + 1; }
    total
}
EOF
diff_ok char_loop "$TMP/f.kd"

# 7) the boundary holds: a trait is still refused
cat > "$TMP/t.kd" <<'EOF'
trait Show { fn show(&self) -> i64; }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" --emit-c "$TMP/t.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [trait_refused]: a trait program should be refused"; exit 1; }
echo "$out" | grep -qi "outside the C-backend subset" || { echo "FAIL [trait_refused]: missing diagnostic"; exit 1; }
echo "PASS [trait_refused]: an out-of-subset (trait) program is still refused"

echo "PASS: Phase 162 — C backend String + heap strings (differentially gated vs LLVM)"
