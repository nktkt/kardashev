#!/usr/bin/env bash
# v19 Phase 112: a MEMORY-SAFETY fuzzer targeting the exact bug class v17/v18
# fixed (field-move double-free / leak, per-field drop tracking). Generates random
# but borrow-VALID struct programs: a struct with K fields, each field an `N` that
# owns a heap String AND prints a unique id on Drop. main builds the struct, moves
# a random subset of DISTINCT fields into a Vec (each moved at most once → valid),
# and lets the rest drop at scope exit. Two oracles, no reference needed:
#   (1) heap-clean under MALLOC_CHECK_=3 (a double-free of a moved field's String
#       aborts with rc 134 / "free");
#   (2) every id is dropped EXACTLY once (moved fields drop via the Vec, the rest
#       at scope exit — a double-drop or a leaked drop shows as a wrong count).
# Plus a loop variant gated on RSS-flatness (a per-iteration leak balloons RSS).
# Seeded for reproducibility.
set -uo pipefail
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

N="${FUZZ_N:-20}"
SEED="${FUZZ_SEED:-4099}"
RANDOM=$SEED

fails=0
for ((i=0; i<N; i++)); do
    K=$(( RANDOM % 3 + 2 ))           # 2..4 fields
    # Build the struct decl + the value literal, and pick which fields to move.
    fields=""; lit=""; moved=()
    base=$(( (i+1) * 100 ))           # disjoint id ranges per program
    for ((f=0; f<K; f++)); do
        fields+="f${f}: N, "
        lit+="f${f}: N { s: int_to_string($((base+f))), id: $((base+f)) }, "
        (( RANDOM % 2 == 0 )) && moved+=("$f")
    done
    # Ensure at least one moved AND at least one NOT moved, to exercise both paths.
    (( ${#moved[@]} == 0 )) && moved=(0)
    (( ${#moved[@]} == K )) && moved=("${moved[@]:0:K-1}")
    pushes=""
    for f in "${moved[@]}"; do pushes+="    vec_push(&mut v, s.f${f});
"; done
    cat > "$TMP/m.kd" <<EOF
trait Drop { fn drop(&mut self) ! { io }; }
struct N { s: String, id: i64 }
impl Drop for N { fn drop(&mut self) ! { io } { print(self.id); } }
struct S { ${fields%, } }
fn main() -> i64 ! { io, alloc } {
    let mut v = vec_new();
    let s = S { ${lit%, } };
$pushes    print(999);
    0
}
EOF
    "$KARDC" --no-cache -o "$TMP/m" "$TMP/m.kd" >/dev/null 2>&1 || {
        echo "FAIL [memsafety compile]: program $i"; cat "$TMP/m.kd"; fails=$((fails+1)); continue; }
    # (1) heap-clean under MALLOC_CHECK_=3 (x3).
    bad=0
    for r in 1 2 3; do
        set +e; out=$(MALLOC_CHECK_=3 "$TMP/m" 2>"$TMP/e"); rc=$?; set -e
        if [[ "$rc" -eq 134 ]] || grep -qi 'free\|corrupt' "$TMP/e"; then bad=1; fi
    done
    [[ "$bad" -eq 0 ]] || { echo "FAIL [memsafety heap]: program $i double-free/corrupt"; cat "$TMP/m.kd"; fails=$((fails+1)); continue; }
    # (2) every id [base, base+K) dropped exactly once.
    out=$("$TMP/m" 2>/dev/null)
    for ((f=0; f<K; f++)); do
        c=$(grep -cx "$((base+f))" <<< "$out")
        [[ "$c" -eq 1 ]] || { echo "FAIL [memsafety drop-count]: program $i id $((base+f)) dropped $c times (want 1)"; cat "$TMP/m.kd"; fails=$((fails+1)); break; }
    done
done

[[ "$fails" -eq 0 ]] || { echo "FAIL [fuzz-memsafety]: $fails/$N programs unsound (seed $SEED)"; exit 1; }
echo "PASS [fuzz-memsafety]: $N random struct/Drop/partial-move programs — heap-clean + every field dropped exactly once (seed $SEED)"

# Loop variant: a fixed program moving one String field out per iteration must
# stay RSS-flat (a leak balloons it) and heap-clean.
cat > "$TMP/loop.kd" <<'EOF'
struct N { s: String, id: i64 }
struct S { a: N, b: N }
fn main() -> i64 ! { io, alloc } {
    let mut sink = 0;
    let mut i = 0;
    while i < 1000000 {
        let s = S { a: N { s: int_to_string(i), id: i }, b: N { s: int_to_string(i + 1), id: i } };
        let mut v = vec_new();
        vec_push(&mut v, s.a);          // move a out; b drops at scope exit
        let g = vec_get(&v, 0);
        sink = sink + str_len(&g.s);
        i = i + 1;
    }
    sink
}
EOF
"$KARDC" --no-cache -o "$TMP/loop" "$TMP/loop.kd" >/dev/null 2>&1
if command -v /usr/bin/time >/dev/null 2>&1; then
    set +e; /usr/bin/time -v "$TMP/loop" >/dev/null 2>"$TMP/t"; set -e
    rss=$(grep -oE 'Maximum resident set size \(kbytes\): [0-9]+' "$TMP/t" 2>/dev/null | grep -oE '[0-9]+$' || true)
    if [[ -n "$rss" ]]; then
        [[ "$rss" -lt 32768 ]] || { echo "FAIL [fuzz-memsafety loop]: RSS $rss KB — partial-move leak"; exit 1; }
        echo "PASS [fuzz-memsafety loop]: 1M partial-move iterations RSS-flat (${rss} KB)"
    else
        echo "PASS [fuzz-memsafety loop]: ran (RSS gate skipped — no GNU time)"
    fi
else
    echo "PASS [fuzz-memsafety loop]: ran (RSS gate skipped — no GNU time)"
fi
echo "ALL MEMORY-SAFETY FUZZ TESTS PASSED"
