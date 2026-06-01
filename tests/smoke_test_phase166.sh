#!/usr/bin/env bash
# v30 Phase 166: the C backend (`--emit-c`) grows to GENERICS via scalar
# monomorphization. A generic fn over a type param T — where every concrete T the
# program uses lowers to a C scalar (i64/bool/char/unit, all int64_t) — is
# emitted ONCE with T bound to int64_t (the single monomorphization the C backend
# needs, since all scalars share one C representation). A generic fn instantiated
# at a NON-scalar type (struct/String/Vec) is REFUSED (that would need a distinct
# instance — a documented follow-on), never miscompiled. Differentially gated:
# LLVM-AOT exit+stdout == emitted-C. Skips with no C compiler.
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
    local lo; lo=$("$TMP/llvm" 2>&1); local lrc=$?
    "$KARDC" --emit-c "$src" > "$TMP/out.c" 2>"$TMP/e" || { echo "FAIL [$name]: --emit-c refused: $(cat "$TMP/e")"; exit 1; }
    "$CC_BIN" -fwrapv -O2 -o "$TMP/cbin" "$TMP/out.c" 2>"$TMP/cc" || { echo "FAIL [$name]: cc rejected the C"; head -5 "$TMP/cc"; exit 1; }
    local co; co=$("$TMP/cbin" 2>&1); local crc=$?
    [[ "$lrc" -eq "$crc" ]] || { echo "FAIL [$name]: LLVM exit $lrc != C exit $crc"; exit 1; }
    [[ "$lo" == "$co" ]] || { echo "FAIL [$name]: stdout differs"; exit 1; }
    echo "PASS [$name]: LLVM == C (exit $lrc)"; }

# 1) identity generic, two i64 instantiations -> ONE int64_t monomorphization
cat > "$TMP/a.kd" <<'EOF'
fn id<T>(x: T) -> T { x }
fn main() -> i64 { id(7) + id(35) }
EOF
diff_ok id "$TMP/a.kd"

# 2) a generic over T used in the body (if/else picking a T)
cat > "$TMP/b.kd" <<'EOF'
fn pick<T>(b: bool, a: T, c: T) -> T { if b { a } else { c } }
fn main() -> i64 { pick(true, 40, 0) + pick(false, 0, 2) }
EOF
diff_ok pick "$TMP/b.kd"

# 3) a generic instantiated at BOTH i64 and bool (still one int64_t instance)
cat > "$TMP/c.kd" <<'EOF'
fn id<T>(x: T) -> T { x }
fn main() -> i64 { let n = id(35); if id(true) { n + 7 } else { n } }
EOF
diff_ok id_i64_and_bool "$TMP/c.kd"

# 4) a generic higher-order fn (fn(T)->T param) + a capturing closure arg
cat > "$TMP/d.kd" <<'EOF'
fn apply<T>(f: fn(T) -> T, x: T) -> T { f(x) }
fn main() -> i64 { let n = 10; apply(|y| y + n, 32) }
EOF
diff_ok generic_hof "$TMP/d.kd"

# 5) two distinct generic fns + recursion
cat > "$TMP/e.kd" <<'EOF'
fn first<T>(a: T, b: T) -> T { a }
fn maxg<T>(a: T, b: T, gt: bool) -> T { if gt { a } else { b } }
fn main() -> i64 { first(42, 99) + maxg(0, 6, false) }
EOF
diff_ok two_generics "$TMP/e.kd"

# 6) SOUNDNESS: a generic instantiated at a NON-scalar type (struct) is refused
cat > "$TMP/s.kd" <<'EOF'
struct P { x: i64, y: i64 }
fn id<T>(x: T) -> T { x }
fn main() -> i64 { let p = id(P { x: 3, y: 4 }); p.x + p.y }
EOF
out=$("$KARDC" --emit-c "$TMP/s.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [struct_gen_refused]: a struct-instantiated generic should be refused"; exit 1; }
echo "$out" | grep -qi "non-scalar type\|outside the C-backend subset" || { echo "FAIL [struct_gen_refused]: missing diagnostic: $out"; exit 1; }
echo "PASS [struct_gen_refused]: a generic instantiated at a non-scalar type is refused"

# 7) SOUNDNESS: a const-generic fn is refused (no const-eval in the C backend)
cat > "$TMP/cg.kd" <<'EOF'
fn cap<const N: i64>() -> i64 { 5 }
fn main() -> i64 { cap::<3>() }
EOF
out=$("$KARDC" --emit-c "$TMP/cg.kd" 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [const_gen_refused]: a const-generic fn should be refused"; exit 1; }
echo "PASS [const_gen_refused]: a const-generic fn is refused"

echo "PASS: Phase 166 — C backend generics (scalar monomorphization, differentially gated)"
