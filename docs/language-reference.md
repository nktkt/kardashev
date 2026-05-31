# kardashev Language Reference

The surface language as it compiles **today**. [`../README.md`](../README.md)
and [`../ROADMAP.md`](../ROADMAP.md) are the authoritative record of everything
that has shipped; this document is the practical reference for the core
constructs and operators. Every snippet here compiles under `kardc` as written.
Features still on the runway are not described here.

A note on honesty: kardashev has a handful of deliberate surface limitations
(an `if` used as a value needs an `else`; a `&mut` parameter is not
auto-reborrowed through recursive calls; strings carry no NUL terminator).
These are real, they are called out below, and the examples obey them — they
are not hidden. Earlier drafts of this reference also listed `&&`/`||`, `%`,
`&` of a temporary, and enum-typed struct fields as missing; all four now work
(Phases 33, 36, 124, 125) and the *Surface limitations* section records the
correction.

## Lexical structure

| Token              | Notes                                                       |
|--------------------|-------------------------------------------------------------|
| Identifier         | `[A-Za-z_][A-Za-z0-9_]*`                                    |
| Integer literal    | `[0-9]+`                                                    |
| Boolean literal    | `true` / `false`                                            |
| String literal     | `"..."` with `\n \t \r \\ \"` escapes                       |
| Operators          | `+ - * / % < <= > >= == != = -> => ? ! && \|\| & \| ^ << >>` |
| Punctuation        | `( ) { } [ ] , ; : :: . _ & .. ..=`                         |
| Keywords           | `fn let if else return struct enum match trait impl for`    |
|                    | `mod pub const extern while loop break continue true false`  |

`%` (modulo), `&&` and `||` (short-circuit boolean operators), and the integer
bitwise operators (`& | ^ << >>`) are all supported. `||` is disambiguated
positionally: after an operand it is logical-or, while at the head of an
expression `|| expr` it is still a zero-parameter closure. `&&` binds tighter
than `||`, and both bind looser than the comparisons.

`async`, `await`, `mut`, `dyn`, `where`, and `self` are recognized
positionally rather than as reserved words, so they can also appear as plain
identifiers (e.g. inside effect rows or as generic parameter names).

Comments are `// to end of line`. No block comments.

## Functions

```rust
fn add(a: i64, b: i64) -> i64 { a + b }                 // pure: no effect row

fn log_add(a: i64, b: i64) -> i64 ! { io } {            // declares the io effect
    print(a + b);
    a + b
}
```

A function declares parameters, a `-> ReturnType`, and an optional **effect
row** `! { ... }` after the return type. Omitting the row means *pure* (the
empty row). The last expression of the block is the return value; `return e;`
is also available. Callers must declare every effect they transitively use —
a pure function calling `log_add` is a type error. The effect row may carry a
**row variable** to be effect-polymorphic:

```rust
fn map<T, U, e>(xs: Vec<T>, f: fn(T) -> U ! {e}) -> Vec<U> ! { e, alloc } {
    let mut out = vec_new();
    for x in xs { vec_push(&mut out, f(x)); }
    out
}
```

The built-in effect labels are `pure` (empty), `alloc`, `io`, `panic`,
`async`, and `unwind`. See [effects.md](effects.md).

## let / let mut / assignment

```rust
let x = 5;            // immutable binding
let mut n = 0;        // mutable binding
n = n + 1;            // assignment (only to a `let mut`)
let y: i64 = 7;       // optional type annotation
```

A plain `let` binding cannot be reassigned. `let mut` permits later `name =
expr;` assignment. **Tuple-destructuring `let` does not bind `mut`** — dest
into immutable names, then seed a `let mut` if you need to mutate (the calc
capstone does exactly this when folding).

## if / else — `if` is an expression and REQUIRES an `else`

`if` is an **expression**, and the `else` arm is **mandatory**. A bare `if c {
... }` with no `else` is a parse error. Either supply both arms, or write an
explicit empty else `else {}` when you only want a side effect:

```rust
let m = if a > b { a } else { b };          // if as a value

if done {
    cleanup();
} else {}                                    // side-effect-only: empty else REQUIRED
```

`else if` ladders are supported and are the idiomatic substitute for the
missing `match` on integers:

```rust
fn sign(x: i64) -> i64 {
    if x > 0 { 1 } else if x < 0 { 0 - 1 } else { 0 }
}
```

Unary `-x` (negation) and `!x` (logical not on `bool`) are available; both
bind tighter than the binary operators. (For a negative literal you can also
write `0 - 7`, which the capstones use.)

Operator precedence, low to high: comparisons (`< <= > >= == !=`), then
additive (`+ -`), then multiplicative (`* /`), then unary (`- !`), then
postfix (`.field`, `.method(..)`, `[i]`, `?`, `.await`).

## match + patterns

```rust
enum Color { Red, Green, Blue }

fn code(c: Color) -> i64 {
    match c {
        Red   => 0,
        Green => 1,
        Blue  => 2,
    }
}
```

`match` binds variant payloads and supports a `_` wildcard:

```rust
fn unwrap_or(o: Option<i64>, d: i64) -> i64 {
    match o {
        Some(x) => x,        // binds the payload as `x`
        None    => d,
    }
}
```

Arm bodies may be a single expression or a `{ ... }` block; arms are
comma-separated. Matching is exhaustive (a `_` arm covers the rest). Patterns
match enum constructors, literals, bindings, and `_`. (Tuple patterns inside a
`match` are not supported — destructure a tuple with `let (a, b) = t;`
instead.)

## Enums and structs

```rust
enum Shape { Circle(i64), Rect(i64, i64), Unit }   // variants, some with payloads

struct Point { x: i64, y: i64 }                     // named fields
```

Construct a struct with a **struct literal**; read fields with `.` (which
auto-derefs through `&T` / `&mut T`):

```rust
fn main() -> i64 {
    let p = Point { x: 3, y: 4 };
    p.x + p.y                                       // 7
}
```

Enums are generic (`enum Option<T> { Some(T), None }`) and so are structs
(`struct Pair<A, B> { first: A, second: B }`).

A struct field may itself be enum-typed (`struct Holder { t: Tree, tag: i64 }`
where `Tree` is an enum). Read such a field by reference and match on it —
`match &h.t { ... }` (see `smoke_test_phase36`). The older `i64`-*code* idiom
(store a code in the field, lift it to the enum at the boundary via a small
function) still appears in the calc and kdlex capstones and remains valid:

```rust
const K_NUM: i64 = 0;
const K_PLUS: i64 = 1;
struct Tok { kind: i64, val: i64 }                  // i64 code; a TokKind field also works

enum TokKind { Num, Plus, Other }
fn kind_of(k: i64) -> TokKind {
    if k == K_NUM { TokKind::Num }
    else if k == K_PLUS { TokKind::Plus }
    else { TokKind::Other }
}
```

## Traits and impls

A `trait` declares method signatures; `impl Trait for Type` supplies them.
`impl Type { ... }` (no `for`) defines **inherent** methods on a type. Both
resolve through the same method table; receivers autoref as `&self` /
`&mut self`.

```rust
trait Show { fn show(&self) -> i64; }
struct Point { x: i64, y: i64 }

impl Show for Point { fn show(&self) -> i64 { self.x + self.y } }  // trait impl
impl Point { fn origin_dist(&self) -> i64 { self.x + self.y } }    // inherent impl
```

### Generics with bounds

A generic parameter may carry a single trait bound, or **multiple bounds with
`+`** (Phase 28):

```rust
fn use_show<T: Show>(t: T) -> i64 { t.show() }          // single bound

fn keyed<K: Hash + Eq>(k: K) -> i64 { k.hash() }        // multiple bounds: K: A + B
```

`where` clauses are accepted and desugar to the inline-bound form (identical
downstream):

```rust
fn head<T, C: Container<T>>(c: C) -> i64 where T: Show { c.first().show() }
```

### Associated types

A trait may declare an associated type with `type Item;`, referenced as
`Self::Item` in the trait and `C::Item` at a bounded call site:

```rust
trait Container { type Item; fn first(&self) -> Self::Item; }
```

### Trait objects: `dyn Trait` and `Box<dyn Trait>`

A trait object is a `{data, vtable}` fat pointer. A single call site through
`&dyn Trait` (or `Box<dyn Trait>`) dispatches to multiple runtime impls via
the vtable. Object safety is enforced (a trait with a static, no-`self` method
is rejected as a `dyn`):

```rust
trait Shape { fn area(&self) -> i64; }
impl Shape for Sq   { fn area(&self) -> i64 { self.side * self.side } }
impl Shape for Rect { fn area(&self) -> i64 { self.w * self.h } }

fn describe(s: &dyn Shape) -> i64 { s.area() }          // one site, many impls
```

`Box<dyn Shape>` holds a heap-owned trait object; `.area()` dispatches the same
way. (Static dispatch via `<T: Shape>` is an unchanged separate path —
monomorphized, no vtable.)

## Closures and function-value types

A closure is `|params| body`; it may capture surrounding bindings (lowered to
a heap env + a uniform fat-pointer fn-value):

```rust
fn main() -> i64 {
    let n = 10;
    let add_n = |x| x + n;        // captures n by value
    add_n(5)                      // 15
}
```

A function-value **type** carries its own effect row:
`fn(T) -> U ! {e}`. The row variable `e` makes a higher-order function
effect-polymorphic — calling it with a pure closure stays pure, with an `io`
closure carries `io`:

```rust
fn apply(f: fn(i64) -> i64 ! {e}, x: i64) -> i64 ! {e} { f(x) }
```

## async fn / .await

An `async fn` returns the built-in `Future<T>`; `.await` suspends until the
future is ready and unwraps it. Awaiting carries the `async` effect.

```rust
async fn add(a: i64, b: i64) -> i64 { a + b }
async fn double(n: i64) -> i64 { add(n, n).await }

fn main() -> i64 ! { async, io } {
    print(block_on(double(21)));   // drives the future to completion -> 42
    0
}
```

A real single-threaded executor backs this: `spawn(f)` enqueues a task
(returns an i64 handle), `block_on(f)` / `join(h)` drive the queue, `yield_now`
suspends once, and `sleep_ms(n)` is a real `CLOCK_MONOTONIC` timer leaf (the
reactor sleeps rather than hot-spins when all tasks are pending). Linux/`epoll`
fd-readiness ships (`pipe_make`/`pipe_send`/`read_pipe`); **macOS/kqueue
fd-readiness is a documented deferral** (timers work cross-platform).

**Documented leak:** async-frame *interior* values are not yet freed on frame
teardown (no use-after-free — it leaks). See README Phase 16/29.

## Arrays, tuples, and destructuring

Fixed-size arrays `[T; N]` are stack value-aggregates; tuples `(A, B, ...)` are
anonymous structs:

```rust
fn sum3(a: [i64; 3]) -> i64 { a[0] + a[1] + a[2] }      // array param + indexing

fn main() -> i64 {
    let mut a = [10, 20, 30];     // array literal
    a[1] = 99;                    // element assignment
    let t = (1, 2);               // tuple literal
    let (x, y) = t;               // tuple destructuring (binds immutable)
    a[0] + t.0 + t.1 + x + y      // .0 / .1 field access
}
```

A dynamic out-of-bounds array index **panics** (Phase 23). (Tuple `match`
patterns and non-`Copy` array elements are not supported — destructure tuples
with `let`.)

## const / const fn / const-generic array lengths

```rust
const LIMIT: i64 = 5;                       // i64/bool const, folded at every use
const fn sq(n: i64) -> i64 { n * n }        // runs at compile time in a const context

fn main() -> i64 {
    let a: [i64; sq(2)] = [0, 0, 0, 0];     // const-generic length: N = sq(2) = 4
    a[0] + LIMIT
}
```

A `const` item is evaluated at compile time and folded to a literal at each
use (verifiable in `--emit-llvm`: no runtime load). A `const fn` runs at
compile time when called in a const context with constant args, and is *also*
an ordinary runtime function. The array length `N` in `[T; N]` may be any
const-expression — a `const` item, a `const fn` call, or arithmetic over them.
Integer overflow / div-by-zero in const evaluation are compile errors.
(`const` types are limited to `i64` / `bool`; full const-generic *type
parameters* like `struct Arr<const N: i64>` are not in scope.)

## extern "C" FFI

Declare an external C function with `extern "C" fn name(args) -> T;` (a block
form `extern "C" { ... }` also parses). It lowers to an unmangled LLVM extern +
a direct call; the JIT resolves it from the host process, AOT links via
`clang`. The spelling `i32` maps to C `int` (trunc/sext at the boundary), and
`&String` / `&[T]` map to a C pointer. An extern call carries the `io` effect
unless the declaration gives it an explicit `! { }` row.

```rust
extern "C" fn abs(x: i32) -> i32;
extern "C" fn strlen(s: &String) -> i64;

fn main() -> i64 ! { io } {
    let s = "hello";
    abs(0 - 7) + strlen(&s)        // 7 + 5 = 12
}
```

(Importing C is what ships; an export-to-C attribute is deferred.)

## References, borrowing, and NLL

References are `&T` (shared) and `&mut T` (unique). The borrow checker is
Rust-style affine ownership with **non-lexical lifetimes** — a borrow is live
only up to its last use, so a value can be moved after its references go dead:

```rust
fn read(p: &Point) -> i64 { p.x + p.y }

fn main() -> i64 {
    let p = Point { x: 3, y: 4 };
    let r = &p;
    let a = read(r);              // r's last use; the borrow is now dead
    let b = consume(p);           // OK to move p now — NLL allows it
    a + b
}
```

**Limitation — no `&` of a literal or temporary.** `&"x"` or `&Foo { .. }` is
rejected. Bind to a `let` first, then borrow the binding:

```rust
let s = "hello";
print_str(&s);                    // &s, not &"hello"
```

**Limitation — a `&mut` parameter is passed by move and is not
auto-reborrowed** when threaded through recursive calls. Passing `&mut v` of a
*local* at a call site is fine (`vec_push(&mut toks, t)`); but you cannot thread
a `&mut self`-style parameter down a recursive descent. The calc capstone works
around this by threading its cursor + error through return tuples instead.

## Drop / RAII

A type that implements `trait Drop` (and the built-in `Vec` / `String` /
`HashMap` / `Box` glue) is dropped deterministically at scope exit, in reverse
declaration order, driven by the NLL move analysis. Runtime drop flags ensure
a conditionally-moved value drops exactly once (no double-free, no
use-after-free). Moved or returned values are not dropped by the source scope.
A 2 M-iteration allocating loop runs in constant ~1.5 MB RSS.

```rust
struct Guard { id: i64 }
impl Drop for Guard { fn drop(&mut self) -> i64 { print(self.id) } }

fn main() -> i64 {
    let a = Guard { id: 1 };
    let b = Guard { id: 2 };
    0                             // at scope exit: b drops, then a (reverse order)
}
```

(Closure-env and async-frame *interior* contents are a documented leak — no
UAF; see README Phase 29.)

## Built-in functions and prelude types

Auto-included prelude (a user definition of the same name suppresses the
prelude one):

```rust
enum Option<T> { Some(T), None }
enum Result<T, E> { Ok(T), Err(E) }
trait Iterator<T> { fn next(&mut self) -> Option<T>; }   // impl'd for the built-in Range
trait Hash { fn hash(&self) -> i64; }                    // impls for i64, String
trait Eq   { fn eq(&self, other: &Self) -> bool; }       // impls for i64, String
```

### Collections

| Type / function                                              | Notes                              |
|--------------------------------------------------------------|------------------------------------|
| `vec_new() -> Vec<T> ! { alloc }`                            | empty growable buffer (per-`T`)    |
| `vec_push(v: &mut Vec<T>, x: T) -> i64 ! { alloc }`          | append (may realloc)               |
| `vec_get(v: &Vec<T>, i: i64) -> T`                           | index; returns a **shallow copy**  |
| `vec_len(v: &Vec<T>) -> i64`                                 | element count                      |
| `string_new() -> String ! { alloc }`                        | empty growable string              |
| `string_push_str(s: &mut String, other: String) -> i64`     | append (`! { alloc }`)             |
| `hashmap_new() -> HashMap<K, V> ! { alloc }`                 | `K: Hash + Eq`; i64 + String keys  |
| `hashmap_insert(m: &mut HashMap<K,V>, k: K, v: V) -> i64`    | `! { alloc }`                      |
| `hashmap_get(m: &HashMap<K,V>, k: K) -> Option<V>`           | lookup                             |
| `hashmap_len(m: &HashMap<K,V>) -> i64`                       | entry count                        |
| `hashset_new() / _insert / _contains / _len`                 | `HashSet<T>`, `T: Hash + Eq`       |

`String` is `{ ptr, len, cap }` with **no NUL terminator**. `Vec<T>` is heap
backed with a `DataLayout`-sized stride (works for `i64`, `bool`, structs,
enums). `vec_get` returns a shallow copy of the element.

### Strings and I/O

| Function                                                | Notes                               |
|---------------------------------------------------------|-------------------------------------|
| `print(n: i64) -> i64 ! { io }`                         | one integer + newline               |
| `print_str(s: &String) -> i64 ! { io }`                 | a string, no newline                |
| `println(s: &String) -> i64 ! { io }`                   | a string + newline                  |
| `print_no_nl(s: &String) -> i64 ! { io }`               | a string, no newline                |
| `str_len(s: &String) -> i64`                            | byte length                         |
| `str_char_at(s: &String, i: i64) -> i64`                | byte at `i` (0..255), or -1 past end |
| `str_eq(a: &String, b: &String) -> bool`                | byte-wise equality                  |
| `str_substring(s: &String, start: i64, len: i64) -> String ! { alloc }` | owned byte slice    |
| `int_to_string(n: i64) -> String ! { alloc }`           | decimal formatting                  |

### File I/O + CLI args (Phase 30)

These are injected lazily — a program that never mentions them carries none of
the machinery. They return `Result<_, IoError>` and carry `io`:

```rust
fn main() -> i64 ! { io, alloc } {
    let path = "config.txt";
    match fs_read_to_string(&path) {
        Ok(s)  => str_len(&s),
        Err(e) => 0 - 1,
    }
}
```

`fs_read_to_string(&String) -> Result<String, IoError>`,
`fs_write(&String, &String) -> Result<i64, IoError>`, `fs_exists`, and
`args() -> Vec<String>` are available. `enum IoError { IoNotFound,
IoPermissionDenied, IoOther }`. Verified on Linux; macOS rides CI.

### Concurrency

`thread_spawn` / `thread_join` run a kardashev fn-value on a real `pthread`;
`Mutex` (pthread-backed) guards shared state. **Data-race floor (enforced):**
`thread_spawn` rejects at compile time any closure that captures a binding *by
reference* — captures must be by value or shared via a `Mutex` handle. (Full
`Send`/`Sync` marker traits, channels, and atomics are deferred.)

## Surface limitations (called out, not hidden)

These are real properties of the language today. Every snippet above obeys them.

1. **`if` requires an `else`.** A bare `if c { ... }` is a parse error; use
   `if c { ... } else { ... }` or `if c { ... } else {}`.
2. **A `&mut` parameter is passed by move, not auto-reborrowed** through
   recursive calls — thread state through return tuples instead. `&mut local`
   at a single call site is fine.
3. **Strings have no NUL terminator** (`{ ptr, len, cap }`); `vec_get` returns a
   shallow copy.

Four items that earlier editions listed here have since shipped and are **no
longer limitations**: `&&` / `||` short-circuit boolean operators (`&&` Phase
33, `||` Phase 124 — `&&` binds tighter), `%` modulo (Phase 33), `&` of a
literal or temporary (Phase 125 — `&5` and `&Foo { .. }` materialize a
statement-scoped, dropped slot), and enum-typed struct fields (Phase 36).

## Modules

`mod foo;` at the top of a `.kd` file pulls in `foo.kd` from the same directory
and flat-merges its declarations (recursive, cycle-safe). `pub fn` gates
path-qualified references (`foo::bar`) across module boundaries; bare-name
references resolve through the flat merge.

```rust
// util.kd
pub fn double(n: i64) -> i64 { n + n }
// main.kd
mod util;
fn main() -> i64 { util::double(21) }    // 42
```

A project may carry a `kard.toml` manifest with local-path `[dependencies]`,
resolved by `kard build` / `kard run`. (Third-party dependency resolution via
the Bazel module registry is a documented deferral — it can't be verified in
this build environment, so it is intentionally not stubbed.)

## Worked examples

- [`examples/calc/`](../examples/calc/) — a recursive-descent arithmetic
  interpreter written in kardashev (enums + `match`, structs, tuples, `Vec`,
  `const`, recursion).
- [`examples/json/`](../examples/json/) — a JSON parser into a
  `HashMap<String, i64>` (the numeric-object subset capstone: a top-level
  object of `"key": integer` members; nested objects/arrays and
  string/bool/null values are out of scope).
- [`examples/kdlex/`](../examples/kdlex/) — a lexer for a kardashev subset.
- [`examples/rpn/`](../examples/rpn/) — an RPN calculator.

## See also

- [Effects system](effects.md) — `! { io, alloc }` semantics
- [Standard library](stdlib.md) — full builtin catalog
- [Architecture](architecture.md) — compiler pipeline
