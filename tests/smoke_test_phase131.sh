#!/usr/bin/env bash
# v24 Phase 131 smoke test: parser error recovery (panic-mode). After a
# statement parse error the parser resynchronizes to the next `;`/boundary, so
# it reports ONE diagnostic per real error and still surfaces the LATER errors
# (recovery), instead of cascading spurious "expected ;" noise off each one.
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

# --- two malformed `let`s: exactly two errors (no cascade), both surfaced ---
printf 'fn main() -> i64 {\n    let a = ;\n    let b = ;\n    a + b\n}\n' > "$TMP/two.kd"
out=$("$KARDC" "$TMP/two.kd" 2>&1) || true
n=$(grep -c 'error' <<<"$out")
[[ "$n" -eq 2 ]] || { echo "FAIL [cascade]: expected 2 errors (one per bad let), got $n"; echo "$out"; exit 1; }
[[ "$(grep -c 'expected expression' <<<"$out")" -eq 2 ]] || { echo "FAIL [recover]: both errors should be the real 'expected expression'"; echo "$out"; exit 1; }
grep -qi 'expected ;' <<<"$out" && { echo "FAIL [cascade]: spurious 'expected ;' cascade leaked"; echo "$out"; exit 1; }
echo "PASS [recover]: 2 malformed lets => 2 clean errors (both surfaced, no cascade)"

# --- a valid program is unaffected (recovery never triggers) ---
echo 'fn main() -> i64 { let a = 1; let b = 2; a + b }' > "$TMP/ok.kd"
got=$("$KARDC" "$TMP/ok.kd" 2>&1); [[ "$got" == "3" ]] || { echo "FAIL [ok]: valid program broke (got '$got')"; exit 1; }
echo "PASS [ok]: a valid program is unaffected (recovery only runs on error)"

# --- a genuinely missing `;` is still reported exactly once ---
printf 'fn main() -> i64 {\n    let a = 5\n    a\n}\n' > "$TMP/ms.kd"
out=$("$KARDC" "$TMP/ms.kd" 2>&1) || true
[[ "$(grep -c 'error' <<<"$out")" -eq 1 ]] || { echo "FAIL [missing-semi]: expected 1 error"; echo "$out"; exit 1; }
echo "PASS [missing-semi]: a real missing ';' is reported exactly once"

echo "PASS: Phase 131 — parser panic-mode recovery (one diagnostic per error, no cascade)"
