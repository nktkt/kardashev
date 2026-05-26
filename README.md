# kardashev

A systems programming language with lightweight effect-label typing, built on LLVM.

## What it is

kardashev is a Rust-flavored systems language whose signature feature is **lightweight effect labels in the type system**: every function declares which side-effects it can produce (`io`, `alloc`, `panic`, `async`, ...) as part of its signature, and the compiler tracks them across call chains. Unlike Koka, there are no handlers or continuations — effects are pure type-system information, with zero runtime cost.

```rust
fn add(a: i64, b: i64) -> i64 { a + b }                       // pure

fn read_cfg(path: &str) -> Result<Config> ! { io, alloc } {   // effects in signature
    let s = std::fs::read_to_string(path)?;
    parse(s)
}

fn map<T, U, e>(xs: Vec<T>, f: fn(T) -> U ! {e}) -> Vec<U> ! { e, alloc } {
    let mut out = Vec::with_capacity(xs.len());
    for x in xs { out.push(f(x)); }
    out
}
```

The `! { ... }` syntax after the return type is the effect row. `e` is a row variable making the function effect-polymorphic — `map` is pure when `f` is pure, and propagates whatever effects `f` introduces.

## Design

- **Memory model**: ownership + borrowing (Rust-style affine, non-lexical lifetimes)
- **Type system**: HM-based with generics, ADTs, traits + `impl`, monomorphization
- **Errors**: `Result<T, E>` + `?` operator
- **Concurrency**: `async` / `await` lightweight tasks (stackless state-machine transform)
- **Surface syntax**: Rust/Go-style — `{}`, `fn`, `->`, `match`, `let`
- **Effect labels**: row-polymorphic effect sets, no handlers, compile-time only
- **Backend**: LLVM (AOT to native binary + ORC JIT for the REPL)
- **Build system**: Bazel + `rules_kardashev` (Starlark) + thin `kard` CLI wrapper
- **Source extension**: `.kd`

### Built-in effect labels (v1)

| Label    | Meaning                                                            |
|----------|--------------------------------------------------------------------|
| `pure`   | No effects (empty row; the default if `! { ... }` is omitted)      |
| `alloc`  | Heap allocation                                                    |
| `io`     | File / network / stdio / general syscalls                          |
| `panic`  | Unrecoverable failure                                              |
| `async`  | Yields to the scheduler                                            |
| `unwind` | Stack unwinding for cancellation (distinct from `panic`)           |

Effect sets are unioned across the call graph and checked at definition sites; no runtime cost.

## Status

Design phase. No code yet. Development follows a phase-by-phase issue-tracked roadmap.

## Roadmap

| Phase | Goal |
|-------|------|
| 0 | Scaffold: Bazel + LLVM toolchain + CI + a JIT binary returning `42` |
| 1 | MVP: JIT REPL running `fib` (lexer + parser + monotype HM + LLVM IR + ORC JIT) |
| 2 | Ownership + NLL borrow check + structs + enums + pattern matching |
| 3 | Traits + generics + `Result` + `?` operator + monomorphization |
| 4 | Effect labels in signatures (the signature feature lands here) |
| 5 | AOT pipeline + minimal stdlib (`Option`, `Result`, `Vec`, `String`) |
| 6 | `async` / `await` + state-machine transform + basic executor |
| 7 | Module system + complete `rules_kardashev` + `kard` CLI |
| 8 | Optimization passes + LSP + docs site |

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.
