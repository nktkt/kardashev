#!/usr/bin/env bash
# Phase 13b smoke test: growable containers + combinators, end to end
# through JIT and AOT.
#
#   1. Growable String: build "ab"+"cd" incrementally via string_push_str;
#      print "abcd" and observe len == 4.
#   2. HashMap<i64,i64> (open addressing): insert (3->30),(7->70); get(7) is
#      Some(70), get(99) is None, len == 2; overwriting an existing key
#      replaces the value; inserting 50 keys forces several rehashes and all
#      remain retrievable.
#   3. Option / Result combinators (prelude fns): option_map / option_unwrap_or
#      / option_and_then, result_map / result_unwrap_or — including an effect-
#      composition case (an `io` closure inside option_map makes the call io)
#      and the negative case (the same io closure from a pure context is
#      rejected).
#   4. Slices `&[T]`: take `&v[a..b]`, read via slice_len / slice_get, sum.
set -euo pipefail

KARDC=""
for candidate in \
    "${TEST_SRCDIR:-}/_main/compiler/kardc" \
    "${TEST_SRCDIR:-}/kardashev/compiler/kardc" \
    "${RUNFILES_DIR:-}/_main/compiler/kardc" \
    "${RUNFILES_DIR:-}/kardashev/compiler/kardc" \
    "./compiler/kardc"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        KARDC="$candidate"
        break
    fi
done

if [[ -z "$KARDC" ]]; then
    echo "FAIL: kardc binary not found in runfiles"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Helper: run a program through JIT and assert its full stdout. Args:
#   $1 = label, $2 = source path, $3 = expected stdout.
jit_expect() {
    local label="$1" src="$2" want="$3"
    local got
    got=$("$KARDC" "$src")
    if [[ "$got" != "$want" ]]; then
        echo "FAIL [$label]: JIT output mismatch"
        echo "expected:"; echo "$want"
        echo "got:"; echo "$got"
        exit 1
    fi
}

# Helper: AOT-compile $2, run it, assert stdout ($3) and exit code ($4).
aot_expect() {
    local label="$1" src="$2" want_out="$3" want_rc="$4"
    "$KARDC" -o "$TMP/prog" "$src"
    set +e
    local got_out
    got_out=$("$TMP/prog")
    "$TMP/prog" > /dev/null
    local rc=$?
    set -e
    if [[ "$got_out" != "$want_out" ]]; then
        echo "FAIL [$label]: AOT stdout mismatch"
        echo "expected:"; echo "$want_out"
        echo "got:"; echo "$got_out"
        exit 1
    fi
    if [[ "$rc" -ne "$want_rc" ]]; then
        echo "FAIL [$label]: AOT exit code was $rc (expected $want_rc)"
        exit 1
    fi
}

# ===========================================================================
# 1. Growable String
# ===========================================================================
cat > "$TMP/string.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let s = string_new();
    string_push_str(&mut s, "ab");
    string_push_str(&mut s, "cd");
    print_string(&s);     // abcd
    string_len(&s)        // exit code / printed: 4
}
EOF
jit_expect "string" "$TMP/string.kd" $'abcd\n4'
aot_expect "string" "$TMP/string.kd" "abcd" 4
echo "PASS [string]: build \"ab\"+\"cd\" -> abcd, len 4 (JIT + AOT)"

# ===========================================================================
# 2. HashMap<i64,i64>: basic ops + overwrite
# ===========================================================================
cat > "$TMP/hashmap.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let m = hashmap_new();
    hashmap_insert(&mut m, 3, 30);
    hashmap_insert(&mut m, 7, 70);
    match hashmap_get(&m, 7)  { Some(v) => print(v), None => print(0 - 1) }   // 70
    match hashmap_get(&m, 99) { Some(v) => print(v), None => print(0 - 1) }   // -1 (None)
    hashmap_insert(&mut m, 3, 33);                                            // overwrite
    match hashmap_get(&m, 3)  { Some(v) => print(v), None => print(0 - 1) }   // 33
    print(hashmap_len(&m));                                                   // 2
    hashmap_len(&m)
}
EOF
# JIT prints the four print() lines, then kardc prints main's return (2).
jit_expect "hashmap_basic" "$TMP/hashmap.kd" $'70\n-1\n33\n2\n2'
aot_expect "hashmap_basic" "$TMP/hashmap.kd" $'70\n-1\n33\n2' 2
echo "PASS [hashmap_basic]: get(7)=Some(70), get(99)=None, overwrite 3->33, len 2 (JIT + AOT)"

# HashMap rehash: insert 50 keys (initial cap 8 -> several rehashes), confirm
# all retrievable (bad == 0).
cat > "$TMP/hashmap_rehash.kd" <<'EOF'
fn main() -> i64 ! { alloc, io } {
    let m = hashmap_new();
    let mut i = 0;
    while i < 50 { hashmap_insert(&mut m, i, i * 10); i = i + 1; }
    print(hashmap_len(&m));   // 50
    let mut bad = 0;
    let mut j = 0;
    while j < 50 {
        let got = match hashmap_get(&m, j) { Some(v) => v, None => 0 - 1 };
        let delta = if got == j * 10 { 0 } else { 1 };
        bad = bad + delta;
        j = j + 1;
    }
    print(bad);               // 0
    bad
}
EOF
# JIT: prints 50 and 0, then kardc prints main's return (bad == 0).
jit_expect "hashmap_rehash" "$TMP/hashmap_rehash.kd" $'50\n0\n0'
aot_expect "hashmap_rehash" "$TMP/hashmap_rehash.kd" $'50\n0' 0
echo "PASS [hashmap_rehash]: 50 keys force rehash, all retrievable, len 50 (JIT + AOT)"

# ===========================================================================
# 3. Option / Result combinators (from the prelude)
# ===========================================================================
cat > "$TMP/combinators.kd" <<'EOF'
fn main() -> i64 ! { io } {
    print(option_unwrap_or(option_map(Some(5), |x| x * 2), 0));        // 10
    print(option_unwrap_or(None, 9));                                  // 9
    print(option_unwrap_or(option_and_then(Some(5), |x| Some(x + 1)), 0)); // 6
    print(result_unwrap_or(result_map(Ok(5), |x| x + 1), 0));          // 6
    print(result_unwrap_or(Err(42), 7));                               // 7
    // effect composition: io closure inside option_map makes the call io
    let oc = option_map(Some(5), |x| print(x + 100));                  // prints 105
    print(option_unwrap_or(oc, 0));                                    // 0
    0
}
EOF
# JIT: 7 print() lines, then kardc prints main's return (0).
jit_expect "combinators" "$TMP/combinators.kd" $'10\n9\n6\n6\n7\n105\n0\n0'
aot_expect "combinators" "$TMP/combinators.kd" $'10\n9\n6\n6\n7\n105\n0' 0
echo "PASS [combinators]: option/result map/unwrap_or/and_then + io effect composition (JIT + AOT)"

# Negative: an io closure threaded through option_map from a PURE context is a
# typecheck error mentioning the io effect.
cat > "$TMP/combinators_bad.kd" <<'EOF'
fn main() -> i64 {
    option_unwrap_or(option_map(Some(5), |x| print(x)), 0)
}
EOF
set +e
ERR_OUT=$("$KARDC" "$TMP/combinators_bad.kd" 2>&1)
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
    echo "FAIL [combinators_neg]: io closure via option_map from pure main should be rejected"
    exit 1
fi
if ! grep -q 'effect `io`' <<< "$ERR_OUT"; then
    echo "FAIL [combinators_neg]: expected an io-effect diagnostic, got:"
    echo "$ERR_OUT"
    exit 1
fi
echo "PASS [combinators_neg]: io closure via option_map from pure context is rejected"

# ===========================================================================
# 4. Slices &[T]
# ===========================================================================
cat > "$TMP/slice.kd" <<'EOF'
fn sum_slice(s: &[i64], i: i64) -> i64 {
    if i < slice_len(s) { slice_get(s, i) + sum_slice(s, i + 1) }
    else { 0 }
}
fn main() -> i64 ! { alloc, io } {
    let v = vec_new();
    vec_push(&mut v, 10);
    vec_push(&mut v, 20);
    vec_push(&mut v, 30);
    vec_push(&mut v, 40);
    vec_push(&mut v, 50);
    let s = &v[1..4];           // {20, 30, 40}
    print(slice_len(s));        // 3
    print(slice_get(s, 0));     // 20
    print(slice_get(s, 2));     // 40
    sum_slice(s, 0)             // 90
}
EOF
jit_expect "slice" "$TMP/slice.kd" $'3\n20\n40\n90'
aot_expect "slice" "$TMP/slice.kd" $'3\n20\n40' 90
echo "PASS [slice]: &v[1..4] -> len 3, get(0)=20, get(2)=40, sum 90 (JIT + AOT)"

echo "PASS: growable String + HashMap + Option/Result combinators + slices work in JIT + AOT"
