#!/usr/bin/env bash
# v24 Phase 133 smoke test: stable error codes + `kardc --explain Exxxx`.
# A diagnostic whose message matches a curated code shows `[Exxxx]` in the
# header (rustc-style `type error[E0308]:`); `--explain` prints the extended
# explanation; an unknown code lists the known ones.
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

# --- a type mismatch is coded E0308 ---
printf 'fn main() -> i64 {\n    let x = true;\n    x + 1\n}\n' > "$TMP/ty.kd"
out=$("$KARDC" "$TMP/ty.kd" 2>&1) || true
grep -qE '^type error\[E0308\]: ' <<<"$out" || { echo "FAIL [code]: type error not coded E0308"; echo "$out"; exit 1; }
echo "PASS [code]: a type mismatch is tagged [E0308]"

# --- a moved value is coded E0382 ---
cat > "$TMP/mv.kd" <<'EOF'
struct S { v: i64 }
fn take(s: S) -> i64 { s.v }
fn main() -> i64 { let a = S { v: 5 }; let b = take(a); take(a) }
EOF
out=$("$KARDC" "$TMP/mv.kd" 2>&1) || true
grep -qE '\[E0382\]' <<<"$out" || { echo "FAIL [code]: moved value not coded E0382"; echo "$out"; exit 1; }
echo "PASS [code]: a use-after-move is tagged [E0382]"

# --- --explain prints the explanation ---
out=$("$KARDC" --explain E0308 2>&1); rc=$?
[[ "$rc" -eq 0 ]] || { echo "FAIL [explain]: --explain E0308 exit $rc"; exit 1; }
grep -qi "mismatched types" <<<"$out" || { echo "FAIL [explain]: no title"; echo "$out"; exit 1; }
grep -qi "cast explicitly with" <<<"$out" || { echo "FAIL [explain]: no explanation body"; echo "$out"; exit 1; }
# case-insensitive code accepted
"$KARDC" --explain e0382 >/dev/null 2>&1 || { echo "FAIL [explain]: lowercase code not accepted"; exit 1; }
echo "PASS [explain]: --explain prints the title + body (case-insensitive)"

# --- unknown code: non-zero + lists known codes ---
out=$("$KARDC" --explain E9999 2>&1); rc=$?
[[ "$rc" -ne 0 ]] || { echo "FAIL [unknown]: --explain bogus should fail"; exit 1; }
grep -q "E0308" <<<"$out" || { echo "FAIL [unknown]: known codes not listed"; echo "$out"; exit 1; }
echo "PASS [unknown]: an unknown code fails and lists the known ones"

echo "PASS: Phase 133 — error codes in diagnostics + kardc --explain"
