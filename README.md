# kardashev

A systems programming language with lightweight effect-label typing, built on LLVM.

**[📖 Documentation](https://kardashevlang.github.io/kardashev/)** · [Language Reference](https://kardashevlang.github.io/kardashev/language-reference.html) · [Effects](https://kardashevlang.github.io/kardashev/effects.html) · [Stdlib](https://kardashevlang.github.io/kardashev/stdlib.html) · [Architecture](https://kardashevlang.github.io/kardashev/architecture.html) — Licensed [MIT](LICENSE-MIT) OR [Apache-2.0](LICENSE-APACHE)

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
- **Concurrency**: `async` / `await` lightweight tasks + OS threads with a checked `Send` / `share` rule
- **Memory management**: deterministic `Drop` / RAII (constant-memory loops)
- **Effect labels**: row-polymorphic effect sets, no handlers, compile-time only
- **Backend**: LLVM (AOT to a native binary + ORC JIT for the REPL)
- **Build**: Bazel + `rules_kardashev`, or a `Makefile.local` LLVM/clang shim
- **Source extension**: `.kd`

### Built-in effect labels

| Label    | Meaning                                                       |
|----------|---------------------------------------------------------------|
| `pure`   | No effects (empty row; the default if `! { ... }` is omitted) |
| `alloc`  | Heap allocation                                               |
| `io`     | File / network / stdio / general syscalls                     |
| `panic`  | Unrecoverable failure                                         |
| `async`  | Yields to the scheduler                                       |
| `unwind` | Stack unwinding for cancellation (distinct from `panic`)      |
| `share`  | Crosses a thread boundary (gates the `Send` rule)             |

Effect sets are unioned across the call graph and checked at definition sites; no runtime cost.

## A taste

```rust
// Generics + traits + borrowing + effects
trait Show { fn show(self) -> i64; }
struct Point { x: i64, y: i64 }
impl Show for Point { fn show(self) -> i64 { self.x + self.y } }

fn read(p: &Point) -> i64 { p.x + p.y }     // borrow; NLL lets you move after its last use

fn raw_read() -> i64 ! { io } { 42 }
fn main() -> i64 ! { io } { raw_read() }     // a pure-declared caller would be rejected
```

```rust
// async / await
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }
fn main() -> i64 ! { async, io } { print(double(21).await); 0 }   // 42
```

`Option` / `Result` ship via a built-in prelude; `Vec<T>`, growable `String`, and `HashMap<K, V>` are built-in containers. Multi-file programs use `mod foo;` (resolves `foo.kd` siblings); a `kard.toml` manifest with local-path dependencies drives `kard build` / `kard run`. More in the [examples](examples/) and the [docs site](https://kardashevlang.github.io/kardashev/).

## Using it

```
kardc <file.kd>              # JIT-run main() and print its result
kardc -o <out> <file.kd>     # AOT-compile to a native executable
kardc --test <file.kd>       # run every `test_*() -> i64` fn (0 = pass)
kardc -O0|-O1|-O2|-O3 ...     # optimization level (default -O2)
kardc                        # interactive REPL (JIT each expression)
kard-lsp                     # Language Server (diagnostics, hover, completion, rename, …)
kard build | kard run        # build/run a kard.toml project
```

Build with Bazel (`bazel build //... && bazel test //...`) on ubuntu or macOS, or — when Bazel isn't available — the `Makefile.local` LLVM/clang shim. Programs compile through lexer → parser → HM typechecker → NLL borrow-checker → effect inference → LLVM IR → ORC JIT (or an AOT native object linked with `clang`). `kardc -o` uses a content-addressed incremental AOT compile cache (under `${XDG_CACHE_HOME:-~/.cache}/kardashev`, keyed on the resolved source + flags); pass `--no-cache` to bypass it.

## Status

Twenty roadmaps (**v1–v20**, Phases 0–119) have shipped and are merged to `main`, each green on a cleared clean build — 6 unit suites plus the full smoke / fuzz aggregate, **JIT and AOT**, on ubuntu + macOS CI. Current release: **[v0.20.0](https://github.com/kardashevlang/kardashev/releases/latest)**.

The north-star arc is **self-hosting**: v15–v17 build a complete compiler *in* kardashev — lexer → parser → type checker → code generator + VM, with `examples/selfhost/compile.kd` type-checking a whole function and then compiling + running its body. Dogfooding it found and fixed three real host-compiler bugs. v18–v19 added a differential fuzzer (random programs, `JIT == AOT == reference`) across the arithmetic, control-flow, memory-safety, and integer codegen paths.

> The per-phase history and every release's details live in **[CHANGELOG.md](CHANGELOG.md)**.

## Roadmap

| Version | Theme |
|---------|-------|
| v1  | MVP: the full pipeline (lexer → HM types → LLVM JIT/AOT), ownership + NLL borrow check, ADTs, traits/generics, `Result`/`?`, **effect labels**, `async`/`await`, modules, LSP |
| v2  | Iteration, closures + effect-carrying fn types, `dyn Trait` dispatch, a growable stdlib, `kardfmt` |
| v3  | `Drop` / RAII (constant memory), panic + unwinding, OS threads + `Mutex`, opt-levels + `--test` |
| v4  | Generic trait params + associated types + `where`, arrays/tuples, `const` evaluation, `extern "C"` FFI |
| v5  | Stdlib depth (strings, generic `HashMap`), file I/O + CLI args, self-written capstones (`calc`, `rpn`) |
| v6  | "make the heap recursive" — `Box` / recursive enums; a JSON parser written in kardashev |
| v7  | "real numbers, real abstraction" — `f64`, `#[derive]` Clone/Eq; JSON 2.0 |
| v8  | "generics, finished" — `Ord`/`Hash`/`Default` derives, generic trait objects; JSON 3.0 |
| v9  | "data in motion" — `Vec` combinators, string tools; a word-frequency capstone |
| v10 | "sized and sound at compile time" — const-generics, dimension-checked matrices, effect-subset soundness |
| v11 | "real machine integers" — the numeric tower (sized int/float, `as`, bitwise, defined wrapping) |
| v12 | "real stdlib" — parsing, `Vec`/`HashMap`/`String` methods, math helpers |
| v13 | "concurrency" — the `share` effect, typed MPSC channels, the structural `Send` rule |
| v14 | "hardening" — cross-platform CI (macOS green), a JIT-vs-AOT differential sweep |
| v15 | "self-hosting" — a compiler front-end written in kardashev |
| v16 | "self-hosting, continued" — the body grammar (parser + interpreter) |
| v17 | "a compiler in kardashev" — a self-hosted type checker + code generator; capstone `compile.kd` |
| v18 | "hardening II" — review-followup fixes + a differential fuzzer |
| v19 | "hardening III" — memory-safety + integer fuzzers, cleaner diagnostics |
| v20 | "toward a real bootstrap" — the self-hosted compiler emits **real LLVM IR** (clang → native, differential-gated vs the host), plus **structs** and **enums + match** |

**v20 — toward a real bootstrap (shipped):** the self-hosted compiler now emits **real LLVM IR** — `clang`-compiled to a native binary and differential-gated against the host — for an i64/bool language **with structs and enums + match**, well past the old stack-VM "toy". It is still a *subset* of kardashev, so not yet a true bootstrap. **Next (v21+) — and an honest take on where this stands vs production languages:** see **[ROADMAP.md](ROADMAP.md)**.

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.

## License

Licensed under either of

 * Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
   <http://www.apache.org/licenses/LICENSE-2.0>)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or
   <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
