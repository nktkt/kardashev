#!/usr/bin/env bash
# v24 Phase 132 smoke test: the opt-in lint pass (`kardc -W`). Lints:
#   - unused `let` bindings (sound — a name used via a closure, a fn-pointer
#     call, a method/builtin call, or a match binding does NOT warn);
#   - code unreachable after a `return` / `break` / `continue`.
# Warnings are non-fatal (the program still compiles + runs) and OPT-IN (no `-W`
# => no warnings), so the existing corpus is unaffected.
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

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- unused let: warns under -W, at the user's line, and still runs ---
printf 'fn main() -> i64 {\n    let used = 5;\n    let dead = 99;\n    used\n}\n' > "$TMP/u.kd"
out=$("$KARDC" -W "$TMP/u.kd" 2>&1); rc=$?
grep -q "warning: unused variable \`dead\`" <<<"$out" || { echo "FAIL [unused]: no unused-var warning"; echo "$out"; exit 1; }
grep -qE -- '--> .*:3:' <<<"$out" || { echo "FAIL [unused]: warning not at user line 3"; echo "$out"; exit 1; }
grep -q '^5$' <<<"$out" || { echo "FAIL [unused]: program did not still run (want 5)"; echo "$out"; exit 1; }
echo "PASS [unused]: -W warns on an unused let (line 3) and still runs"

# --- unreachable code after return ---
printf 'fn f() -> i64 {\n    return 1;\n    let z = 2;\n    z\n}\nfn main() -> i64 { f() }\n' > "$TMP/r.kd"
out=$("$KARDC" -W "$TMP/r.kd" 2>&1)
grep -q "warning: unreachable" <<<"$out" || { echo "FAIL [unreach]: no unreachable warning"; echo "$out"; exit 1; }
echo "PASS [unreach]: -W warns on code after a return"

# --- opt-in: WITHOUT -W, no warnings even with an unused var ---
out=$("$KARDC" "$TMP/u.kd" 2>&1); rc=$?
[[ "$rc" -eq 0 && "$out" == "5" ]] || { echo "FAIL [optin]: program broke without -W (rc=$rc out=$out)"; exit 1; }
grep -qi "warning" <<<"$out" && { echo "FAIL [optin]: warnings emitted without -W"; echo "$out"; exit 1; }
echo "PASS [optin]: no warnings without -W (existing corpus unaffected)"

# --- soundness: every binding IS used (closure / fn-ptr call / builtin method /
#     match binding). Must produce ZERO warnings (no false positives). ---
cat > "$TMP/ok.kd" <<'EOF'
fn add(a: i64, b: i64) -> i64 { a + b }
fn main() -> i64 {
    let n = 10;
    let clo = || n + 1;
    let r = clo();
    let mut v = vec_new();
    vec_push(&mut v, r);
    let len = vec_len(&v);
    let opt = Some(len);
    let m = match opt { Some(x) => x, None => 0, };
    add(m, n)
}
EOF
out=$("$KARDC" -W "$TMP/ok.kd" 2>&1)
if grep -qi "warning" <<<"$out"; then echo "FAIL [sound]: false-positive unused warning"; echo "$out"; exit 1; fi
echo "PASS [sound]: no false positives (closure / fn-ptr / method / match uses recognized)"

echo "PASS: Phase 132 — opt-in lint (-W): unused vars + unreachable code, non-fatal, no false positives"
