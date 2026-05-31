#!/usr/bin/env bash
# v26 Phase 141 smoke test: or-patterns `p1 | p2 | … => body`. The parser splits
# an or-pattern arm into one arm per alternative (deep-cloning the body), so the
# match compiler / exhaustiveness checker see only single-pattern arms. Covers
# enum variants, int literals, and shared payload bindings (`A(x) | B(x) => x`).
# JIT + AOT.
#
# (Match GUARDS — `pat if cond =>` — are a documented follow-on: the Maranget
# decision-tree match compiler needs guard-backtracking support.)
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

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

run_eq() { # name file want
    local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?
    [[ "$rc" -eq "$3" ]] || { echo "FAIL [$1/aot]: exit $rc want $3"; exit 1; }
}

# --- or-pattern over enum variants ---
cat > "$TMP/v.kd" <<'EOF'
enum Color { Red, Green, Blue }
fn classify(c: Color) -> i64 { match c { Red | Blue => 1, Green => 2 } }
fn main() -> i64 { classify(Red) + classify(Blue) * 10 + classify(Green) * 100 }
EOF
run_eq variants "$TMP/v.kd" 211
echo "PASS [variants]: Red | Blue => 1, Green => 2 (211), JIT + AOT"

# --- or-pattern over int literals (with a wildcard tail) ---
cat > "$TMP/n.kd" <<'EOF'
fn small(n: i64) -> i64 { match n { 1 | 2 | 3 => 1, _ => 0 } }
fn main() -> i64 { small(2) + small(3) * 10 + small(5) * 100 }
EOF
run_eq literals "$TMP/n.kd" 11
echo "PASS [literals]: 1 | 2 | 3 => 1 (11)"

# --- or-pattern with a shared payload binding ---
cat > "$TMP/x.kd" <<'EOF'
enum E { A(i64), B(i64), C }
fn val(e: E) -> i64 { match e { A(x) | B(x) => x, C => 0 } }
fn main() -> i64 { val(A(5)) + val(B(7)) * 10 + val(C) }
EOF
run_eq binding "$TMP/x.kd" 75
echo "PASS [binding]: A(x) | B(x) => x binds in every alternative (75)"

echo "PASS: Phase 141 — or-patterns (p1 | p2 => e), JIT + AOT"
