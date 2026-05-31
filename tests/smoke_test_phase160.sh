#!/usr/bin/env bash
# v29 Phase 160: the C backend (`--emit-c`) grows to `for`-over-range,
# `loop`-with-value, and MULTI-FILE MODULES. `for x in a..b` / `a..=b` -> a C
# `for`; `loop { ... break v; }` -> a `while(1)` whose value is the break value;
# `mod foo;` programs are merged (resolveModules on the raw source, sans prelude)
# before emission. Differentially gated: LLVM-AOT exit == emitted-C exit.
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
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    "$TMP/llvm" >/dev/null 2>&1; local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused in-subset: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C:"; head -5 "$TMP/cc"; exit 1; }
    "$TMP/cbin" >/dev/null 2>&1; local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    echo "PASS [$name]: LLVM == C == $lrc"; }

# 1) for over an exclusive range
cat > "$TMP/f.kd" <<'EOF'
fn main() -> i64 { let mut s = 0; for i in 0..10 { s = s + i; } s }
EOF
diff_ok for_range "$TMP/f.kd"

# 2) nested for over inclusive ranges
cat > "$TMP/n.kd" <<'EOF'
fn main() -> i64 { let mut s = 0; for i in 1..=3 { for j in 1..=3 { s = s + i * j; } } s }
EOF
diff_ok for_nested "$TMP/n.kd"

# 3) loop with a break value
cat > "$TMP/l.kd" <<'EOF'
fn main() -> i64 { let mut i = 0; loop { if i >= 7 { break i * 6; } else { i = i + 1; } } }
EOF
diff_ok loop_value "$TMP/l.kd"

# 4) loop with continue + break (value)
cat > "$TMP/c.kd" <<'EOF'
fn main() -> i64 {
    let mut i = 0; let mut s = 0;
    loop {
        i = i + 1;
        if i > 10 { break s; } else { }
        if i % 2 == 0 { continue; } else { }
        s = s + i;
    }
}
EOF
diff_ok loop_continue "$TMP/c.kd"

# 5) a multi-file `mod foo;` program is merged before emission
mkdir -p "$TMP/proj"
cat > "$TMP/proj/helper.kd" <<'EOF'
pub fn triple(x: i64) -> i64 { x * 3 }
EOF
cat > "$TMP/proj/main.kd" <<'EOF'
mod helper;
fn main() -> i64 { let mut s = 0; for i in 1..5 { s = s + triple(i); } s }
EOF
diff_ok module "$TMP/proj/main.kd"

echo "PASS: Phase 160 — C backend for/loop-with-value + multi-file modules (gated vs LLVM)"
