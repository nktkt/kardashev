#!/usr/bin/env bash
# v24 Phase 130 smoke test: rich diagnostics — a rustc-style source snippet with
# a caret, reporting the USER's own line number (the front-end parses a
# ~450-line prelude-prepended source, so a naive report shows "455" for user
# line 3; the renderer recovers the offset). Covers parse / type / borrow errors
# and the in-message secondary position; a valid program still compiles cleanly.
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

# Asserts the snippet structure (header / `-->` location / gutter+caret) and a
# user-relative line number, and that no 3-digit prelude-offset line leaks.
expect_snippet() {
    local name="$1" out="$2" kind="$3" wantline="$4"
    grep -qE "^$kind error(\\[E[0-9]+\\])?: " <<<"$out" || { echo "FAIL [$name]: no '$kind error:' header"; echo "$out"; exit 1; }
    grep -q -- '-->' <<<"$out" || { echo "FAIL [$name]: no '-->' location line"; echo "$out"; exit 1; }
    grep -qE '^ *[0-9]+ \| ' <<<"$out" || { echo "FAIL [$name]: no source gutter line"; echo "$out"; exit 1; }
    grep -qE '\^' <<<"$out" || { echo "FAIL [$name]: no caret"; echo "$out"; exit 1; }
    grep -qE -- "--> .*:$wantline:" <<<"$out" || { echo "FAIL [$name]: location not at user line $wantline"; echo "$out"; exit 1; }
    if grep -qE -- '--> .*:[0-9]{3,}:' <<<"$out"; then echo "FAIL [$name]: a 3-digit prelude-offset line leaked"; echo "$out"; exit 1; fi
}

# --- type error: caret under the `+`, user line 3 ---
cat > "$TMP/ty.kd" <<'EOF'
fn main() -> i64 {
    let x = true;
    x + 1
}
EOF
out=$("$KARDC" "$TMP/ty.kd" 2>&1) || true
expect_snippet "type" "$out" "type" 3
echo "PASS [type]: type-error snippet at user line 3 with a caret"

# --- parse error: user line 2 ---
printf 'fn main() -> i64 {\n    let y = ;\n}\n' > "$TMP/pa.kd"
out=$("$KARDC" "$TMP/pa.kd" 2>&1) || true
expect_snippet "parse" "$out" "parse" 2
echo "PASS [parse]: parse-error snippet at user line 2"

# --- borrow error: primary at user line 6, AND the in-message "(moved at N:M)"
#     position is user-relative (single digit), not a prelude offset. ---
cat > "$TMP/bo.kd" <<'EOF'
struct S { v: i64 }
fn take(s: S) -> i64 { s.v }
fn main() -> i64 {
    let a = S { v: 5 };
    let b = take(a);
    take(a)
}
EOF
out=$("$KARDC" "$TMP/bo.kd" 2>&1) || true
expect_snippet "borrow" "$out" "borrow" 6
grep -q "use of moved value" <<<"$out" || { echo "FAIL [borrow]: message text not preserved"; exit 1; }
grep -qE 'moved at [0-9]{1,2}:' <<<"$out" || { echo "FAIL [borrow]: in-message position not user-relative"; echo "$out"; exit 1; }
grep -qE 'moved at [0-9]{3,}:' <<<"$out" && { echo "FAIL [borrow]: in-message position still prelude-offset"; echo "$out"; exit 1; }
echo "PASS [borrow]: borrow-error snippet + user-relative in-message position"

# --- a valid program still compiles + runs, no diagnostics on stderr ---
echo 'fn main() -> i64 { 6 * 7 }' > "$TMP/ok.kd"
out=$("$KARDC" "$TMP/ok.kd" 2>/tmp/ok.stderr); rc=$?
[[ "$rc" -eq 0 && "$out" == "42" ]] || { echo "FAIL [ok]: valid program broke (rc=$rc out=$out)"; exit 1; }
[[ -s /tmp/ok.stderr ]] && { echo "FAIL [ok]: stderr not empty for a valid program"; cat /tmp/ok.stderr; exit 1; }
echo "PASS [ok]: a valid program compiles + runs with no diagnostics"

echo "PASS: Phase 130 — rich diagnostics (snippet + caret + user-relative line numbers)"
