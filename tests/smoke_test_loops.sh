#!/usr/bin/env bash
# Phase 9 smoke test: loop constructs (while / loop+break / continue / for
# over integer ranges) run identically through JIT and AOT. Each program
# computes a known value so we can assert on the exit code / stdout.
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

# Run a program through both JIT (stdout) and AOT (exit code) and assert
# both yield the expected i64. `main` returns the value; JIT prints it,
# AOT uses it as the process exit code.
run_case() {
    local name="$1" src="$2" want="$3"
    printf '%s' "$src" > "$TMP/$name.kd"

    local jit
    jit=$("$KARDC" "$TMP/$name.kd")
    if [[ "$jit" != "$want" ]]; then
        echo "FAIL [$name]: JIT printed '$jit', expected '$want'"
        exit 1
    fi

    "$KARDC" -o "$TMP/$name" "$TMP/$name.kd"
    set +e
    "$TMP/$name"
    local code=$?
    set -e
    if [[ "$code" != "$want" ]]; then
        echo "FAIL [$name]: AOT exit code $code, expected $want"
        exit 1
    fi
    echo "PASS [$name]: JIT + AOT == $want"
}

# 1. while: sum 1+2+...+10 == 55 using a mutable accumulator.
run_case while_sum '
fn main() -> i64 {
    let mut sum = 0;
    let mut i = 1;
    while i <= 10 { sum = sum + i; i = i + 1; }
    sum
}
' 55

# 2. loop + break value: `let x = loop { break 42; }; x` == 42.
run_case loop_break_value '
fn main() -> i64 {
    let x = loop { break 42; };
    x
}
' 42

# 3. continue: skip even numbers, sum odds in 1..=10 == 25.
run_case continue_odds '
fn main() -> i64 {
    let mut sum = 0;
    let mut i = 0;
    loop {
        i = i + 1;
        if i > 10 { break; } else {}
        if i / 2 * 2 == i { continue; } else {}
        sum = sum + i;
    }
    sum
}
' 25

# 4. for over an inclusive range: sum 1..=10 == 55.
run_case for_inclusive '
fn main() -> i64 {
    let mut sum = 0;
    for x in 1..=10 { sum = sum + x; }
    sum
}
' 55

# 5. nested loops with break/continue targeting the innermost loop.
#    Inner loop yields 2 increments per outer pass (j=1,3 counted, j==2
#    skipped, j>3 breaks); 3 outer passes => 6.
run_case nested_loops '
fn main() -> i64 {
    let mut total = 0;
    let mut i = 0;
    while i < 3 {
        let mut j = 0;
        loop {
            j = j + 1;
            if j > 3 { break; } else {}
            if j == 2 { continue; } else {}
            total = total + 1;
        }
        i = i + 1;
    }
    total
}
' 6

echo "PASS: while / loop+break / continue / for ranges work in JIT + AOT"
