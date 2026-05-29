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
kardc --test <file.kd>           # run every `test_*() -> i64` fn (0 = pass)
kardc -O0|-O1|-O2|-O3 ...         # optimization level (default -O2)
kard-lsp                         # Language Server Protocol over stdio
                                 #   (diagnostics, hover, completion,
                                 #    go-to-definition, find-references, rename)
kard build | kard run            # build/run a kard.toml project (no file arg)
```

The optimization flag selects the post-codegen LLVM pass pipeline: `-O0`
runs LLVM's minimal O0 pipeline (no inlining; alloca-heavy IR kept), while
`-O1/-O2/-O3` run the matching `buildPerModuleDefaultPipeline`. The default
is `-O2` — byte-for-byte the historic behavior. The level is folded into the
AOT compile-cache key, so `-O0` and `-O2` objects never collide.

`kardc --test` discovers test functions by convention — `fn test_*() -> i64`
(no params; `return 0` = pass, nonzero = fail) — compiles the file once,
JIT-runs each test, prints a `running N tests … result: X passed, Y failed`
summary, and exits nonzero if any test failed. A test file need not define
`main()`.

Plus the thin `kard` shell wrapper (`kard build`, `kard run`, `kard test`,
`kard repl`) and Bazel rules (`kardashev_library`, `kardashev_binary`) for
projects that want to compose kardashev targets into a larger Bazel
monorepo. A worked capstone — a reverse-Polish-notation calculator
exercising `Vec`/`HashMap`/structs/trait dispatch/`match`/`Option`/`Result`
— lives in `examples/rpn/`.

### Package manifest (`kard.toml`) + local-path dependencies

A project can carry a `kard.toml` at its root. `kard build` / `kard run`
**with no file argument** read it, compile the declared entry point, and make
each local-path dependency resolvable:

```toml
[package]
name = "app"
version = "0.1.0"
entry = "src/main.kd"            # optional; defaults to src/main.kd

[dependencies]
mathlib = { path = "../mathlib" }   # inline-table form
strutil = "../strutil"               # bare-string form (path)
```

```
// src/main.kd
mod mathlib;
fn main() -> i64 { square(5) + cube(3) }   // square/cube come from mathlib
```

A local dependency's library `.kd` is located under its path (`<dep>.kd`,
`src/lib.kd`, `lib.kd`, or `src/<dep>.kd`, or the path itself if it's a `.kd`
file) and staged as a `mod`-resolvable sibling of the entry, so kardashev's
existing intra-project flat-merge picks it up — then unstaged afterward (the
source tree is left untouched). The manifest parser is a tiny hand-written
reader in the `kard` wrapper (no TOML library dependency). A missing/malformed
manifest, a missing entry, or an unresolvable dependency each fail with a
clear message.

**Deferred (documented, not stubbed):** resolving *third-party* dependencies
via the Bazel module registry. Bazel can't run in this build environment, so a
real registry integration isn't verifiable here and is intentionally left as
future work — the `kard.toml` manifest + local-path resolution above is what's
implemented and tested (`tests/smoke_test_manifest.sh`).

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

## Roadmap v2 — shipped

v1 proved the pipeline end to end but leaned on recursion + `match` for control
flow, top-level `fn`s for higher-order code, and static dispatch everywhere. v2
closed those gaps and is now implemented and green on the full test suite (6
unit suites + 18 smoke tests, JIT + AOT). The **north star** held: the
effect-polymorphic higher-order pattern — closures + effect-carrying function
types + iteration — now composes end to end, which is exactly what the
language's thesis ("effects are part of the type") demands.

| Phase | Goal | Status |
|-------|------|--------|
| 9 | **Iteration**: `while`, `loop`/`break`/`continue`, `for x in it`, and an `Iterator` trait | ✅ `while` / `loop` with `break`/`break v`/`continue`; `let mut` + assignment; `for x in a..b` / `a..=b` over a first-class `Range`; a prelude `Iterator` trait (`fn next(&mut self) -> Option<i64>`) with `for` desugaring through `next()` for any impl (Phase 13a). |
| 10 | **Closures + effect-carrying function types** | ✅ The `Function` type carries an effect row (`effectLabels` + an HM row variable); `fn(T) -> U ! {e}` row-polymorphism is real — a pure caller invoking `apply(ioFn)` is rejected. Capturing closures `\|x\| x + n` lower to a heap env-struct + a uniform fat-pointer fn-value that subsumes the Phase 4.3 path; a closure's effect row is inferred from its body. **The capstone of the signature feature.** |
| 11 | **Trait objects + dynamic dispatch**: `dyn Trait`, vtables, `Box<dyn Trait>` | ✅ Trait objects are `{data, vtable}` fat pointers behind `&dyn Trait` / `Box<dyn Trait>`; per-impl vtable globals + thunks; one call site dispatches to multiple runtime impls; object-safety enforced. Static dispatch is an unchanged separate path. |
| 12 | **Real async runtime**: suspending primitive + single-threaded executor + multi-state state-machine transform | ✅ `Future = {poll, frame}`; each `async fn` lowers to a resumable poll fn over a heap frame that `switch`es on a resume state and spills locals live across awaits; `.await` genuinely suspends (returns `Pending`) and resumes. `yield_now` suspends once; `block_on` drives to completion. Verified: the `pending` path is taken (poll count > awaits) and pre-await effects fire once. |
| 13 | **Growable stdlib**: mutable `String`, `HashMap<K,V>`, `&[T]` slices, iterator adaptors (`map`/`filter`/`fold`), `Option`/`Result` combinators | ✅ Method receivers autoref-borrow (`&self`/`&mut self`) so stateful methods work; heap-backed growable `String` (`push_str`), `HashMap<i64,i64>` (linear-probing + rehash) returning `Option`, `&v[a..b]` slices; `fold`/`map`/`filter` as generic closure-driven library fns; prelude `Option`/`Result` combinators — all effect-row aware. |
| 14 | **Tooling + ecosystem**: `kard fmt`, richer LSP (completion/hover/go-to-def), incremental build cache, dependency resolution via the Bazel module registry, DWARF debug info | ✅ (14a) `kardfmt` idempotent source formatter + DWARF debug info behind `-g`. ✅ (14b) `kard-lsp` serves hover (signature **with the effect row**), scope-aware completion, and go-to-definition on top of diagnostics; `kardc -o` has a content-addressed incremental compile cache (`${XDG_CACHE_HOME:-~/.cache}/kardashev`, keyed on resolved source + flags + format version; `--no-cache` to bypass). **Deferred (1 item):** cross-project dependency resolution via the Bazel module registry — intra-project deps work via `mod foo;` + the `kardashev_library`/`kardashev_binary` rules, but a third-party package registry isn't implemented (it can't be verified in this build environment, so it was intentionally not stubbed). |

## Roadmap v3 — shipped

v2 made the language expressive; v3 makes it a language you can run in production,
and is now implemented and green on the full suite (6 unit suites + 30 smoke tests,
JIT + AOT, ubuntu + macOS). The **north star held**: a program that allocates in a
loop now runs in *constant memory* (Drop frees deterministically), and you write
idiomatic code (`if flag`, `else if`, `!done`) without contortions. One item stays
deferred (cross-project deps via the Bazel module registry — unverifiable in this
build environment); everything else is real and tested.

| Phase | Goal | Status |
|-------|------|--------|
| 15 | **Expression & item completeness**: boolean literals `true`/`false`, unary `-x` (neg) and `!x` (logical not), `else if` chains, inherent impls `impl Type { … }`, `pub` on all item kinds | ✅ `true`/`false` → `i1`; unary `-x`/`!x` with a precedence layer tighter than binary ops; `else if` ladders; inherent `impl Type { … }` resolving into the same method table as trait impls (incl. `&mut self`); `pub` parsed/stored on all item kinds (enforcement stays fn-only). Also fixed `fn main() -> bool` JIT/AOT return-width handling. |
| 16 | **Deterministic memory management (Drop / RAII)**: a `Drop` trait + compiler-inserted drops at scope exit, driven by the existing NLL ownership/move analysis | ✅ `trait Drop`; Vec/String/HashMap/Box get built-in free glue; drops are inserted at scope exit in reverse order via **runtime drop flags** (so conditional moves drop exactly once — never double-free/UAF); moved/returned values aren't dropped. Verified: reverse drop order, move semantics, and a 2 M-iteration alloc loop running in **constant ~1.5 MB RSS**. (Closure-env + async-frame contents aren't dropped yet — documented leak, no UAF.) |
| 17 | **Fully generic stdlib + closures**: de-`i64`-ify `Iterator`/`Future<T>`/`HashMap<K,V>`/adaptors; `FnMut` + capture-by-reference; calling a field-held fn value | ✅ Generic `Future<T>` (async returns `bool`/structs; `block_on<T>` specialized per-T) and `HashMap<i64, V>` (generic value via `DataLayout` stride); `FnMut`/capture-by-reference closures (env stores a pointer to the captured slot; by-ref closures can't escape); calling a field-held fn value `(s.f)(x)`. **Deferred:** generic `Iterator<T>` element type (still `i64`) — needs generic-trait/associated-type support; and `HashMap` non-`i64` keys (needs a `Hash` trait). |
| 18 | **Real async I/O + multitask executor**: an epoll/kqueue reactor, timer + I/O-readiness leaf futures, `spawn` + `join` | ✅ A process-global executor holds a growable queue of type-erased `Future` tasks and round-robins them: `spawn(f)` enqueues a task (returns an i64 handle), `block_on(f)`/`join(h)` drive the whole queue (so spawned tasks interleave) until the target completes. `sleep_ms(n)` is a real `CLOCK_MONOTONIC` timer leaf; when every task is Pending the reactor **sleeps** (`nanosleep`, or `epoll_wait` on Linux) until the nearest deadline instead of hot-spinning (3×100 ms sleeps finish in ~100 ms at ~0% CPU). Linux/epoll fd-readiness landed (`pipe_make`/`pipe_send`/`read_pipe` — an async reader suspends in `epoll_wait` and wakes on a write); macOS/kqueue fd-readiness is deferred (timers work there via `nanosleep`). |
| 19 | **Threads + compile-time data-race freedom**: OS threads, `Send`/`Sync` marker traits enforced by the type system, channels, `Mutex`/atomics | ✅ Real `pthread`-backed `thread_spawn`/`thread_join` running a kardashev fn value on a new thread; `Mutex` (pthread_mutex-backed) — verified 2 threads × 100 000 shared increments == exactly 200 000, deterministic. **Data-race freedom (enforced):** `thread_spawn` rejects at compile time any closure that captures a binding **by reference** (it would alias the spawning frame's stack across threads) — captures must be by value (`Send` by move) or shared via a `Mutex` handle. **Deferred:** full `Send`/`Sync` marker traits + a `Sync` rule for shared `&T`, channels, and atomics (the by-ref rejection is the enforced floor). |
| 20 | **Production toolchain & ecosystem**: cross-project dependency resolution via the Bazel module registry (the v2 deferral), a package manifest, `kard test`, `-O0/-O2/-O3` opt-level flags, LSP rename/find-references — capped by building a real non-trivial program end to end | ✅ (20a) `-O0..-O3` opt-level flags (folded into the AOT cache key), the `kardc --test` runner (`fn test_*() -> i64`, pass/fail counts + exit code), and the RPN-calculator capstone (`examples/rpn/`). ✅ (20b) `kard-lsp` adds **find-all-references** (resolves the symbol under the cursor to its definition site over the Phase 14b occurrence index, then returns every occurrence that resolves to it; honors `includeDeclaration`) and **rename** (a single-document `WorkspaceEdit` rewriting every occurrence + the declaration); a `kard.toml` **package manifest** + **local-path dependency** resolution drive `kard build`/`kard run` with no file argument. **Deferred (1 item, unchanged from v2):** third-party dependency resolution via the Bazel module registry — Bazel can't run in this build environment, so it isn't verifiable here and was intentionally not stubbed; intra-project + local-path deps (`mod foo;`, `kardashev_library`/`kardashev_binary`, and now `kard.toml` `[dependencies]`) are what ship. |

Landed in order **15 → 16 → 17 → 18 → 19 → 20** (17's generic `Future<T>`
unblocked 18; 16's Drop underpins 19's safe sharing), each verified — clean build
+ direct behavior checks — and CI-green on ubuntu + macOS before the next built on it.

## Roadmap v4 — shipped

v3 made kardashev a real systems language you can *run*; v4 makes it one you'd
reach for, and is now implemented and green on the full suite (6 unit suites + 35
smoke tests, JIT + AOT, ubuntu + macOS). The stdlib is no longer `i64`-bound, the
`panic`/`unwind` labels have a real runtime, the language talks to C, and there are
arrays/tuples/`const`. The **north star** is met by `examples/calc/` — a real
recursive-descent arithmetic interpreter written *in kardashev itself*, tokenizing
+ parsing-with-precedence + evaluating, compiled by `kardc`.

| Phase | Goal | Status |
|-------|------|--------|
| 21 | **Generic traits + associated types + `where` clauses**: `trait Iterator<T>` / `trait Container { type Item; }` / `fn f<T>() where T: Bound` | ✅ Trait type parameters (`trait Name<T>`), impls supplying them (`impl Iterator<i64> for Range`), parameterized bounds (`fn head<T, C: Container<T>>`), and the prelude `Iterator` migrated to `Iterator<T>` so `for`/`fold`/`map`/`filter` work over any element type. Associated types (`type Item;`, `Self::Item`, and `C::Item` at a bounded call site) + `where` clauses (desugared to inline bounds, byte-identical IR). **Deferred:** generic `dyn Trait<T>` (static dispatch only); impls take no own generic list; multiple bounds per param; assoc-type bounds/defaults. |
| 22 | **Aggregate data: fixed-size arrays `[T; N]` + tuples `(A, B)`** | ✅ Stack value-aggregates: arrays `[T; N]` → LLVM `[N x T]` (literal `[a,b,c]`, indexing, `arr[i] = x`, `&[T;N]` auto-deref) and tuples `(A, B)` → anonymous struct (`.0`/`.1` access, `let (x, y) = t` destructuring). **Deferred:** tuple `match` patterns, non-Copy elements, runtime bounds-checking of dynamic indices. |
| 23 | **Real panic + unwinding**: make the `panic`/`unwind` effect labels honest | ✅ `panic(msg)` prints to stderr then unwinds via setjmp/longjmp + a cleanup stack, **running Drop glue on the way out** (verified: guards drop in reverse during unwind), with `catch(f, recover)` to recover and an uncaught panic exiting 101. The single Phase-16 drop flag gates both the normal and panic paths so every value drops exactly once (no double-free, verified with a 100k-panic constant-memory loop). Panic-free programs emit zero panic machinery. |
| 24 | **FFI / C interop**: `extern "C"` import + export | ✅ `extern "C" fn name(args) -> T;` (and block form) declares external C functions, lowered to an unmangled LLVM extern + direct call; JIT resolves from the host process, AOT links via clang. Type→C-ABI mapping incl. a `i32`/C-`int`-width spelling (trunc/sext at the boundary) and `&String`/`&[T]` → C pointer; extern calls carry `io`. Verified `abs(-7)=7`, `strlen("hello")=5` in JIT+AOT. **Deferred:** export-to-C attribute. |
| 25 | **comptime / const evaluation**: `const` items, `const fn`, const folding, const generics | ✅ `const NAME: T = <const-expr>;` (i64/bool) evaluated at compile time and folded to a literal at every use (verified in `--emit-llvm`: `ret i64 5`, no runtime load/global); a const may reference an earlier const, and a cyclic/forward-bad reference is a clear error. `const fn` runs at compile time when called in a const context (with constant args) and is **also** an ordinary runtime fn (codegen unchanged). The evaluator covers int/bool literals, arithmetic/comparison/unary ops, `if`/`else`, `let`, and const-fn calls; it is bounded by call-depth + a step budget (runaway → clear error, not a hang), and integer overflow / div-by-zero are compile errors. **Const generics:** the `N` in `[T; N]` (Phase 22) is now any const-expr — a `const` item, a const-fn call, or arithmetic over them (`[i64; N]`, `[i64; sq(2)]`, `[i64; A + 1]`), evaluated to the array length. **Deferred:** const types beyond i64/bool, and full const-generic *type parameters* (`struct Arr<const N: i64>`) — only const-expr array lengths are in scope. |
| 26 | **Self-hosting milestone + stdlib depth** | ✅ `examples/calc/` — a real recursive-descent arithmetic interpreter written *in kardashev*, compiled by `kardc`: it tokenizes a string byte-by-byte (via the new `str_char_at` builtin) into a `Vec<Tok>`, parses with proper precedence through mutually-recursive grammar rules, and evaluates — exercising enums+`match`, structs, tuples, `Vec`, recursion, loops, and `const`. Verified end to end (`12 + 3 * (4 - 1)` → 21, etc.). Stdlib gained `str_char_at` (the tokenizer's core need). **Deferred:** broader stdlib (file I/O, formatting, generic sets) beyond what the capstone needed. |

Landed in order **21 → 22 → 23 → 24 → 25 → 26** (21's generic traits de-`i64`'d the
Iterator stdlib; 22's arrays fed 25's const-generic lengths; 23 built on v3's Drop;
24's FFI + 26's capstone pair up), each verified — clean build + direct behavior
checks — and CI-green on ubuntu + macOS before the next built on it.

## Roadmap v5 — shipped

v4 proved kardashev can compile a real program written *in itself* (`examples/calc/`).
v5's **north star** made that no longer a demo: two genuinely non-trivial tools written in
kardashev — a **JSON parser** (`examples/json/`) and a **kardashev-subset lexer**
(`examples/kdlex/`) — compile and run green, standing on a deeper stdlib, a tighter memory
model, and accurate docs. Phases 27–32 are implemented and green on the full suite (6 unit
suites + 46 smoke tests, JIT + AOT, ubuntu + macOS). As in v1–v4, each phase shipped fully
green before the next built on it, and everything unverifiable in this build environment is
documented-deferred, never stubbed.

| Phase | Goal | Status |
|-------|------|--------|
| 27 | **String toolkit** | ✅ `str_eq` (byte-exact, pure), `str_substring` (a fresh heap slice, start/len **clamped** into bounds), `int_to_string` (`snprintf "%lld"`), and `print_no_nl` — the genuinely new output op, since `print_str` / `print_string` / the new `println` all force a trailing newline. Verified JIT + AOT (`smoke_test_strings`). |
| 28 | **`Hash` trait + generic map/set keys** | ✅ Multiple trait bounds (`T: A + B`, inline + via `where`); a prelude `Hash` + `Eq` trait with built-in i64/String impls; **generic `HashMap<K, V>`** (the bucket entry now stores `K` — i64 keys keep the historic inline identity-hash+icmp; String keys hash via FNV-1a + compare via `str_eq`; **user key types** dispatch through their `impl Hash`/`impl Eq`); and `HashSet<T>`. A non-hashable key is a clear error. Verified at i64/String/user-struct keys (`smoke_test_hash`). |
| 29 | **Plug the documented Drop leaks** | ✅ The closure-env and `match`-payload-binding leaks are closed: a fn-value is droppable (its heap capture env is freed), and a droppable enum payload bound in a `match` arm drops at arm exit unless moved out. Verified with 2 M-iteration constant-memory loops (~1.5 MB RSS) for both, plus a no-UAF check that a moved-out payload survives (`smoke_test_dropleaks`). *(Dynamic array-index OOB already panics since Phase 23.)* **Documented-deferred:** async-frame interior free — freeing a completed `Future`'s heap frame needs reworking the executor task lifecycle (read-after-free risk on the poll slot) and async is off the v5 capstone path, so it stays a known leak rather than a risky half-fix. |
| 30 | **File I/O + CLI args** | ✅ `fs_read_to_string` → `Result<String, IoError>`, `fs_write` → `Result<i64, IoError>`, `fs_exists` → `bool`, and `args()` → `Vec<String>` (`arg_count`/`arg_get`). Errors are classified **portably** via `access()` probes (no libc `errno` symbol, so the IR links on Linux + macOS); the AOT `int main(argc, argv)` wrapper captures argv (JIT sees none); the runtime is emitted only when a program uses it. Verified JIT + AOT (`smoke_test_io`). |
| 31 | **Capstones, written in kardashev** | ✅ `examples/json/` parses a JSON object into a `HashMap<String, i64>` (the headline Phase-28 map), and `examples/kdlex/` lexes a kardashev subset into a `Vec<Tok>` and counts `fn` decls + checks brace balance — both compiled by `kardc`, JIT + AOT (`smoke_test_capstones`). **The north star.** *(Subset, documented: the JSON parser targets a numeric-config object — top-level, integer values — the sound shape that exercises the String-keyed map end to end; nested/string/bool values are out of scope.)* |
| 32 | **Docs + source-comment truth pass** | ✅ `docs/` (language reference, effects, stdlib, architecture) rewritten from the Phase-7/v2 era to v5; the stale "Phase 6 (stub)" comments that labelled now-fully-working async code are purged; a `doc-lint` smoke test (`smoke_test_doclint`) guards against the markers and the worst stale claims regressing. |

**Documented-deferred (carried forward unchanged, never stubbed):** third-party dependency
resolution via the Bazel module registry (Bazel can't run in this build environment;
`mod foo;` + `kard.toml` local-path deps are what ship); macOS/kqueue async fd-readiness
(Linux/`epoll` only here; timers work cross-platform, and CI covers macOS); and the new v5
deferral above — async-frame interior free (the `Future` heap frame is reclaimed by neither
`block_on` nor the executor yet, so a long-running async workload leaks frames; a bounded
one-shot does not).

Landed in order **27 → 28 → 29 → 30 → 31 → 32** along the planned dependency arc: 27's
strings fed 28's string-key hashing and 31's tokenizers; 28's `Hash` trait preceded the
generic keys; 29 plugged the Drop leaks 27–28's new droppable values made load-bearing;
30's `Result<String, IoError>` drops cleanly on the error path *because* 29 closed that
hole; 31 integrated 27–30 into the self-written capstones; 32 documented the result last.
Each shipped green before the next, exactly as v1–v4 did.

## Roadmap v7 — shipped

> **Status: shipped.** All of v7 (Phases 39–44) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared clean build. The capstone is **JSON 2.0**: the nested-JSON value now carries `f64` numbers and decoded string escapes, and its `Clone`/`Eq` are `#[derive]`d — round-tripping via the derived `Eq`, leak-free under the constant-memory gate.

v6 made the heap recursive and proved it on a full nested-JSON value — but that capstone could only carry **integers** (`1.5` didn't even lex) and its helpers were hand-written because kardashev had **no generic `impl<T>` blocks**. v7's theme is **real numbers, real abstraction**: it gave the value layer `f64`, gave the abstraction layer generic impls so `Clone`/`Display`/`Eq` are real `#[derive]`-able traits, and proved it by upgrading JSON to floats + escapes with derived helpers — still leak-free.

| Phase | Goal | Status |
|-------|------|--------|
| 39 | **`f64` floating-point** | ✅ Lexer float literals (`1.5`, `-2.0`, `3e8` — carefully not swallowing `1..5`); an `f64` type → LLVM `double`; arithmetic (`+ - * /`), ordered comparisons, FNeg; explicit `to_f64` / `float_to_int` conversions + `print_f64` (no implicit i64↔f64 mixing); `f64` is Copy + a valid Copy aggregate element. Verified: an `f64` kernel + a `Vec<f64>` sum, JIT + AOT (`smoke_test_phase39`). *(Single width; NaN/Inf as LLVM gives.)* |
| 40 | **Generic `impl<T: Bound>` blocks** | ✅ Parse + typecheck + monomorphize `impl<T: Trait> Trait2 for Container<T>` (the open Phase-21 gap): the impl's params are prepended to each method's generic vars, bound-checked, and inferred from the receiver; methods monomorphize per concrete T. Verified: `impl<T: Display> Display for Pair<T>` formats `Pair<i64>` AND `Pair<String>` (distinct instances); a generic impl on built-in `Vec<T>`; unbounded-T trait call rejected — JIT + AOT (`smoke_test_phase40`). |
| 41 | **Real generic `Clone` / `Eq` over containers** | ✅ Prelude `impl<T: Clone> Clone for Vec<T>` + `impl<T: Eq> Eq for Vec<T>` dispatch through each element's own impl (the `clone(&x)` intrinsic stays the fallback). Required completing the Phase-40 deferral — a generic-impl method dispatching THROUGH a bounded-generic receiver to another generic-impl method (the call site now threads the receiver's concrete type args). Verified: `Vec<Pair<String>>` deep-cloned via a user `Clone for Pair` (→ String::clone) + compared via `Eq`, constant memory, JIT + AOT (`smoke_test_phase41`). *(Box/HashMap trait Clone/Eq deferred to the intrinsic.)* |
| 42 | **`#[derive(Clone, Eq, Display)]`** | ✅ A `#[derive(...)]` attribute (new `#` token) that SYNTHESIZES the impls by generating impl source from the type's fields/variants and re-parsing it — riding the real generic-impl machinery. Verified: `#[derive(Clone, Eq)]` on a recursive `enum Json` clones + deep-compares with no hand impl; `#[derive(Display)]` yields `P { x: .., y: .. }` / `C(a, b)`; works on a generic `Pair<T>` — JIT + AOT (`smoke_test_phase42`). |
| 43 | **Runtime string escapes + close the async-frame leak** | ✅ `str_unescape`/`str_escape` (kardashev prelude fns over a new `str_push_byte`) decode/encode `\n \t \r \" \\ \/ \uXXXX` for runtime strings (\u = Latin-1). **And the last documented leak is closed**: completed Futures' heap frames + poll slots are reclaimed — block_on reaps when the executor is idle (safe: never frees a live frame nor invalidates a spawn/join handle), and an `.await` frees the completed sub-future's frame. A 300k block_on loop drops ~50 MB → flat ~1.5 MB; spawn/join/executor intact. Verified JIT + AOT (`smoke_test_phase43`). |
| 44 | **Capstone: JSON 2.0 — floats + escapes + derived helpers** | ✅ `examples/json` now carries `JNum(f64)` + escape-aware string parse/serialize (+ a new `f64_to_string`), with `#[derive(Clone, Eq)]` on `Json`/`Entry` — the hand-written `json_eq` retired. Round-trips via the DERIVED `Eq`. Verified: parse `{"pi":3.14159,"msg":"a\nb","xs":[1.5,-2.0,true,null]}`, derived-Eq round-trip, 200k parse+serialize+drop loop flat at ~1.6 MB, JIT + AOT, wired into `make test` (`smoke_test_json`). *(Objects are an order-preserving `Vec<Entry>` so `#[derive]` applies uniformly — a HashMap trait Clone/Eq needs a bounded-key Hash+Eq path, deferred; the v6 HashMap object form + its intrinsic clone remain valid.)* **The north star — reached.** |

**Landing order — 39 → 40 → 41 → 42 → 43 → 44.** 39 added the missing scalar (self-contained). 40 was the **keystone**: generic `impl<T>` blocks unblocked everything abstraction-related. 41 used 40 to make `Clone`/`Eq` real generic traits; 42 derived them automatically. 43 closed the runtime-escape + async-frame gaps. 44 integrated all five into the floats-and-escapes JSON with derived helpers — the integration proof under its constant-memory gate. Each phase shipped fully green before the next, exactly as v1–v6 did.

## Roadmap v6 — shipped

> **Status: shipped.** All of v6 (Phases 33–38) is implemented and fully green — 6 unit suites (631 cases) + the smoke-test aggregate, JIT **and** AOT, on a cleared cache. Each phase shipped green before the next built on it, exactly as v1–v5 did. The capstone — a **full nested-JSON parser + serializer written in kardashev** — round-trips and runs leak-free under a constant-memory gate.

v5 proved kardashev can compile genuinely non-trivial tools written *in itself* — but its JSON capstone (`examples/json/`) had to collapse to a **flat numeric-config subset** (`HashMap<String, i64>`, integer values only), because reading a value back out of a container is a buffer-sharing *shallow copy* that Phase-29 drop then double-frees, and `match` cannot go *through* a reference. v6's **north star** retires that asterisk: a **FULL nested-JSON value type written in kardashev** —

```rust
enum Json { JNull, JBool(bool), JInt(i64), JStr(String), JArr(Vec<Json>), JObj(HashMap<String, Json>) }
```

— that parses arbitrarily nested input, traverses it **by reference**, round-trips back to canonical text through a `Display` serializer, and runs **leak-free under a constant-memory smoke test** (JIT + AOT). The **theme** is *make the heap recursive*: close the recursive heap-owning-enum soundness gaps (read-without-move, match-through-reference, sound recursive `Drop` + `Clone`, enum-typed fields), then dress the surface for real programs (`%`/`&&`, `Result` errors, `Display` formatting, de-`i64`'d iteration), and prove it end to end by promoting the v5 SUBSET to a full recursive tree.

| Phase | Goal | Status |
|-------|------|--------|
| 33 | **Container interior drop + rehash reclaim + the missing `%`/`&&` operators** | ✅ The HashMap/HashSet `emitDropGlue` arm now walks occupied buckets and drops every live key+value before freeing the array (generalizing the Vec template), and rehash-grow frees the **old** bucket buffer. `%` modulo (lexer `Percent` → `*`-tier → LLVM `SRem`) and short-circuit `&&` (two-char `AmpAmp` → branch IR) ship. Verified: a `HashMap<String, Vec<i64>>` built/dropped in a 2 M-iteration loop holds flat peak-RSS; `%`/`&&` green JIT + AOT (`smoke_test_phase33`). *(Only `%`/`&&` — `\|\|` is the zero-param-closure token.)* |
| 34 | **Read-without-move: borrow into containers + `match &x` / `*r`** | ✅ `vec_get_ref(&v,i) -> &T` / `hashmap_get_ref(&m,k) -> Option<&V>` return a **borrow**; a `*r` deref operator; and `match &Enum` binds payload sub-patterns **as references** (one-layer auto-deref). Verified: a `Vec<String>` / a `Vec` of a heap-owning enum read & `match &`'d element-by-element, originals drop exactly once, constant memory JIT + AOT (`smoke_test_refread`). Two pre-existing latent bugs fixed en route: a String-struct double-layout, and a per-iteration loop-binding stack overflow (alloca hoisting). *The keystone.* |
| 35 | **Sound recursive heap enums + `clone`** | ✅ A self-recursive type (`Vec<Self>`/`HashMap<K,Self>`/`Box<Self>` payloads) lays out (a `resolveInInstance` cycle guard) and drops via a per-type **drop function** that recurses at run time. `clone<T>(&T) -> T` is a deep-copy intrinsic over scalars/String/Vec/HashMap/Box and user structs/enums incl. recursive types (subsumes the planned trait+hand-written-impls since generic `impl<T>` blocks stay deferred — the intrinsic is strictly more capable). **Fixed a serious pre-existing miscompile**: the optimizer ran without the target datalayout, folding multi-field-aggregate GEPs to wrong offsets (a value read through a pointer returned garbage at -O1+). Verified: a 1000-node-deep `Tree` cloned + dropped, and a 200k build+clone+match+drop loop flat at ~1.6 MB (`smoke_test_clone`). |
| 36 | **Enum-typed struct fields + non-Copy tuples + tuple `match` + `Result` errors** | ✅ Struct/enum layout split into name + body passes so a struct field of enum type (and vice-versa) resolves; `&place` borrows (`match &h.t`). Tuples hold non-Copy elements (`(String, i64)`, `(Json, i64)`) with drop + move-tracking (arrays stay Copy-only); tuple `match` patterns across parser → typecheck → the match compiler (irrefutable single-constructor) → codegen; `Result<_, ParseError>` via the shipped `?`. Verified: recursive-enum field read, `(String,i64)` returned + destructured (dropped once), tuple/Result match — JIT + AOT, 300k loop flat (`smoke_test_phase36`). |
| 37 | **`Display`/`to_string` + de-`i64`'d generic iteration & slices** | ✅ A prelude `Display` trait (`to_string(&self) -> String`) with built-in impls for i64/bool/String (user types hand-write `impl Display`); `&[T]` slices generic per element T (`slice_len`/`slice_get`/`slice_get_ref`), non-owning. `checkFor` already bound the loop variable to the iterator's real `next()` payload — verified with an `Iterator<String>`. Verified: a bounded `show<T: Display>`, a recursive `impl Display for Json` serializing `[3,[1,22]]`, a `for s in <Iterator<String>>`, and a `&[String]` slice read — JIT + AOT (`smoke_test_phase37`). *(Static dispatch only; `dyn Display` deferred.)* |
| 38 | **Capstone: FULL nested-JSON (parse + serialize, leak-free) + doc-truth fixup** | ✅ `examples/json/` is the full recursive `enum Json { … JArr(Vec<Json>), JObj(HashMap<String, Json>) }`: a `&mut self` recursive-descent parser, a recursive `Display` serializer, and an order-independent deep-equality round trip — all in kardashev. Added `hashmap_keys` (enumerate a map) and fixed three by-value-consumer leaks (`string_push_str`, `hashmap_get`/`get_ref` now free what they consume). Corrected the stale closure-env "not freed today" comments. Verified: parses `{"a":[1,{"b":true},"x"],"n":null,"k":-42}`, round-trips, and a 200k parse+serialize+drop loop flat at ~1.6 MB, JIT + AOT, wired into `make test` (`smoke_test_json`). **The north star — reached.** |

**Landing order — 33 → 34 → 35 → 36 → 37 → 38 — along a strict dependency arc.** 33 established the interior-drop + constant-memory leak-probe discipline on the proven Vec template and folded in the two cost-free operators. 34 was the **keystone**: read/`match`-by-reference is the root-cause fix for the v5 subset, so it preceded everything touching recursive data. 35 made the recursive value itself a sound droppable/clonable heap citizen. 36 gave the AST node its *shape* (enum-typed fields, non-Copy tuples). 37 supplied the `Display` serializer backbone and de-`i64`'d iteration. 38 integrated all five into the full-nested-JSON capstone — the integration proof that surfaced (and closed) every latent leak/UAF under its constant-memory gate. Each phase shipped fully green before the next built on it, exactly as v1–v5 did.

**Documented-deferred (carried forward unchanged, never stubbed):** third-party dependency resolution via the **Bazel module registry** (Bazel cannot run in this build environment — `MODULE.bazel`-only, no `WORKSPACE`; `mod foo;` + `kard.toml` local-path deps are what ship); **macOS/kqueue** async fd-readiness (Linux/`epoll` only; timers work cross-platform and CI covers macOS); the v5 **async-frame interior free** + **all droppable async-fn locals** (the `inAsyncFn_` early-return at codegen.cpp:6279 — async is off the v6 north-star path, so the known leak/no-UAF stays rather than a risky executor-lifecycle half-fix). **Off-lens, still open** (honestly not in v6 scope): full **`Send`/`Sync`** marker traits + channels + atomics (the by-ref-capture rejection at typecheck.cpp:4841 remains the enforced floor); **generic `dyn Trait<T>`**, generic `impl<T>` blocks, associated-type bounds/defaults (Phase 21); **const-generic type params** + non-i64/bool const values (Phase 25); `extern "C"` **export** (Phase 24); full **`&mut` auto-reborrow** through recursive calls; **`#[derive]`** machinery; cross-file LSP rename; `pub` enforcement beyond functions; real module namespacing. Anything verifiable only under Bazel or macOS is flagged, not pretended-done.

## Why "kardashev"?

The [Kardashev scale](https://en.wikipedia.org/wiki/Kardashev_scale) ranks civilizations by how much energy they can harness. A systems language, in its own small way, is about controlling resources at scale — a fitting name for one that aims to be precise about effects, ownership, and computation.
