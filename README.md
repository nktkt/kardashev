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

The full README roadmap (Phases 0–8) lands in the repository. Built
locally with `bazel build //... && bazel test //...` or, when Bazel
isn't available, the `Makefile.local` shim (LLVM + clang). The CI
matrix runs both ubuntu-latest and macos-latest via Bazel on every
push; every commit goes in green.

Tour: see [`docs/`](docs/) for the language reference, effects-system
notes, stdlib catalog, and compiler-architecture deep dive.
[`examples/hello/`](examples/hello/) shows a two-file program built
through the Bazel rules.

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
redeclaring them. A growable `Vec` (heap-allocated `i64` buffer) and
immutable `String` ship too, with `vec_new` / `vec_push` / `vec_get` /
`vec_len` and `print_str` / `str_len`. A built-in `print(n: i64) -> i64
! { io }` writes one integer plus newline to stdout. Callers must
declare every effect they use, same rule as any other effect:

```rust
fn main() -> i64 ! { io, alloc } {
    let s = "hello, kardashev";
    print_str(&s);              // -> "hello, kardashev"
    print(42);                  // -> "42"
    let v = vec_new();
    vec_push(&mut v, 10);
    print(vec_len(&v));         // -> "1"
    0
}
```

Async fns return the built-in `Future` opaque type; `.await` unwraps:

```rust
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }
fn main() -> i64 ! { async, io } {
    print(double(21).await);    // -> "42"
    0
}
```

Multi-file programs: write `mod foo;` at the top of a `.kd` file to
pull in `foo.kd` from the same directory. `pub fn` gates path-qualified
references across module boundaries; bare-name references still resolve
via the Phase 7.1 flat-merge that runs alongside.

```
// util.kd
pub fn double(n: i64) -> i64 { n + n }
// main.kd
mod util;
fn main() -> i64 { util::double(21) }      // -> 42
```

Four driver entry points:

```
kardc                            # interactive REPL (JIT each expression)
kardc <file.kd>                  # JIT-run main() and print result
kardc -o <out> <file.kd>         # AOT-compile to a native executable
kard-lsp                         # Language Server Protocol over stdio
                                 #   (publishes diagnostics for every edit)
```

Plus the thin `kard` shell wrapper (`kard build`, `kard run`, `kard
repl`) and Bazel rules (`kardashev_library`, `kardashev_binary`) for
projects that want to compose kardashev targets into a larger Bazel
monorepo.

The AOT path emits a native object via LLVM's `TargetMachine`,
synthesizes a C-compatible `int main()` wrapper that returns the
kardashev `fn main() -> i64` result truncated to an exit code, and
shells out to `clang` for linking. Programs compile through lexer →
parser → HM typechecker → NLL borrow-checker → effect inference →
LLVM IR → LLVM O2 pipeline → ORC v2 JIT (or AOT).

## Roadmap v1 — shipped

Phases 0–8 are implemented and green on CI (ubuntu + macOS). This is the
self-hosting-compiler MVP: the language compiles, type-checks, borrow-checks,
JITs, and AOT-links real programs.

| Phase | Goal | Status |
|-------|------|--------|
| 0 | Scaffold: Bazel + LLVM toolchain + CI + a JIT binary returning `42` | ✅ |
| 1 | MVP: JIT REPL running `fib` (lexer + parser + monotype HM + LLVM IR + ORC JIT) | ✅ |
| 2 | Ownership + NLL borrow check + structs + enums + pattern matching | ✅ |
| 3 | Traits + generics + `Result` + `?` operator + monomorphization | ✅ |
| 4 | Effect labels in signatures (the signature feature lands here) | ✅ concrete labels (`io`, `alloc`, `panic`, `async`, `unwind`) + propagation + first-class fn-pointer values flow through `let`-bindings and conditionals, dispatched via LLVM indirect calls. |
| 5 | AOT pipeline + minimal stdlib (`Option`, `Result`, `Vec`, `String`) | ✅ AOT + Option/Result via prelude + heap-backed generic `Vec<T>` (per-T specialization with `DataLayout`-sized stride; works for `i64`, `bool`, structs, enums) + immutable `String` (literal-backed). |
| 6 | `async` / `await` + state-machine transform + basic executor | ✅ `async fn` returns the built-in `Future`. Codegen splits each async fn into `__async_body_<n>` + a Future-wrapping shim; `.await` lowers to a poll loop that branches on `READY`, ready to plug a real scheduler under the existing `pending` block once blocking primitives exist. |
| 7 | Module system + complete `rules_kardashev` + `kard` CLI | ✅ `mod foo;` resolves siblings recursively; `pub` enforced on path-qualified references; `foo::bar` path syntax parses; `kard` shell wrapper + Bazel `kardashev_library` / `kardashev_binary` rules ship. |
| 8 | Optimization passes + LSP + docs site | ✅ LLVM O2 PassBuilder pipeline runs on every emitted module; `kard-lsp` speaks the LSP protocol over stdio and publishes diagnostics; `docs/` carries the language reference, effects system notes, stdlib catalog, and compiler-architecture deep dive. |

## Roadmap v2 — from MVP to a language you'd actually write programs in

v1 proves the pipeline end to end, but to keep programs small it leans on
recursion + `match` for control flow, top-level `fn`s for higher-order code,
and static dispatch everywhere. v2 closes those gaps. The **north star** is to
make the headline `map` example at the top of this README compile end to end —
an effect-polymorphic higher-order function — because that exercises closures,
effect-carrying function types, and iteration all at once, which is precisely
what the language's thesis ("effects are part of the type") demands.

| Phase | Goal | What's missing today / why it's next |
|-------|------|--------------------------------------|
| 9 | **Iteration**: `while`, `loop`/`break`/`continue`, `for x in it`, and an `Iterator` trait | The `for` keyword is currently *only* `impl Trait for Type`; there are no loop forms at all, so every repetition is hand-rolled recursion. This is the biggest day-to-day ergonomics gap and a prerequisite for stdlib combinators. |
| 10 | **Closures + effect-carrying function types** | Phase 4.3 gives first-class *top-level* `fn` values, but `types.hpp`'s `Function` kind carries only `args`/`ret` — no effect row. So `fn(T) -> U ! {e}` row-polymorphism through values isn't real yet, and there are no capturing closures (`\|x\| x + n`). This phase lowers closures to env-struct + fn-ptr pairs and threads effect rows through function types so `e` genuinely propagates. **This is the capstone of the language's signature feature.** |
| 11 | **Trait objects + dynamic dispatch**: `dyn Trait`, vtables, `Box<dyn Trait>` | Traits today are static-only (monomorphized). Heterogeneous collections and plugin-style APIs need runtime dispatch via a vtable layout. |
| 12 | **Real async runtime**: blocking primitives (timer/IO-readiness) + single-threaded executor + multi-state state-machine transform | Phase 6.2 scaffolded a poll loop with a `pending` block that's never taken because bodies run eagerly. This phase adds genuine suspension points and a scheduler that drives futures to completion, finally exercising that `pending` path. |
| 13 | **Growable stdlib**: mutable `String` (`push_str`), `HashMap<K,V>`, `&[T]` slices, iterator adaptors (`map`/`filter`/`fold`), `Option`/`Result` combinators | `String` is immutable/literal-backed and there's no associative container. Builds directly on the iterators (9) and closures (10) landed above. |
| 14 | **Tooling + ecosystem**: `kard fmt`, richer LSP (completion/hover/go-to-def), incremental build cache, dependency resolution via the Bazel module registry, DWARF debug info | ✅ (14a) `kardfmt` source formatter + DWARF debug info behind `-g`. ✅ (14b) `kard-lsp` now serves hover (type / signature **with the effect row**), scope-aware completion, and go-to-definition on top of diagnostics; `kardc -o` has a content-addressed incremental compile cache (`${XDG_CACHE_HOME:-~/.cache}/kardashev`, keyed on resolved source + flags + format version; `--no-cache` to bypass). **Deferred:** cross-project dependency resolution via the Bazel module registry — intra-project deps work today through `mod foo;` sibling resolution + the `kardashev_library`/`kardashev_binary` rules, but a third-party package registry isn't implemented (it can't be verified in this build environment, so it was intentionally not stubbed). |

Dependencies: 13 needs 9 + 10; 12 extends 6; 9, 11, 14 are independent and can
land in parallel. Suggested order: **9 → 10 → 13**, with **11 / 12 / 14** slotted
in opportunistically.

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.
