#!/usr/bin/env bash
# v29 Phase 158: the C backend (`--emit-c`) grows to ENUMS + `match`. An enum
# lowers to a tagged struct `struct E { int64_t tag; int64_t p0..; }`; a variant
# constructor is a compound literal; `match` lowers to an if/else chain on the
# tag (enum) or value (int), binding scalar payloads from `.p<i>`. Differentially
# gated: LLVM-AOT exit == emitted-C exit. Skips cleanly with no C compiler.
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

# 1) unit + single-payload variants, match binds the payload
cat > "$TMP/o.kd" <<'EOF'
enum Opt { Nothing, Just(i64) }
fn unwrap(o: Opt) -> i64 { match o { Just(x) => x, Nothing => 0 } }
fn main() -> i64 { unwrap(Just(42)) + unwrap(Nothing) }
EOF
diff_ok enum_option "$TMP/o.kd"

# 2) multi-payload variants + arithmetic in arms
cat > "$TMP/m.kd" <<'EOF'
enum Shape { Dot, Line(i64), Rect(i64, i64) }
fn area(s: Shape) -> i64 { match s { Rect(w, h) => w * h, Line(len) => len, Dot => 0 } }
fn main() -> i64 { area(Rect(6, 7)) + area(Line(5)) + area(Dot) }
EOF
diff_ok enum_multi "$TMP/m.kd"

# 3) a `match` on an integer (LitInt arms + wildcard)
cat > "$TMP/i.kd" <<'EOF'
fn classify(n: i64) -> i64 { match n { 1 => 10, 2 => 20, _ => 99 } }
fn main() -> i64 { classify(1) + classify(2) + classify(5) }
EOF
diff_ok int_match "$TMP/i.kd"

# 4) a bare-binding catch-all arm + a bool-payload variant
cat > "$TMP/b.kd" <<'EOF'
enum Ev { Quit, Key(i64), Toggle(bool) }
fn code(e: Ev) -> i64 {
    match e { Key(k) => k, Toggle(on) => if on { 1 } else { 0 }, rest => 99 }
}
fn main() -> i64 { code(Key(65)) + code(Toggle(true)) + code(Quit) }
EOF
diff_ok bind_catchall "$TMP/b.kd"

# 5) the out-of-subset boundary holds: a trait is still refused
cat > "$TMP/t.kd" <<'EOF'
trait Show { fn show(&self) -> i64; }
fn main() -> i64 { 0 }
EOF
out=$("$KARDC" --emit-c "$TMP/t.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [trait_refused]: a trait program should be refused"; exit 1; }
echo "$out" | grep -qi "outside the C-backend subset" || { echo "FAIL [trait_refused]: missing diagnostic"; exit 1; }
echo "PASS [trait_refused]: an out-of-subset (trait) program is still refused"

echo "PASS: Phase 158 — C backend enums + match (differentially gated vs LLVM)"
