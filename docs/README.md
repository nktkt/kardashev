# The kardashev Documentation

**kardashev** is a Rust-flavored systems programming language whose signature
feature is **lightweight effect labels in the type system**: every function
declares which side effects it can produce (`io`, `alloc`, `panic`, `async`,
`unwind`) as part of its signature, and the compiler tracks them across call
chains — pure type-system information, with zero runtime cost. It compiles to
native code through LLVM (an ORC JIT for the REPL and an AOT path for
executables), with Rust-style ownership + non-lexical-lifetime borrow checking.

This site is the reference documentation. It is built from the Markdown in the
[`docs/`](https://github.com/kardashevlang/kardashev/tree/main/docs) directory
with [mdBook](https://rust-lang.github.io/mdBook/) and lives next to the
compiler, so each language change updates its docs in the same pull request.

## Contents

- **[Language Reference](language-reference.md)** — the surface syntax and
  semantics: types, generics, traits + `impl`, ADTs + pattern matching,
  ownership, `Result` + `?`, and the effect-row notation.
- **[Effects System](effects.md)** — the signature feature in depth: effect
  labels, effect-row polymorphism, and how effects propagate across calls.
- **[Standard Library](stdlib.md)** — the built-in prelude: `Option` / `Result`,
  `Vec` / `String` / `HashMap` / `HashSet` / `Box`, the trait + `#[derive]`
  machinery, combinators, and string utilities.
- **[Compiler Architecture](architecture.md)** — the pipeline from lexer through
  type/borrow/effect checking to LLVM codegen, monomorphization, and the
  JIT/AOT backends.

For the project roadmap, build instructions, and source, see the
[repository README](https://github.com/kardashevlang/kardashev).
