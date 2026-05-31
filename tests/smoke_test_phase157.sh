#!/usr/bin/env bash
# v29 Phase 157: the C backend (`--emit-c`) grows to STRUCTS — typedefs, struct
# literals (compound literals), field access/assignment, and struct-typed
# lets/params/returns. Each program is DIFFERENTIALLY GATED: the LLVM-AOT exit
# code must equal the emitted-C-compiled-by-the-system-cc exit code. Skips
# cleanly if no C compiler is present.
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

# Differential: LLVM-AOT exit == emitted-C exit.
diff_ok() { local name="$1" src="$2"
    "$KARDC" --no-cache -o "$TMP/llvm" "$src" >/dev/null 2>&1 || { echo "FAIL [$name]: LLVM AOT compile"; exit 1; }
    "$TMP/llvm" >/dev/null 2>&1; local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused an in-subset program: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C:"; head -5 "$TMP/cc"; exit 1; }
    "$TMP/cbin" >/dev/null 2>&1; local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    echo "PASS [$name]: LLVM == C == $lrc"; }

# 1) struct literal, by-value param, field access, returned struct
cat > "$TMP/a.kd" <<'EOF'
struct Point { x: i64, y: i64 }
fn make(a: i64, b: i64) -> Point { Point { x: a, y: b } }
fn sum(p: Point) -> i64 { p.x + p.y }
fn main() -> i64 { let p = make(30, 12); sum(p) }
EOF
diff_ok struct_basic "$TMP/a.kd"

# 2) mutable struct local + field assignment
cat > "$TMP/m.kd" <<'EOF'
struct Counter { n: i64, step: i64 }
fn main() -> i64 {
    let mut c = Counter { n: 0, step: 7 };
    c.n = c.n + c.step;
    c.n = c.n + c.step;
    c.n
}
EOF
diff_ok struct_mut "$TMP/m.kd"

# 3) nested struct + nested field access
cat > "$TMP/n.kd" <<'EOF'
struct Inner { v: i64 }
struct Outer { a: Inner, b: i64 }
fn main() -> i64 { let o = Outer { a: Inner { v: 40 }, b: 2 }; o.a.v + o.b }
EOF
diff_ok struct_nested "$TMP/n.kd"

# 4) a bool struct field driving control flow
cat > "$TMP/b.kd" <<'EOF'
struct Flag { on: bool, hi: i64, lo: i64 }
fn pick(f: Flag) -> i64 { if f.on { f.hi } else { f.lo } }
fn main() -> i64 { pick(Flag { on: true, hi: 9, lo: 1 }) + pick(Flag { on: false, hi: 9, lo: 1 }) }
EOF
diff_ok struct_bool "$TMP/b.kd"

# 5) the out-of-subset boundary holds: an enum is still refused
cat > "$TMP/e.kd" <<'EOF'
enum E { A, B }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" --emit-c "$TMP/e.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [enum_refused]: an enum program should be refused"; exit 1; }
echo "$out" | grep -qi "outside the C-backend subset" || { echo "FAIL [enum_refused]: missing diagnostic"; exit 1; }
echo "PASS [enum_refused]: an out-of-subset (enum) program is still refused"

echo "PASS: Phase 157 — C backend structs (differentially gated vs LLVM)"
