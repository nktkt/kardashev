#!/usr/bin/env bash
# v28 Phase 156: monomorphization control + specialization + code-bloat
# mitigation. The compiler monomorphizes generics ON DEMAND and DEDUPLICATES
# identical instantiations (each (fn, type-args) instance is emitted exactly
# once), and a concrete `impl Trait for T0` SPECIALIZES (takes precedence over) a
# blanket `impl<T: Bound> Trait for T`. The new `--mono-report` flag prints the
# monomorphization footprint so code bloat is visible.
set -uo pipefail
KARDC=""
for c in "${TEST_SRCDIR:-}/_main/compiler/kardc" "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
         "${RUNFILES_DIR:-}/_main/compiler/kardc" "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
         "./compiler/kardc" "./build.local/kardc"; do
    [[ -n "$c" && -x "$c" ]] && { KARDC="$c"; break; }; done
[[ -z "$KARDC" ]] && { echo "FAIL: kardc not found"; exit 1; }
echo "Using kardc at: $KARDC"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
run_eq() { local jit; jit=$("$KARDC" "$2" 2>&1 | head -1)
    [[ "$jit" == "$3" ]] || { echo "FAIL [$1/jit]: want $3 got '$jit'"; exit 1; }
    "$KARDC" --no-cache -o "$TMP/b" "$2" >/dev/null 2>&1 || { echo "FAIL [$1/aot]: compile"; exit 1; }
    "$TMP/b" >/dev/null; local rc=$?; [[ "$rc" -eq $(( $3 & 255 )) ]] || { echo "FAIL [$1/aot]: exit $rc want $(( $3 & 255 ))"; exit 1; }
    echo "PASS [$1]: $4"; }

# 1) dedup: `id` called 3x with i64 + 1x bool -> exactly 2 instances (id__i64 once)
cat > "$TMP/d.kd" <<'EOF'
fn id<T>(x: T) -> T { x }
fn unused<T>(x: T) -> T { x }
fn main() -> i64 {
    let a = id(1); let b = id(2); let c = id(3);
    let d = id(true);
    a + b + c
}
EOF
report=$("$KARDC" --mono-report "$TMP/d.kd" 2>&1)
n=$(echo "$report" | head -1 | grep -oE '[0-9]+')
[[ "$n" == "2" ]] || { echo "FAIL [dedup]: want 2 instances, got $n; report: $report"; exit 1; }
echo "$report" | grep -q "id__i64" || { echo "FAIL [dedup]: no id__i64"; exit 1; }
echo "$report" | grep -q "id__bool" || { echo "FAIL [dedup]: no id__bool"; exit 1; }
echo "$report" | grep -q "unused" && { echo "FAIL [dedup]: an UNINSTANTIATED generic was emitted"; exit 1; }
echo "PASS [dedup]: id(i64)x3 + id(bool) -> 2 instances; uninstantiated 'unused' emits nothing"

# 2) the program still runs correctly with the deduped instances
run_eq dedup_runs "$TMP/d.kd" 6 "deduped generic instances still execute (1+2+3=6)"

# 3) specialization: a concrete impl takes precedence over a bounded blanket
cat > "$TMP/s.kd" <<'EOF'
trait Tag { fn tag(&self) -> i64; }
trait Describe { fn describe(&self) -> i64; }
struct Widget { v: i64 }
struct Gadget { v: i64 }
impl Tag for Widget { fn tag(&self) -> i64 { 1 } }
impl Tag for Gadget { fn tag(&self) -> i64 { 2 } }
impl<U: Tag> Describe for U { fn describe(&self) -> i64 { self.tag() * 100 } }
impl Describe for Gadget { fn describe(&self) -> i64 { 999 } }
fn show<D: Describe>(d: &D) -> i64 { d.describe() }
fn main() -> i64 {
    let w = Widget { v: 0 }; let g = Gadget { v: 0 };
    w.describe() + g.describe() + show(&w) + show(&g)
}
EOF
run_eq specialization "$TMP/s.kd" 2198 "concrete impl beats blanket: Widget=100 (blanket), Gadget=999 (concrete), x2 = 2198"

echo "PASS: Phase 156 — monomorphization dedup + specialization + --mono-report"
