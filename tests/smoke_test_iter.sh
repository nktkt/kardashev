#!/usr/bin/env bash
# Phase 13a smoke test: method-receiver autoref + a real Iterator trait +
# iterator adaptors, end to end through JIT and AOT.
#
#   - THE foundation fix: a `&mut self` method mutates the receiver in place
#     across repeated calls (`c.inc(); c.inc(); c.inc()` == 3) — previously
#     the 2nd call reported `c` as moved.
#   - `for x in 1..=10` still sums to 55 (Phase 9 range fast path kept).
#   - `for` over a NON-Range Iterator impl (Countdown) visits the right
#     elements (routes through the new Iterator::next desugar).
#   - `fold(it, init, |acc, x| ...)` drives next() with a closure (eager).
#   - eager `map`/`filter` collect into a Vec via real closures.
#   - effect composition: a fold with an `io` closure is `io` at the call
#     site (positive accepted, negative rejected).
#
# The prelude supplies `trait Iterator` + `impl Iterator for Range`, so the
# range programs below rely on it implicitly.
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
    echo "FAIL: kardc binary not found"
    exit 1
fi

echo "Using kardc at: $KARDC"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Run a program through both JIT (stdout, last line) and AOT (exit code) and
# assert both yield the expected i64. `main` returns the value.
run_case() {
    local name="$1" src="$2" want="$3"
    printf '%s' "$src" > "$TMP/$name.kd"

    local jit
    jit=$("$KARDC" "$TMP/$name.kd" | tail -n 1)
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL [$name]: JIT printed '$jit', expected '$want'"
        exit 1
    fi

    "$KARDC" -o "$TMP/$name" "$TMP/$name.kd"
    set +e
    "$TMP/$name" > /dev/null
    local code=$?
    set -e
    if [[ "$code" != "$want" ]]; then
        echo "FAIL [$name]: AOT exit code $code, expected $want"
        exit 1
    fi
    echo "PASS [$name]: JIT + AOT == $want"
}

# 1. THE foundation fix: &mut self persists across calls -> 3.
run_case mut_self_counter '
trait Inc { fn inc(&mut self) -> i64; }
struct Counter { n: i64 }
impl Inc for Counter { fn inc(&mut self) -> i64 { self.n = self.n + 1; self.n } }
fn main() -> i64 ! { alloc } {
    let mut c = Counter { n: 0 };
    c.inc(); c.inc(); c.inc()
}' 3

# 2. Phase 9 regression: for over an inclusive range still sums to 55.
run_case for_inclusive_range '
fn main() -> i64 ! { alloc } {
    let mut s = 0;
    for x in 1..=10 { s = s + x; }
    s
}' 55

# 3. for over a NON-Range Iterator impl (Countdown): 4+3+2+1 == 10.
run_case for_countdown '
trait Iterator { fn next(&mut self) -> Option<i64>; }
struct Countdown { n: i64 }
impl Iterator for Countdown {
    fn next(&mut self) -> Option<i64> {
        if self.n <= 0 { None } else { self.n = self.n - 1; Some(self.n + 1) }
    }
}
fn main() -> i64 ! { alloc } {
    let cd = Countdown { n: 4 };
    let mut s = 0;
    for x in cd { s = s + x; }
    s
}' 10

# 4. fold driven by a closure over an inclusive range: 1+2+3+4+5 == 15.
run_case fold_closure '
fn fold<I: Iterator, e>(it: I, init: i64, f: fn(i64, i64) -> i64 ! {e}) -> i64 ! { alloc, e } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
fn main() -> i64 ! { alloc } {
    fold(1..=5, 0, |acc, x| acc + x)
}' 15

# 5. eager map (double) + filter (evens) collecting into a Vec, then sum.
#    map: [1..=5]*2 -> [2,4,6,8,10] sum 30; filter evens in 1..=10 -> 30.
run_case map_filter '
fn map<I: Iterator>(it: I, f: fn(i64) -> i64) -> Vec<i64> ! { alloc } {
    let mut iter = it;
    let out = vec_new();
    loop {
        match iter.next() {
            Some(x) => { vec_push(&mut out, f(x)); },
            None => { break; },
        }
    }
    out
}
fn filter<I: Iterator>(it: I, pred: fn(i64) -> i64) -> Vec<i64> ! { alloc } {
    let mut iter = it;
    let out = vec_new();
    loop {
        match iter.next() {
            Some(x) => {
                match pred(x) {
                    0 => {},
                    _ => { vec_push(&mut out, x); },
                }
            },
            None => { break; },
        }
    }
    out
}
fn sum_vec(v: &Vec<i64>, i: i64) -> i64 {
    if i < vec_len(v) { vec_get(v, i) + sum_vec(v, i + 1) } else { 0 }
}
fn is_even(x: i64) -> i64 { if x - (x / 2) * 2 == 0 { 1 } else { 0 } }
fn main() -> i64 ! { alloc } {
    let doubled = map(1..=5, |x| x * 2);
    let ds = sum_vec(&doubled, 0);
    let evens = filter(1..=10, |x| is_even(x));
    let es = sum_vec(&evens, 0);
    ds + es
}' 60

# 6. Effect composition POSITIVE: a fold with an io closure typechecks in an
#    io fn. Prints 1,2,3 then returns 6.
cat > "$TMP/fold_io.kd" <<'EOF'
fn fold<I: Iterator, e>(it: I, init: i64, f: fn(i64, i64) -> i64 ! {e}) -> i64 ! { alloc, e } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
fn main() -> i64 ! { io, alloc } {
    fold(1..=3, 0, |acc, x| { print(x); acc + x })
}
EOF
JIT_IO=$("$KARDC" "$TMP/fold_io.kd")
EXPECTED_IO=$'1\n2\n3\n6'
if [[ "$JIT_IO" != "$EXPECTED_IO" ]]; then
    echo "FAIL [fold_io positive]: JIT output mismatch"
    echo "expected:"; echo "$EXPECTED_IO"
    echo "got:"; echo "$JIT_IO"
    exit 1
fi
"$KARDC" -o "$TMP/fold_io" "$TMP/fold_io.kd"
set +e
AOT_IO=$("$TMP/fold_io")
"$TMP/fold_io" > /dev/null
IO_RC=$?
set -e
if [[ "$AOT_IO" != $'1\n2\n3' || "$IO_RC" -ne 6 ]]; then
    echo "FAIL [fold_io positive]: AOT stdout='$AOT_IO' exit=$IO_RC (want 1,2,3 / 6)"
    exit 1
fi
echo "PASS [fold_io_positive]: io closure in io fold prints 1,2,3 -> 6 (JIT + AOT)"

# 7. Effect composition NEGATIVE: the same fold + io closure in a PURE fn is
#    rejected (the io effect leaks to the undeclared call site).
cat > "$TMP/fold_io_neg.kd" <<'EOF'
fn fold<I: Iterator, e>(it: I, init: i64, f: fn(i64, i64) -> i64 ! {e}) -> i64 ! { alloc, e } {
    let mut iter = it;
    let mut acc = init;
    loop {
        match iter.next() {
            Some(x) => { acc = f(acc, x); },
            None => { break; },
        }
    }
    acc
}
fn main() -> i64 ! { alloc } {
    fold(1..=3, 0, |acc, x| { print(x); acc + x })
}
EOF
set +e
NEG_OUT=$("$KARDC" "$TMP/fold_io_neg.kd" 2>&1)
NEG_RC=$?
set -e
if [[ "$NEG_RC" -eq 0 ]]; then
    echo "FAIL [fold_io negative]: a pure fn leaking io should be rejected"
    exit 1
fi
if ! grep -q 'io' <<< "$NEG_OUT"; then
    echo "FAIL [fold_io negative]: expected an io-effect diagnostic, got:"
    echo "$NEG_OUT"
    exit 1
fi
echo "PASS [fold_io_negative]: pure fn calling an io-closure fold is rejected"

echo "PASS: method-receiver autoref + Iterator trait + adaptors work in JIT + AOT (mut-self persists; for over Range + custom Iterator; fold/map/filter with closures; effects compose)"
