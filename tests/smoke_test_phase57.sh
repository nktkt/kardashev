#!/usr/bin/env bash
# Phase 57 smoke test (Roadmap v10): parse + bind const-generic params, symbolic
# array lengths, and the tuple-let annotation — the DECLARATION shell only (no
# instantiation yet; that is Phase 58).
#   1. `struct Mat<const N: i64> { data: [i64; N] }` parses + type-checks (N is a
#      symbolic const-generic length, not a const-eval failure) and a program
#      using `let (a, b): (i64, i64) = (3, 4)` runs (signal 7), JIT + AOT.
#   2. Negatives: a non-i64 const param (`const N: bool`), an arity-mismatched
#      tuple-let annotation, and a type-mismatched one are all rejected clearly.
# (The ast_print round-trip of both spellings is guarded by parser_test.)
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rejects() { # name file needle
    local n=$1 f=$2 needle=$3 out
    set +e; out=$("$KARDC" "$f" 2>&1); set -e
    if "$KARDC" "$f" >/dev/null 2>&1; then
        echo "FAIL [$n]: expected REJECTION, but it compiled"; exit 1
    fi
    if [[ -n "$needle" ]] && ! grep -qi "$needle" <<< "$out"; then
        echo "FAIL [$n]: rejected but missing '$needle'; got: $out"; exit 1
    fi
    echo "PASS [$n]: rejected as expected"
}

# 1. acceptance: const-generic struct decl-shell + tuple-let annotation.
cat > "$TMP/p57.kd" <<'EOF'
struct Mat<const N: i64> { data: [i64; N] }
struct Buf<T, const CAP: i64> { x: T }
fn main() -> i64 {
    let (a, b): (i64, i64) = (3, 4);
    a + b
}
EOF
jit=$("$KARDC" "$TMP/p57.kd" | tail -1)
[[ "$jit" == "7" ]] || { echo "FAIL [accept/jit]: expected 7 got '$jit'"; exit 1; }
"$KARDC" --no-cache -o "$TMP/p57" "$TMP/p57.kd" >/dev/null
set +e; "$TMP/p57" >/dev/null; rc=$?; set -e
[[ "$rc" -ne 7 ]] && { echo "FAIL [accept/aot]: exit $rc expected 7"; exit 1; }
echo "PASS [decl-shell]: const-generic struct + tuple-let annotation, JIT 7, AOT exit $rc"

# 2. negatives.
printf 'struct Mat<const N: bool> { data: [i64; N] }\nfn main() -> i64 { 0 }\n' > "$TMP/nb.kd"
rejects const-non-i64 "$TMP/nb.kd" "must be"
printf 'fn main() -> i64 { let (a, b): (i64, i64) = (1, 2, 3); a + b }\n' > "$TMP/na.kd"
rejects tuple-arity "$TMP/na.kd" "tuple"
printf 'fn main() -> i64 ! { alloc } { let (a, b): (i64, String) = (1, 2); a }\n' > "$TMP/nt.kd"
rejects tuple-elem-type "$TMP/nt.kd" "annotation says"

echo "PASS: Phase 57 — const-generic params + symbolic lengths + tuple-let annotation (JIT + AOT)"
