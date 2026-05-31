# Roadmap

Where kardashev honestly stands, and the work that would move it forward.

kardashev is a **research / portfolio-grade** systems language: a real,
well-tested, ~32 K-LOC LLVM-backed compiler with a genuinely uncommon feature —
zero-runtime-cost, row-polymorphic **effect labels** in the type system — plus
ownership + NLL borrow checking, deterministic `Drop`/RAII, generics/traits, a
full numeric tower, async, and threads. It is well above the median hobby/student
compiler in breadth and test discipline. It is **not** a production language: it
is pre-ecosystem, pre-performance-proven, and MVP-shaped in places.

**Shipped: v1–v19** (Phases 0–114, through `v0.19.0`). The per-version themes are
in the [README roadmap table](README.md#roadmap); every phase's detail is in
[CHANGELOG.md](CHANGELOG.md). v15–v19 built a self-hosted *mini* compiler and a
differential-fuzzing test surface.

## The honest gaps

These separate kardashev from a production language. They are stated plainly so
the roadmap below can close them in priority order.

- **"Self-hosting" is a mini compiler, not a bootstrap.** `examples/selfhost/compile.kd`
  type-checks and runs a tiny 2-type (`i64`/`bool`) expression + function
  language, lowering it to an 8-opcode in-process stack VM. It emits no LLVM IR
  or native code and handles none of kardashev's own features (no
  structs/enums/traits/generics/borrow-check/effects/`Drop`). It proves the
  language can express a compiler-shaped program; it is **not** kardashev
  compiling kardashev.
- **No performance numbers.** There are zero benchmarks. "Zero-cost effects" is
  true but type-system-only; the compiler's actual codegen/runtime speed vs a C
  or Rust reference is simply unmeasured.
- **MVP / leaky stdlib.** `HashMap`/`HashSet` have no `remove` (deferred —
  open-addressing deletion needs tombstone-aware probing) and do **not** drop
  interior keys/values (a documented leak, no use-after-free); a completed async
  `Future`'s heap frame is never reclaimed (long-running async leaks frames);
  the const-eval scalar set and some library surfaces are still `i64`/`bool`-MVP.
- **A few real ergonomic gaps** (verified against the current compiler, *not* the
  stale docs): no `||` logical-or (it collides with closure `||` syntax); no `&`
  of a temporary/rvalue (`&A(10)` errors — bind to a `let` first), plus a related
  miscompile where a ref to an enum literal passes the wrong scalar. *(Note:
  `%`, `&&`, and enum-typed struct fields **do** work — the language reference is
  out of date and claims otherwise.)*
- **No ecosystem / single backend / thin platform story.** Local-path
  dependencies only (a third-party registry is deferred); LLVM is the only
  backend; CI covers Linux + macOS, where one arm64 JIT-teardown flake is papered
  over with retries rather than root-caused; no Windows/WASM; no normative spec;
  pre-1.0 with no stability policy.

## Planned

> Planned, not done. Each roadmap follows the established cadence: implement each
> phase green (JIT **and** AOT) → adversarial review → fix findings →
> consolidating PR → tag/release.

### v20 — toward a real bootstrap (the north star)

Move the self-hosted compiler from a toy toward real kardashev. Full
kardashev-compiles-kardashev is several roadmaps out; v20 is the first concrete
step past "toy":

- Extend the source language the in-kardashev compiler accepts past `i64`/`bool`
  — at least **structs and enums** (the shapes the real compiler is built from).
- Emit a **real artifact** instead of running an in-process stack VM — e.g. a
  textual LLVM-IR (or the host's bytecode), so the self-hosted compiler's output
  is something you can compile/run, not just interpret.
- A **differential gate**: the self-hosted compiler's result must match the host
  compiler's on the shared subset, fuzzed like the existing JIT-vs-AOT checks.

### v21 — prove it, and close the leaks

Turn anecdotes into numbers and fix the documented soundness/footprint gaps:

- A **benchmark suite**: compile-time + runtime of the capstones (`json`,
  `wordfreq`, `matrix`, …) vs a C/Rust reference, with **committed numbers** —
  replacing the "-O2 default" / "flat RSS" anecdotes with reproducible data.
- Fix the **documented leaks**: drop `HashMap`/`HashSet` interior keys/values;
  add `remove` via tombstone-aware probing; reclaim completed async `Future`
  frames in the executor.
- Generalize the remaining `i64`/`bool`-MVP surfaces (library element types,
  const-eval scalars) toward arbitrary types.

### v22 — ergonomics, docs, and platform

- **`||` logical-or** (resolve the closure-syntax collision); **`&<temporary>`**
  (materialize an rvalue into a statement-scoped, dropped slot) and the related
  `&A(10)` ref-to-enum-literal miscompile.
- **Reconcile the docs with reality** — the language reference still claims `%`,
  `&&`, and enum-typed struct fields are unsupported when they work; bring the
  reference in line with the implementation and the test suite.
- The macOS `codegen_test` arm64 JIT-teardown **flake**: root-cause it (needs a
  macOS-arm64 environment) or raise `--flaky_test_attempts` to cut the residual.
- Explore a **second platform or backend** (Windows, or a WASM/C portability
  backend) to break the LLVM/Linux-leaning monoculture.

## Deferred (honest — documented, not stubbed)

- **Third-party package registry** (resolution via the Bazel module registry) —
  Bazel can't run in this build environment, so a real registry integration
  isn't verifiable here; only `mod foo;` + `kard.toml` local-path deps ship.
- **A normative language spec + conformance suite**, and a **stability / semver
  policy** — appropriate once the language stops changing core semantics (it is
  still pre-1.0).
