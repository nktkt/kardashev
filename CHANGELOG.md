# Changelog

All notable changes to kardashev are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Versioning

kardashev uses [Semantic Versioning](https://semver.org). It is **pre-1.0**, so
each completed **roadmap** is a `MINOR` bump (Roadmap v9 → `0.9.0`, v10 →
`0.10.0`) and bug-fix releases bump `PATCH`. Per SemVer's 0.x rule anything may
change between minors until 1.0. `1.0.0` is reserved for a language-surface
**stability commitment**; after it the language evolves via opt-in **editions**
(the Rust model) rather than `MAJOR` bumps. The version lives in
`compiler/include/kardashev/version.hpp` (reported by `kardc --version`),
`MODULE.bazel`, and here.

`0.9.0` is the first tagged release; the entries below `0.9.0` document the
pre-tag roadmap history (Phases 0–56), each of which shipped fully green (6 unit
suites + the smoke aggregate, JIT **and** AOT).

## [Unreleased] — Roadmap v13 "concurrency" (Phases 75–80, in progress)

Theme: make concurrency SAFE BY CONSTRUCTION — typed channels that move data
between threads, with thread-safety enforced *through the effect system* (the
language's differentiator). Designed via a 3-proposal / 3-judge multi-agent
panel (MVP-first won, grafting the structural Send/Sync rule + an `Rc` negative
witness).

### Added
- The **`share` effect** (Phase 75) — the concurrency effect that makes
  thread-safety a CHECKED property rather than a library convention.
  `thread_spawn` now carries `share`, so a fn that spawns must declare
  `! { share }`. Because `share` is a built-in effect it rides the existing
  effect-SUBSET rule: a trait method declared without `share` can NEVER have an
  impl that spawns, so concurrent work can't be smuggled past a pure-looking
  `<T: Task>` / `&dyn Task` interface (the super-effecting impl is rejected).
  This is the value-crossing *control* half; the value-*safety* half (only
  `Send` data crosses) lands with channels in Phase 77.
- **Typed MPSC channels** (Phase 76) — `channel() -> (Sender<i64>,
  Receiver<i64>)`, `chan_send` / `chan_recv` (→ `Option`, `None` once closed
  AND drained) / `chan_close`. The runtime is an unbounded linked-list queue
  guarded by a **pthread mutex + condition variable** (`chan_recv` blocks on
  the condvar while the channel is empty and open; `chan_close` broadcasts to
  wake blocked receivers). A producer thread sending 1..=100 and the main
  thread draining sums to exactly 5050, deterministic across runs, JIT and
  AOT. `Sender`/`Receiver` are named generic structs that lower to a Copy i64
  handle, so a `Sender` (multi-producer, `Send`) is captured by value into a
  worker thread — while a `Receiver` is the single-consumer endpoint and is
  **not** `Send`: moving one into a thread is a compile error.
- **Generic channels + the `Send` rule** (Phase 77) — `channel<T>` now MOVES a
  real `T` across threads (the queue node carries a `T`-sized cell, specialized
  per `T`), so an owned `String` or `Vec<i64>` is sent from one thread and
  received on another with ownership transferring sender → node → receiver,
  freed exactly once (no clone, no double-free). The structural **`Send`**
  predicate (`isSend`) gets teeth at `chan_send`: scalars / `String` / owning
  aggregates / the channel `Sender` are `Send`, while a `&T` borrow, the
  `Receiver`, and (Phase 78) `Rc` are not — sending a non-`Send` value on a
  channel is a compile error, so no borrow can dangle across a thread.
- **`Rc<T>`** (Phase 78) — a non-atomic reference-counted shared owner
  (`rc_new` / `rc_clone` / `rc_get` / `rc_strong_count`), a pointer to a heap
  `{ i64 strong, T value }`. The strong count tracks clones; the shared value
  and the block are dropped EXACTLY once when the last `Rc` drops (verified
  drop-once over a `Drop`-counted inner; 200k clone+drop pairs stay flat). It
  is the **legible non-`Send` witness**: its refcount is non-atomic, so an
  `Rc` may not cross a thread boundary (sending one on a channel is a compile
  error that names `Rc`) — the contrast to sharing safely via a `Mutex`.
- **The parallelism payoff + `chan_try_recv`** (Phase 79) — the v13 primitives
  compose into real fork-join parallelism: split 0..N across W worker threads,
  each summing its range and sending the partial on a SHARED `Sender`
  (multi-producer), with the main thread gathering the W partials over the
  MPSC channel (W producers → 1 consumer) and folding — deterministic, JIT and
  AOT. Plus `chan_try_recv` — a non-blocking receive (`Some` if ready, `None`
  if momentarily empty, never blocks on the condvar) for poll loops.

## [0.12.0] — Roadmap v12 "real stdlib" (Phases 69–74)

Theme: turn a language you can *compute* in into one you can *get data in and
out of* — parsing, richer collections, string and numeric methods. The second
step toward production use. A pre-merge adversarial multi-agent review fixed a
`parse_int` integer-overflow and a discarded-owned-temporary leak the green
suite had missed (see Fixed).

### Added
- **String → number parsing** (Phase 69): `parse_int(&String) -> Option<i64>`
  and `parse_f64(&String) -> Option<f64>` — the all-or-nothing parse a real
  stdlib needs (a string that is not *wholly* a valid number, including one
  with leading/trailing junk or whitespace, is `None`). Built on low-level
  `str_parse_i64` / `str_parse_f64` out-param primitives (C `strtoll`/`strtod`
  over a transient stack buffer, with strict full-consume + no-leading-
  whitespace validation). Plus `int_to_hex(i64) -> String` (lowercase hex, the
  two's-complement pattern for a negative). Reading data no longer needs a
  hand-rolled digit loop.
- **Vec mutation + query** (Phase 70): `vec_pop` / `vec_remove` / `vec_insert`
  / `vec_reverse` (built-ins) and `vec_contains` / `vec_index_of`
  (`Eq`-bounded prelude scans; index −1 when absent). `vec_pop` and
  `vec_remove` MOVE the element out (the length is decremented so the Vec no
  longer owns that slot — no clone, no double-free, the dual of the cloning
  `vec_get`), so they are sound for a non-Copy element type (`Vec<String>`).
  `vec_insert` grows when full and clamps its index to `[0, len]`.
- **HashMap / HashSet enumeration + membership** (Phase 71):
  `hashmap_contains(&HashMap, &K) -> bool` and `hashmap_values(&HashMap) ->
  Vec<V>` (`Eq`+`Clone`-bounded prelude scans over `hashmap_get_ref` /
  `hashmap_keys`, deep-cloning the values), plus `hashset_items(&HashSet) ->
  Vec<T>` — the first way to enumerate a `HashSet` (a codegen built-in
  delegating to the backing map's keys). `hashmap_remove` / `hashset_remove`
  are a deliberate deferral (open-addressing deletion needs tombstone-aware
  get/insert).
- **String methods** (Phase 72): `str_starts_with` / `str_ends_with` /
  `str_contains` / `str_index_of` (pure reads, substring index or −1) and
  `str_to_upper` / `str_to_lower` / `str_concat` / `str_repeat` (fresh heap
  Strings). All kardashev prelude functions over `str_char_at` / `str_len` /
  `str_push_byte` — high-level string manipulation without a manual char loop.
- **Numeric + math helpers** (Phase 73): integer `i64_abs` / `i64_min` /
  `i64_max` / `i64_pow` (prelude) and the f64 math `f64_sqrt` / `f64_floor` /
  `f64_ceil` / `f64_abs` (built-ins lowering to LLVM float intrinsics; the AOT
  link now pulls in `-lm`), plus more Option/Result inspectors
  (`option_is_some`, `option_ok_or`, `result_is_ok`). A real-number program no
  longer needs its own FFI declaration of libm.
- **Capstone** `examples/csvstats` (Phase 74) — "the real stdlib, applied": a
  CSV statistics aggregator that READS data (the thing v11 could not do),
  grouping `category,value` rows and reporting per-category count + sum + the
  running global max in sorted order. Exercises the whole v12 line at once —
  `parse_int` (with an `Option`-driven skip of a malformed row), `str_split`,
  HashMap aggregation, `i64_max`, `sort`, and `int_to_string` + `str_concat`
  formatting.

### Fixed
- A pre-merge adversarial multi-agent review found + fixed two MAJORs the green
  smoke suite had missed — both pinned by `tests/smoke_test_v12_review.sh`:
  - `parse_int` of a value PAST the `i64` range returned a silently-clamped
    `Some(i64::MAX/MIN)` instead of `None` (C `strtoll`'s `ERANGE` was
    unchecked). It now clears `errno` and rejects on `ERANGE`; `i64::MAX` /
    `i64::MIN` themselves still parse. (`parse_f64` keeps `strtod`'s
    overflow-to-`inf` — a valid `f64` parse, like Rust.)
  - a DISCARDED owned temporary leaked: a value moved out by
    `vec_remove(&mut v, 0);` (or any call result like `int_to_string(n);`) used
    as an expression-STATEMENT was never dropped, orphaning its heap. The
    codegen now drops a discarded droppable call-result via an entry-block temp
    — exactly once (the drop / dropleaks / soundness suites confirm no
    double-free).

## [0.11.0] — Roadmap v11 "real machine integers" (Phases 63–68)

Theme: the **numeric tower** — make kardashev practical by giving it real
machine integers (sized + unsigned + f32, `as` casts, bit ops, defined overflow)
instead of i64-only. The first step toward production use. A pre-merge
adversarial multi-agent review hardened a const-evaluation width/sign cluster
(including an invalid-IR blocker) plus two parser/lexer bugs the green suite had
missed (see Fixed).

### Added
- Sized SIGNED machine integers `i8` / `i16` / `i32` (Phase 63) — `i64` stays
  the default. The `Int` type carries a bit width + signedness; codegen lowers
  to the matching LLVM width (`i32 @add(i32, i32)`, not i64). The lattice is
  NON-coercive: no implicit widening (`i32` + `i64` is a type error — `as`
  bridges, Phase 65), and an out-of-range literal for a narrow width is a
  compile error. An unsuffixed literal is i64 by default and narrows to a
  concrete width in context (`let x: i32 = 5`); the type system carries zero
  literal churn (all v10 i64 programs are byte-for-byte unchanged).
- Integer-literal **width suffixes** and **radix prefixes** (Phase 64). A
  suffixed literal `5i32` *is* an `i32` with no annotation (it does not narrow,
  it has that concrete type), so `add(5i32, 3i32)` type-checks against an `i32`
  parameter directly; an out-of-range suffixed literal (`200i8`) is a compile
  error. Hexadecimal `0xFF` and binary `0b1010` literals parse to their value
  (default `i64`), compose with a suffix (`0xFFi32`), and work in `match`
  patterns (`0xFF => …`). Unsigned suffixes (`u8`..`u64`) are parsed and
  rejected with a clear "arrives in a later phase" diagnostic until Phase 66
  lands unsigned integers — never silently mis-typed.
- The **`as` cast operator** (Phase 65) — the only bridge across the
  non-coercive lattice. `operand as Type` converts between any two numeric
  types (an int of any width/signedness, or `f64`): integer widen (`sext`),
  narrow (`trunc`), and `int`↔`f64` (`sitofp` / `fptosi`, truncating toward
  zero), lowered to the width/signedness-correct LLVM cast. A cast is the only
  way to add an `i32` to an `i64` (`a as i64 + b`). `as` binds tighter than
  every binary operator but looser than a prefix unary (`-x as i32` is
  `(-x) as i32`, `a as i32 * 2` is `(a as i32) * 2`) and chains left-to-right
  (`x as i32 as i64`). An `int`→`int` cast is const-foldable and wraps with
  two's-complement semantics (`300 as i8` == 44) identically at compile time
  and run time. Casting from/to a non-numeric type (a struct, `bool`, String,
  reference) is a compile error.
- **Unsigned integers** `u8` / `u16` / `u32` / `u64` and the integer **bitwise
  operators** `& | ^ << >> ~` (Phase 66). Each unsigned type is a distinct
  non-coercive type (`u32` ≠ `i32`; `as` bridges), and codegen lowers its
  division, remainder, ordering comparison, and right-shift to the UNSIGNED
  opcode (`udiv` / `urem` / `icmp u…` / `lshr`) — a signed right-shift stays
  arithmetic (`ashr`). A `u64` literal past `i64::MAX` (e.g. the FNV-1a offset
  basis `0xcbf29ce484222325`) parses, and a wrapping `u64` multiply yields the
  textbook hash. Bitwise operators work on any integer width/signedness, fold
  in const expressions, and are rejected on `f64`. The `&` and `|` tokens are
  position-disambiguated (prefix `&` is still a borrow, a primary `|…|` is
  still a closure; infix they are bitwise-and / bitwise-or), and `<<` / `>>`
  are parsed by token adjacency so nested generics `Vec<Vec<T>>` stay
  unambiguous. Operator precedence now matches Rust: `&&` < comparison < `|` <
  `^` < `&` < shift < `+ -` < `* / %`.
- The **`f32`** single-precision float and **defined overflow semantics**
  (Phase 67). `f32` is a real type lowering to LLVM `float` (`f64` stays the
  default `double`); it is a distinct non-coercive type (`f32` ≠ `f64`), so an
  `as` cast bridges them with `fpext` (`f32`→`f64`) / `fptrunc` (`f64`→`f32`),
  an unsuffixed float literal is `f64` by default and narrows to `f32` in
  context, and `1.5f32` pins the width. Integer overflow is now DEFINED as
  two's-complement **wrapping** at every width (`127i8 + 1 == -128`,
  `255u8 + 1 == 0`), identically at compile and run time. Negative narrow-int
  literals narrow in context — `let x: i8 = -128` (i8::MIN) is valid even
  though `+128` would not fit, while `let x: u8 = -1` is a compile error.
- **Capstone** `examples/checksum` (Phase 68) — "the numeric tower, applied":
  three textbook algorithms written in kardashev, each checked against its
  known answer. **FNV-1a** (64-bit) uses a `u64` offset basis past `i64::MAX`
  (`0xcbf29ce484222325`) and a wrapping `u64` multiply; **CRC-32** (IEEE) uses
  a `u32` with a logical `>>`, the bitwise ops, and a branchless mask built by
  wrapping subtraction (`0 - (crc & 1)`); a **binary parser** assembles `u16`
  / `u32` from raw `u8` bytes with shifts and casts in both byte orders. Each
  routine is generic over its input length with a const-generic `[u8; N]`,
  integrating the v10 const-generic line with the whole v11 numeric tower —
  none of it is expressible in an i64-only language.

### Fixed
- A pre-merge adversarial multi-agent review hardened a cluster the green smoke
  suite had missed — every one with a verified repro, now pinned by
  `tests/smoke_test_v11_review.sh`:
  - **(blocker)** a narrow / unsigned `const` flowed into a narrow slot as a
    64-bit immediate — invalid LLVM IR (`call i32 @id(i64 7)`) / verifier
    crash. Codegen now emits a folded const at the const-reference's resolved
    int width.
  - a sized / unsigned `const`'s folded value disagreed with the same
    expression at run time — an unsigned `>>` folded as an arithmetic shift, a
    narrow result was not wrapped to its width (`100i8 + 100i8` → 200 at const
    time vs −56 at run time), and `1i32 << 31` silently held 2147483648 in an
    `i32`. The const evaluator now wraps every result to its expression-type
    width (two's-complement), so an unsigned `>>` is logical and every sized
    const folds identically to run time.
  - a plain-literal narrow / unsigned `const` (`const C: i32 = 100`) was
    rejected though the identical `let` was accepted — `const` now narrows its
    initializer like any other coercion site.
  - `expr as Type << ..` / `expr as Type < ..` was a parse error — the cast's
    target type greedily consumed the `<` / `<<` as a generic-argument list.
    A cast now parses only a bare (numeric) target, leaving the operator for
    the expression parser.
  - an integer/float width suffix was absorbed in tuple-index position
    (`t.0i32` silently became `t.0`) — the suffix is no longer taken after a
    `.`.

## [0.10.0] — Roadmap v10 "sized and sound at compile time" (Phases 57–62)

Theme: **sized and sound at compile time** — const-generic type params + the
effect system's last soundness floor. A pre-merge adversarial multi-agent review
hardened 5 blockers + 5 majors the green smoke suite had missed (see Fixed).

### Added
- Const-generic parameters parse and bind: `const N: i64` (mixed with type
  params), a symbolic `[i64; N]` array length, and the `let (a, b): (T, T) = ..`
  tuple-pattern annotation (Phase 57 — declaration shell only).
- Monomorphization over a const VALUE (Phase 58): `Mat<3>` and `Mat<5>` become
  DISTINCT LLVM struct types (`{ [3 x i64] }` vs `{ [5 x i64] }`, mangled
  `Mat__c3` / `Mat__c5`), incl. nested `Matrix<R, C>` over `[[i64; C]; R]`. The
  const value substitutes the symbolic array length; a struct literal infers
  each `const N` from the dimensions of the field that carries it
  (`Mat { data: [1,2,3] }` is a `Mat<3>`). Type/const argument slot mismatches,
  a const-value dimension mismatch, and negative const args are compile errors.
- Const-generic FUNCTIONS + compile-time dimension unification (Phase 59):
  `fn dot<const N>(a: [i64; N], b: [i64; N]) -> i64` infers N from the argument
  array lengths, lets `N` be used as a value in the body, and monomorphizes per
  size (`@dot__c3` over `[3 x i64]` vs `@dot__c2`). A dimension MISMATCH
  (`dot([i64;3], [i64;2])`) and a const param that appears in no argument array
  type are compile errors.

- `RingBuffer<T, const CAP>` (Phase 61): a struct generic over BOTH a type and
  a const param, with element-wise Drop and deep clone over a NON-Copy element.
  Fixed-size arrays `[T; N]` now allow non-Copy elements (String/struct/Vec/Box)
  — clone element-wise, drop element-wise; moving a non-Copy element out by
  index (`let x = a[i]`) is a compile error (clone or borrow instead). Symbolic
  const params flow through generic impls (`impl<T, const CAP> Clone for
  RingBuffer<T, CAP>`) and `derive(Clone)`. Plus closure-param INFERENCE:
  `vec_map(v, |x| *x * 2)` infers `x`'s type from the callee's fn-typed
  parameter — no `|x: &i64|` annotation needed.
- Array-repeat `[value; N]` (Phase 62) — `N` a literal, const item, or a
  const-generic param (a symbolic length).
- **Capstone** `examples/matrix` (Phase 62) — a fixed-size linear-algebra
  library: `Matrix<const R, const C>` carries its shape in the TYPE,
  `transpose() -> Matrix<C, R>` swaps the dims, and a dimension-checked
  `matmul(Matrix<R, K>, Matrix<K, C>) -> Matrix<R, C>` rejects a shape mismatch
  at COMPILE time (the shared inner dim `K` can't be two values). Integrates the
  whole v10 line: monomorphize-over-a-value, dimension unification, symbolic
  const params, non-Copy arrays, and array-repeat.

### Fixed
- A pre-merge adversarial multi-agent review (6 dimensions) hardened **5
  blockers + 5 majors** the green smoke suite had missed — every one with a
  verified repro — now pinned by `tests/smoke_test_v10_review.sh`:
  - a const param not threaded into a NESTED struct/enum field's type-args
    (`Inner<N>` field of `Outer<N>` mangled `Inner__c0` → LLVM-verifier failure);
  - a bare `b.clone()` on a const-generic struct leaving the const arg symbolic
    (mangled `c0`) → result type confusion;
  - `Drop` is no longer EXEMPT from the effect-subset rule — a `dyn Drop`
    dispatch could launder io/alloc through a pure-declared `Drop` trait;
  - a BOUNDED-generic method call (`<T: Trait>` + `t.method()`) attributed ZERO
    effects (vs the trait's declared effects) — the subset rule's actual floor;
  - forwarding a SYMBOLIC array length alongside a concrete one was accepted
    ill-typed (LLVM miscompile) and legitimate symbolic forwarding was wrongly
    rejected;
  - const-generic ENUM variant payloads (`[i64; N]`) were wrongly rejected;
  - a monomorphization name colliding with a user identifier (`g__i64`) silently
    resolved to the user fn — now a clear compile error;
  - assigning to a non-Copy array element `a[i] = x` was wrongly rejected;
  - array-repeat `[v; N]` ignored a local shadowing a const param;
  - a method-level const param leaked an internal mangled name — now a clear
    "declare it on the impl block" diagnostic.

### Changed
- The **effect-subset rule** (Phase 60), the effect system's last soundness
  floor: a trait impl method's effects must be a SUBSET of the trait method's
  declared effects, so a `dyn Trait` / `<T: Trait>` dispatch (which attributes
  the TRAIT's effects) can never under-count what an impl actually does. A
  super-effecting impl is a compile error. `Drop` is exempt (static drop glue,
  never dyn-dispatched). To make the prelude honest, `Eq::eq`,
  `Iterator::next`, `Display::to_string` and `Default::default` now declare
  `! { alloc }` (their container/heap impls allocate); a concrete `for` loop
  still attributes its concrete `next`'s effects, so pure (Range) loops stay
  pure, and `derive(Eq)` annotates `! { alloc }` only when a field's `eq`
  actually allocates (a map-/Vec-/generic-free struct's derived `eq` is pure).

## [0.9.0] — Roadmap v9 "data in motion" (Phases 51–56)

### Added
- `Box<T>` as a first-class impl target + `&*`/`**` deref ergonomics, and
  prelude `Clone`/`Eq` for `Box<T>`.
- Generic associated functions: a bounded `T::method()` (e.g. `T::default()`).
- `Vec` higher-order combinators `vec_map` / `vec_filter` / `vec_fold` over
  closures (effect-polymorphic).
- String tokenizing (`str_split`, `str_trim`) and `hashmap_entries → Vec<(K,V)>`.
- Capstone `examples/wordfreq` — a word-frequency histogram pipeline.

### Fixed
- A pre-merge adversarial review hardened 5 memory-safety / type-soundness
  holes the green smoke suite had missed (by-value container-getter double-free,
  `dyn Trait<T>` argument confusion, move-out-of-`&` via `*r`, `&mut` reborrow
  aliasing, an unjoined `if`-branch move-state) plus dyn/generic effect
  attribution — locked in by `tests/smoke_test_soundness.sh`.

## [0.8.0] — Roadmap v8 "generics, finished" (Phases 45–50)

### Added
- Bounded type params (`K: Hash + Eq`) inside container ops; prelude `Clone`/`Eq`
  trait impls for `HashMap`.
- `Ord` trait + a generic in-place `sort<T: Ord>` (+ `vec_swap`, `&mut → &`
  reborrow).
- `#[derive(Hash, Ord, Default)]` and associated functions (static
  `Type::method()`).
- `dyn Trait<T>` generic trait objects + dispatch through `Vec<Box<dyn …>>`.
- Capstone `examples/json` upgraded to JSON 3.0 — `HashMap<String, Json>`
  objects, fully `#[derive]`d, canonical sorted-key output.

## [0.7.0] — Roadmap v7 "real numbers, real abstraction" (Phases 39–44)

### Added
- `f64` floating point.
- Generic `impl<T: Bound>` blocks; generic `Clone`/`Eq` over containers;
  `#[derive(Clone, Eq, Display)]`.
- Runtime string escapes; the last async-frame leak closed.
- Capstone JSON 2.0 — floats + decoded escapes + derived `Clone`/`Eq`.

## [0.6.0] — Roadmap v6 "make the heap recursive" (Phases 33–38)

### Added
- Sound recursive heap-owning enums (`Box`/`Vec<Self>`/`HashMap<K,Self>`) with
  recursive `Drop` + deep `clone`; read-without-move + `match`-by-reference;
  enum-typed struct fields + non-Copy tuples; `Display` + de-`i64`'d iteration.
- Capstone: a full nested-JSON parser + serializer written in kardashev.

### Fixed
- An `-O1+` miscompile: the optimizer ran without the target datalayout, folding
  multi-field-aggregate reads-through-a-pointer to wrong byte offsets.

## [0.5.0] — Roadmap v5 (Phases 27–32)

### Added
- Stdlib depth (string toolkit, generic `HashMap<K,V>`), file I/O + CLI args,
  `Drop`-leak fixes, and self-written capstones (`examples/calc`,
  `examples/rpn`). Docs + a source-comment truth pass.

## [0.4.0] — Roadmap v4 (Phases 21–26)

### Added
- Generic trait parameters + associated types + `where` clauses; fixed-size
  arrays `[T; N]` + tuples `(A, B)`; compile-time `const` items + const
  evaluation (incl. const-generic array lengths); `extern "C"` FFI; an
  arithmetic-interpreter capstone written in kardashev.

## [0.3.0] — Roadmap v3 (Phases 15–20)

### Added
- Expression & item completeness (bool/unary ops, inherent impls); deterministic
  memory management — `Drop`/RAII with runtime drop flags; real panic + unwinding
  with cleanup; OS threads + `Mutex`; opt-level flags + the `kardc --test` runner.

## [0.2.0] — Roadmap v2 (Phases 9–14)

### Added
- Iteration (loops, ranges, `for`); closures + effect-carrying function types
  (first-class fn values, `FnMut` captures); `dyn Trait` dynamic dispatch; a
  growable stdlib (`String`, `HashMap`, `&[T]` slices, `map`/`filter`/`fold`
  combinators, `Option`/`Result` combinators); the source formatter (`kardfmt`)
  and richer LSP.

## [0.1.0] — Roadmap v1 (Phases 0–8)

### Added
- The MVP and foundation: the full pipeline (lexer → parser → Hindley-Milner
  type inference → LLVM IR → ORC JIT + AOT); ownership + non-lexical-lifetime
  borrow checking; ADTs + pattern matching; traits + generics + monomorphization;
  `Result` + the `?` operator; **effect labels** in signatures (the signature
  feature) with effect-row polymorphism; a minimal stdlib (`Option`/`Result`/
  `Vec`/`String`) + AOT pipeline; `async`/`await` + a single-thread executor;
  the module system + `kard` CLI + `rules_kardashev` Bazel rules; `-O0..-O3`
  pass pipelines + the `kard-lsp` language server.
