# Benchmarks

Reproducible runtime numbers for kardashev's AOT output vs an equivalent C
reference — turning "performance unmeasured" (the honest gap noted in
[ROADMAP.md](ROADMAP.md)) into actual data.

**Method.** Each workload is written identically in kardashev and C. The
kardashev program is AOT-compiled with `kardc -O2` (LLVM, linked via `clang`);
the C reference with `clang -O2`. Both are run best-of-3 (lowest wall-clock), and
their outputs are checked to be identical. Reproduce with:

```sh
make -f Makefile.local kardc      # or: bazel build //compiler:kardc
bench/run.sh
```

## Results

Measured on this dev box (x86-64, LLVM 21, `clang -O2` for both back-ends).
Absolute times vary by machine; the **ratio to C** is the portable figure.

| Workload  | What it stresses                    | kardashev | C (clang -O2) | ratio |
|-----------|-------------------------------------|-----------|---------------|-------|
| `fib`     | recursion + function-call overhead  | ~0.23 s   | ~0.23 s       | **1.00×** |
| `collatz` | branches + signed `/` `%`           | ~0.36 s   | ~0.38 s       | **~1.0×** |
| `loop`    | a tight integer-arithmetic loop     | ~0.11 s   | ~0.05 s       | **~2.2×** |

`fib(40)`, the Collatz step-count over `1..3,000,000`, and a 200 M-iteration
arithmetic loop. All three produce the same result as the C reference.

## Reading these honestly

- kardashev's AOT codegen is **C-competitive** on call-heavy and branch-heavy
  code (`fib`, `collatz` ≈ 1.0×) — unsurprising, since it shares LLVM's `-O2`
  pipeline with `clang`.
- The **~2.2× gap on the tight `loop`** is real and the most interesting figure:
  the simplest counted integer loop is where kardashev currently trails C the
  most (the front-end's alloca-heavy lowering of `let mut` counters + the signed
  division leave the loop less optimized than clang's). This is a concrete
  codegen-optimization target, not a fundamental limit.
- These are **micro-benchmarks**, not a representative application workload, and
  they exercise only the scalar/integer path (no allocation, GC-free by design,
  no I/O). They establish that the compiler emits real, reasonable native code —
  they do **not** claim kardashev is "as fast as C" in general.
