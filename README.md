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

Phases 0–5 (AOT + prelude) and Phase 7.1 (`mod foo;` flat import) land;
row-polymorphic effects (`! {e}`) and stdlib types that need a heap
(`Vec`, `String`) wait for first-class function values + a small runtime
in Phase 6. Built locally with `bazel build //... && bazel test //...`
or, when Bazel isn't available, the `Makefile.local` shim (LLVM +
clang). The CI matrix runs both ubuntu-latest and macos-latest via
Bazel on every push.

What works today:

```rust
// Generics + traits + bounded params (Phase 3)
trait Show { fn show(self) -> i64; }
struct Point { x: i64, y: i64 }
impl Show for Point { fn show(self) -> i64 { self.x + self.y } }
fn use_show<T: Show>(t: T) -> i64 { t.show() }

// Result + ? operator (Phase 3.4)
enum Result<T, E> { Ok(T), Err(E) }
fn double(n: i64) -> Result<i64, i64> {
    let x = parse(n)?;        // early-returns Err if parse fails
    Ok(x + x)
}

// References + NLL borrow check (Phase 2.4)
fn read(p: &Point) -> i64 { p.x + p.y }
fn main() -> i64 {
    let p = Point { x: 3, y: 4 };
    let r = &p;
    let a = read(r);          // r's last use here; borrow is now dead
    let b = consume(p);       // OK to move — NLL allows it
    a + b
}

// Effect labels (Phase 4) — pure by default; explicit effects propagate
fn raw_read() -> i64 ! { io } { 42 }
fn main() -> i64 ! { io, alloc } { raw_read() }    // pure-caller would error
```

`Option<T>` and `Result<T, E>` are auto-included via a built-in prelude
so user programs can use `Some` / `None` / `Ok` / `Err` without
redeclaring them.

Multi-file programs (Phase 7.1): write `mod foo;` at the top of a `.kd`
file to pull in `foo.kd` from the same directory. Modules are inlined
flat (no path syntax yet) and resolution is recursive and cycle-safe.

```
// util.kd
fn double(n: i64) -> i64 { n + n }

// main.kd
mod util;
fn main() -> i64 { double(21) }
```

Three driver modes:

```
kardc                      # interactive REPL (JIT each expression)
kardc <file.kd>            # JIT-run main() and print result
kardc -o <out> <file.kd>   # AOT-compile to a native executable
```

The AOT path emits a native object via LLVM's `TargetMachine`, generates
a C-compatible `int main()` wrapper that returns the kardashev
`fn main() -> i64` result truncated to an exit code, and shells out to
`clang` for linking. Programs compile through lexer → parser → HM
typechecker → NLL borrow-checker → effect inference → LLVM IR → ORC v2
JIT (or AOT).

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Scaffold: Bazel + LLVM toolchain + CI + a JIT binary returning `42` | ✅ |
| 1 | MVP: JIT REPL running `fib` (lexer + parser + monotype HM + LLVM IR + ORC JIT) | ✅ |
| 2 | Ownership + NLL borrow check + structs + enums + pattern matching | ✅ |
| 3 | Traits + generics + `Result` + `?` operator + monomorphization | ✅ |
| 4 | Effect labels in signatures (the signature feature lands here) | ✅ (concrete labels; row-polymorphic `! {e}` waits for fn-pointer values in Phase 6) |
| 5 | AOT pipeline + minimal stdlib (`Option`, `Result`, `Vec`, `String`) | ✅ AOT + Option/Result prelude; Vec/String wait for the Phase 6 runtime |
| 6 | `async` / `await` + state-machine transform + basic executor | — |
| 7 | Module system + complete `rules_kardashev` + `kard` CLI | 🟡 `mod foo;` flat import lands (recursive, cycle-safe); `rules_kardashev` Starlark macros + a `kard` driver thin-wrapping kardc still to come |
| 8 | Optimization passes + LSP + docs site | — |

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.
