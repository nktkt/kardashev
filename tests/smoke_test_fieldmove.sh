#!/usr/bin/env bash
# Phase 99 (Roadmap v17): regression test for the FIELD-MOVE double-free fix —
# a real memory-safety bug the v17 self-hosting code surfaced. Moving a non-Copy
# struct field BY VALUE (`vec_push(&mut v, s.field)`) used to double-free: codegen
# copies the field's {ptr,len,cap} out without a per-field move flag, so the
# struct's drop freed it AGAIN. Fixed by clearing the ROOT binding's drop flag on
# a field/index partial move. Verifies (1) a Drop-counted field drops EXACTLY once
# (was twice), and (2) a loop of String field-moves has no heap corruption.
set -euo pipefail
KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc" "./build.local/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then KARDC="$candidate"; break; fi
done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc binary not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 1. A Drop-counted field moved out drops EXACTLY once (regression: was twice).
cat > "$TMP/dc.kd" <<'EOF'
trait Drop { fn drop(&mut self) ! { io }; }
struct Noisy { id: i64 }
impl Drop for Noisy { fn drop(&mut self) ! { io } { print(self.id); } }
struct S { x: Noisy, n: i64 }
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    let s = S { x: Noisy { id: 100 }, n: 1 };
    vec_push(&mut v, s.x);     // move the Noisy field out
    print(999);
    0
}
EOF
got=$("$KARDC" "$TMP/dc.kd" 2>/dev/null)
# JIT prints: 999 (print), 100 (the moved Noisy's single drop), 0 (main's return,
# echoed by the JIT). The double-free bug printed 100 TWICE; the fix => exactly once.
n100=$(grep -cx "100" <<< "$got")
[[ "$n100" -eq 1 ]] || { echo "FAIL [drop-once]: Noisy dropped $n100 times (expected 1); output: $got"; exit 1; }
echo "PASS [drop-once]: a moved-out Drop-counted field is dropped EXACTLY once (not twice)"

# 2. A loop of String field-moves: no double-free under MALLOC_CHECK_=3.
cat > "$TMP/lm.kd" <<'EOF'
struct S { name: String, n: i64 }
fn main() -> i64 ! { io, alloc } {
    let a = "aa"; let b = "bb";
    let mut src = vec_new();
    vec_push(&mut src, S { name: clone(&a), n: 1 });
    vec_push(&mut src, S { name: clone(&b), n: 2 });
    let mut names = vec_new();
    let mut i = 0;
    while i < 2 { let p = vec_get(&src, i); vec_push(&mut names, p.name); i = i + 1; }
    let g0 = vec_get(&names, 0);
    str_len(&g0)   // 2
}
EOF
"$KARDC" --no-cache -o "$TMP/lm" "$TMP/lm.kd" >/dev/null 2>&1
bad=0
for r in 1 2 3 4 5 6; do
    set +e; MALLOC_CHECK_=3 "$TMP/lm" >/dev/null 2>"$TMP/e"; rc=$?; set -e
    if [[ "$rc" -eq 134 ]] || grep -qi 'free\|corrupt' "$TMP/e"; then bad=$((bad+1)); fi
done
[[ "$bad" -eq 0 ]] || { echo "FAIL [no-double-free]: $bad/6 runs corrupted the heap"; exit 1; }
echo "PASS [no-double-free]: a loop of String field-moves is heap-clean (MALLOC_CHECK_=3 x6)"

echo "ALL FIELD-MOVE REGRESSION TESTS PASSED"
