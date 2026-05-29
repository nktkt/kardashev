#include "kardashev/typecheck.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "kardashev/pattern_match.hpp"

namespace kardashev {
namespace {

class TypeChecker {
public:
    TypeCheckResult check(const ast::Program& program) {
        // Phase 6.0 built-ins: register stdlib primitives the user can
        // call without ever declaring them. The implementations live in
        // codegen (generated wrappers that call libc's printf, malloc,
        // realloc); we only commit schemas here so the typechecker
        // accepts the calls + propagates effects.

        // print(i64) -> i64 ! { io }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["print"] = std::move(sch);
        }
        // Phase 39: f64 conversions + printing.
        // to_f64(n: i64) -> f64        (signed widen, SIToFP)
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeFloat());
            fnSchemas_["to_f64"] = std::move(sch);
        }
        // float_to_int(x: f64) -> i64  (truncation toward zero, FPToSI)
        {
            FnSchema sch;
            sch.signature = makeFunction({makeFloat()}, makeInt());
            fnSchemas_["float_to_int"] = std::move(sch);
        }
        // print_f64(x: f64) -> i64 ! { io }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeFloat()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["print_f64"] = std::move(sch);
        }

        // Phase 5.z built-in: `Vec<T>` — a growable buffer with one
        // type parameter. typeArgs at use sites tell codegen which
        // element type to specialize the runtime functions for. The
        // LLVM struct layout stays uniform { i8* data, i64 len, i64 cap }
        // regardless of T; what changes is the byte-stride used by
        // push / get.
        TypePtr vecGenericVar = makeFreshVar();
        {
            StructSchema sch;
            sch.type = makeStruct("Vec", {});
            sch.genericVars.push_back(vecGenericVar);
            structSchemas_["Vec"] = std::move(sch);
        }
        TypePtr vecGenericInst = structSchemas_["Vec"].type;
        // Vec's signature carries one typeArg (the generic Var) so
        // call-site instantiation walks it the same way generic struct
        // schemas do.
        vecGenericInst->typeArgs = {vecGenericVar};

        // Phase 5.y built-in: `String` — immutable utf-8-ish byte buffer
        // backing string literals (`"..."`). Codegen emits each literal
        // as an LLVM global constant + a struct view { i8* data, i64 len }.
        {
            StructSchema sch;
            sch.type = makeStruct("String", {});
            structSchemas_["String"] = std::move(sch);
        }
        TypePtr stringTy = structSchemas_["String"].type;

        // Phase 12 built-in, Phase 17b generic: `Future<T>` — a
        // poll-function/frame pair. Codegen lays it out as { i8* poll, i8*
        // frame } where `poll(frame, &Poll<T>)` advances the future and
        // reports Ready(value) / Pending via the out-param. The result type T
        // is carried in `typeArgs[0]`; an `async fn -> T` produces
        // `Future<T>`, `.await` unwraps to T, and `block_on` returns T. The
        // struct stays nameable but opaque to user dot-access. Registered as a
        // generic schema (one type param) so `resolveTypeRef` can materialize
        // a concrete `Future<T>` and codegen treats it like Vec (single
        // layout, T surfaced per use site).
        TypePtr futureGenericVar = makeFreshVar();
        {
            StructSchema sch;
            sch.type = makeFuture(futureGenericVar);
            sch.genericVars.push_back(futureGenericVar);
            structSchemas_["Future"] = std::move(sch);
        }
        // A `Future<i64>` instance used by the i64-leaf builtins below.
        TypePtr futureI64Ty = makeFuture(makeInt());

        // Phase 9 built-in: `Range` — the iterable produced by `a..b` and
        // `a..=b`. Fields: current `start`, `end` bound, and `inclusive`
        // (0/1) so `for` knows whether `end` is part of the range. Codegen
        // lays it out as { i64, i64, i64 } and `for` lowers a counted loop
        // over it directly (the `Iterator::next` impl below is the trait-
        // level spelling of the same logic).
        {
            StructSchema sch;
            sch.type = makeStruct("Range", {{"start", makeInt()},
                                            {"end", makeInt()},
                                            {"inclusive", makeInt()}});
            structSchemas_["Range"] = std::move(sch);
        }

        // print_str(s: &String) -> i64 ! { io }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["print_str"] = std::move(sch);
        }
        // str_len(s: &String) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            fnSchemas_["str_len"] = std::move(sch);
        }
        // Phase 26: str_char_at(s: &String, i: i64) -> i64 — the byte at index
        // `i` widened to i64, or -1 when `i` is negative / past the end (a
        // bounds-checked read, mirroring vec_get's Phase 24 guard). This is the
        // one stdlib gap the calc-interpreter capstone needs: a way to read an
        // individual byte out of a String so a tokenizer can scan characters.
        // Pure (no effect) — it only reads borrowed memory.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false), makeInt()}, makeInt());
            fnSchemas_["str_char_at"] = std::move(sch);
        }

        // Phase 13b growable-String builtins. `String` is now a heap-backed
        // {ptr,len,cap}; these mirror the Vec builtin family. string_new and
        // string_push_str allocate, so they carry the `alloc` effect (any
        // caller building a string must declare `alloc`).
        //
        // string_new() -> String ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({}, stringTy);
            sch.declaredEffects.add("alloc");
            fnSchemas_["string_new"] = std::move(sch);
        }
        // string_push_str(s: &mut String, other: String) -> i64 ! { alloc }
        // `other` is by value so a string literal (a String value) appends
        // directly: `string_push_str(&mut s, "cd")`.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/true), stringTy},
                makeInt());
            sch.declaredEffects.add("alloc");
            fnSchemas_["string_push_str"] = std::move(sch);
        }
        // Phase 43: str_push_byte(s: &mut String, b: i64) -> i64 ! { alloc } —
        // append one byte (low 8 bits of b) to a String, growing as needed.
        // The low-level builder the str_escape / str_unescape prelude fns use.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/true), makeInt()}, makeInt());
            sch.declaredEffects.add("alloc");
            fnSchemas_["str_push_byte"] = std::move(sch);
        }
        // string_len(s: &String) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            fnSchemas_["string_len"] = std::move(sch);
        }
        // print_string(s: &String) -> i64 ! { io }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["print_string"] = std::move(sch);
        }

        // --- Phase 27: string toolkit ---
        // The pre-existing string ops were print_str / str_len / str_char_at
        // (Phase 26) and the heap string_* family (Phase 13b). This group adds
        // the comparison, slicing, integer-formatting, and no-newline output a
        // self-written parser/serializer needs. Note print_str/print_string
        // already force a trailing newline; the genuinely new output capability
        // here is print_no_nl (compose a line piece by piece), with println as
        // the conventional newline-terminated name.

        // str_eq(a: &String, b: &String) -> bool — byte-exact equality (length
        // then memcmp). Pure: only reads borrowed memory.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false),
                 makeRef(stringTy, /*isMut=*/false)},
                makeBool());
            fnSchemas_["str_eq"] = std::move(sch);
        }
        // str_substring(s: &String, start: i64, len: i64) -> String ! { alloc }
        // A fresh heap-owned String holding the clamped byte range
        // [start, start+len). start and len are clamped into bounds, so an
        // out-of-range request yields a shorter (possibly empty) string rather
        // than reading past the buffer.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false), makeInt(), makeInt()},
                stringTy);
            sch.declaredEffects.add("alloc");
            fnSchemas_["str_substring"] = std::move(sch);
        }
        // Phase 43: str_unescape / str_escape are kardashev PRELUDE functions
        // (defined over str_push_byte), not builtins — so they are NOT
        // registered here (that would collide with the prelude `fn`).
        // int_to_string(n: i64) -> String ! { alloc } — decimal formatting of
        // an i64 into a fresh heap String (snprintf "%lld").
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, stringTy);
            sch.declaredEffects.add("alloc");
            fnSchemas_["int_to_string"] = std::move(sch);
        }
        // Phase 44: f64_to_string(x: f64) -> String ! { alloc } — format an f64
        // into a fresh heap String (snprintf "%g"). The dual of int_to_string,
        // used to serialize JSON numbers.
        {
            FnSchema sch;
            sch.signature = makeFunction({makeFloat()}, stringTy);
            sch.declaredEffects.add("alloc");
            fnSchemas_["f64_to_string"] = std::move(sch);
        }
        // print_no_nl(s: &String) -> i64 ! { io } — writes s with NO trailing
        // newline (print_str / print_string / println all force one), so
        // output can be composed piece by piece on a single line.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["print_no_nl"] = std::move(sch);
        }
        // println(s: &String) -> i64 ! { io } — the conventional name for a
        // newline-terminated string print (same behavior as print_str).
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false)}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["println"] = std::move(sch);
        }

        // --- Phase 28: Hash / Eq support builtins ---
        // These back the prelude `impl Hash/Eq for i64` and `for String`
        // (so user code can call `k.hash()` / `a.eq(&b)` and bound generics on
        // `Hash`/`Eq`); `hash_string` is also called directly by the generic
        // HashMap/HashSet container for String keys. All pure.
        // hash_i64(s: &i64) -> i64 — identity hash (matches the historic
        // i64-keyed HashMap probing).
        {
            FnSchema sch;
            sch.signature =
                makeFunction({makeRef(makeInt(), /*isMut=*/false)}, makeInt());
            fnSchemas_["hash_i64"] = std::move(sch);
        }
        // int_eq(a: &i64, b: &i64) -> bool
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(makeInt(), /*isMut=*/false),
                 makeRef(makeInt(), /*isMut=*/false)},
                makeBool());
            fnSchemas_["int_eq"] = std::move(sch);
        }
        // hash_string(s: &String) -> i64 — FNV-1a over the byte contents.
        {
            FnSchema sch;
            sch.signature =
                makeFunction({makeRef(stringTy, /*isMut=*/false)}, makeInt());
            fnSchemas_["hash_string"] = std::move(sch);
        }

        // --- Phase 30: file I/O + CLI args support builtins ---
        // The prelude wraps these in Result<_, IoError> (fs_read_to_string /
        // fs_write) and Vec<String> (args). Status convention: 0 = ok, 1 =
        // not-found, 2 = permission-denied, 4 = other (classified portably via
        // access(), so no libc errno symbol is referenced).
        // fs_read_into(path: &String, out: &mut String) -> i64 ! { io, alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false),
                 makeRef(stringTy, /*isMut=*/true)},
                makeInt());
            sch.declaredEffects.add("io");
            sch.declaredEffects.add("alloc");
            fnSchemas_["fs_read_into"] = std::move(sch);
        }
        // fs_write_raw(path: &String, contents: &String) -> i64 ! { io }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false),
                 makeRef(stringTy, /*isMut=*/false)},
                makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["fs_write_raw"] = std::move(sch);
        }
        // fs_exists(path: &String) -> bool ! { io }
        {
            FnSchema sch;
            sch.signature =
                makeFunction({makeRef(stringTy, /*isMut=*/false)}, makeBool());
            sch.declaredEffects.add("io");
            fnSchemas_["fs_exists"] = std::move(sch);
        }
        // arg_count() -> i64 — number of process CLI args (argv[0] included).
        // Pure: it reads process state set at startup, no syscall.
        {
            FnSchema sch;
            sch.signature = makeFunction({}, makeInt());
            fnSchemas_["arg_count"] = std::move(sch);
        }
        // arg_get(i: i64) -> String — a borrowed view of argv[i] (cap 0, not
        // freed on drop); an out-of-range index yields the empty string.
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, stringTy);
            fnSchemas_["arg_get"] = std::move(sch);
        }

        // Phase 5.z: vec_* are generic over T. Each call site infers T
        // from arg types; codegen lazily specializes the runtime per T.
        TypePtr vecFnGenericVar = makeFreshVar();
        TypePtr vecFnInst = makeStruct("Vec", {});
        vecFnInst->typeArgs = {vecFnGenericVar};

        // vec_new<T>() -> Vec<T> ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({}, vecFnInst);
            sch.genericVars.push_back(vecFnGenericVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["vec_new"] = std::move(sch);
        }
        // vec_push<T>(v: &mut Vec<T>, x: T) -> i64 ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true), vecFnGenericVar},
                makeInt());
            sch.genericVars.push_back(vecFnGenericVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["vec_push"] = std::move(sch);
        }
        // vec_get<T>(v: &Vec<T>, i: i64) -> T
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/false), makeInt()},
                vecFnGenericVar);
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_get"] = std::move(sch);
        }
        // Phase 34: vec_get_ref<T>(v: &Vec<T>, i: i64) -> &T — a BORROW into
        // the buffer (read without moving/copying the element out).
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/false), makeInt()},
                makeRef(vecFnGenericVar, /*isMut=*/false));
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_get_ref"] = std::move(sch);
        }
        // vec_len<T>(v: &Vec<T>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_len"] = std::move(sch);
        }
        // Phase 35: clone<T>(x: &T) -> T ! { alloc } — a DEEP copy. The result
        // owns freshly-allocated heap storage (String buffer, Vec/HashMap
        // backing array, Box payload — recursively), so the clone and the
        // original can each be dropped exactly once with no shared storage.
        // The built-in deep-clone covers scalars, String, Vec, HashMap/HashSet,
        // Box, and user structs/enums INCLUDING self-recursive types (cloned by
        // a per-type clone function that recurses at run time). It subsumes the
        // roadmap's "Clone trait + hand-written user impls": generic `impl<T>`
        // blocks are deferred (Phase 21), so an intrinsic is the sound path and
        // is strictly more capable (no per-type boilerplate).
        {
            FnSchema sch;
            TypePtr cloneVar = makeFreshVar();
            sch.signature =
                makeFunction({makeRef(cloneVar, /*isMut=*/false)}, cloneVar);
            sch.genericVars.push_back(cloneVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["clone"] = std::move(sch);
        }

        // Phase 13b / 17b built-in: `HashMap<i64, V>` — an open-addressing
        // map. The KEY stays `i64` (no Hash trait yet); the VALUE V is
        // generic, inferred at the first insert (like `Vec<T>` via vec_push).
        // Internally we carry only V in `typeArgs[0]` (the key is implicitly
        // i64). The struct is nameable but its fields are opaque to user
        // dot-access (codegen lays it out as { i8* buckets, i64 len, i64 cap }
        // with a per-V `{ i64 state, i64 key, V value }` bucket entry).
        // hashmap_get returns `Option<V>`; it's registered after the prelude's
        // Option enum is resolved (see below, post pass-1).
        // Phase 28: HashMap is now generic over BOTH the key K and value V
        // (`typeArgs = {K, V}`). Codegen stores the key by value in a per-(K,V)
        // bucket entry `{ i64 state, K key, V value }` and hashes/compares K via
        // its Hash/Eq impls (i64 + String built in; user types pluggable). K is
        // required to be a hashable type (see the HashMap key-bound check in
        // resolveTypeRef). hashmap_get returns `Option<V>` (registered after the
        // prelude Option enum resolves, below).
        TypePtr hmKeyVar = makeFreshVar();
        TypePtr hmValVar = makeFreshVar();
        TypePtr hashMapInst = makeStruct("HashMap", {});
        hashMapInst->typeArgs = {hmKeyVar, hmValVar};
        {
            StructSchema sch;
            sch.type = hashMapInst;
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            structSchemas_["HashMap"] = std::move(sch);
        }
        // hashmap_new<K,V>() -> HashMap<K,V> ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({}, hashMapInst);
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashmap_new"] = std::move(sch);
        }
        // hashmap_insert<K,V>(m: &mut HashMap<K,V>, k: K, v: V) -> i64 ! {alloc}
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashMapInst, /*isMut=*/true), hmKeyVar, hmValVar},
                makeInt());
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashmap_insert"] = std::move(sch);
        }
        // hashmap_len<K,V>(m: &HashMap<K,V>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashMapInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            fnSchemas_["hashmap_len"] = std::move(sch);
        }
        // Phase 38: hashmap_keys<K,V>(m: &HashMap<K,V>) -> Vec<K> ! { alloc } —
        // a freshly-allocated Vec of the live keys (each deep-cloned, so the
        // Vec owns them). Bucket-order. The way to enumerate a map's entries
        // (e.g. to serialize a JSON object).
        {
            FnSchema sch;
            TypePtr vecOfKey = makeStruct("Vec", {});
            vecOfKey->typeArgs = {hmKeyVar};
            sch.signature = makeFunction(
                {makeRef(hashMapInst, /*isMut=*/false)}, vecOfKey);
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashmap_keys"] = std::move(sch);
        }

        // Phase 28: `HashSet<T>` — a set over a hashable element T. Codegen
        // reuses the HashMap table machinery with a dummy i64 value, so T must
        // be hashable just like a HashMap key (checked in resolveTypeRef).
        TypePtr hsVar = makeFreshVar();
        TypePtr hashSetInst = makeStruct("HashSet", {});
        hashSetInst->typeArgs = {hsVar};
        {
            StructSchema sch;
            sch.type = hashSetInst;
            sch.genericVars.push_back(hsVar);
            structSchemas_["HashSet"] = std::move(sch);
        }
        // hashset_new<T>() -> HashSet<T> ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({}, hashSetInst);
            sch.genericVars.push_back(hsVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashset_new"] = std::move(sch);
        }
        // hashset_insert<T>(s: &mut HashSet<T>, k: T) -> i64 ! { alloc }
        // Returns 1 if the element was newly added, 0 if already present.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/true), hsVar}, makeInt());
            sch.genericVars.push_back(hsVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashset_insert"] = std::move(sch);
        }
        // hashset_contains<T>(s: &HashSet<T>, k: T) -> bool
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/false), hsVar}, makeBool());
            sch.genericVars.push_back(hsVar);
            fnSchemas_["hashset_contains"] = std::move(sch);
        }
        // hashset_len<T>(s: &HashSet<T>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(hsVar);
            fnSchemas_["hashset_len"] = std::move(sch);
        }

        // Phase 13b built-in: `&[i64]` slices (MVP element = i64). A slice is
        // the fat pointer produced by `&v[a..b]`; the two ops mirror Vec's
        // read API. The slice value is passed by value (it's a small {ptr,len}
        // aggregate). Constructing a slice doesn't allocate, so no `alloc`.
        // Phase 37: the slice read API is now generic over the element type T
        // (was i64-only). The slice value carries its element type, so codegen
        // strides by `sizeof(T)`. slice_len ignores T (reads the len field);
        // slice_get copies element i (T must be Copy at the call site for a
        // value read); slice_get_ref borrows it (&T) for non-Copy elements.
        TypePtr sliceFnVar = makeFreshVar();
        TypePtr sliceFnInst = makeSlice(sliceFnVar);
        // slice_len<T>(s: &[T]) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction({sliceFnInst}, makeInt());
            sch.genericVars.push_back(sliceFnVar);
            fnSchemas_["slice_len"] = std::move(sch);
        }
        // slice_get<T>(s: &[T], i: i64) -> T
        {
            FnSchema sch;
            sch.signature = makeFunction({sliceFnInst, makeInt()}, sliceFnVar);
            sch.genericVars.push_back(sliceFnVar);
            fnSchemas_["slice_get"] = std::move(sch);
        }
        // slice_get_ref<T>(s: &[T], i: i64) -> &T — a borrow into the slice.
        {
            FnSchema sch;
            sch.signature = makeFunction({sliceFnInst, makeInt()},
                                         makeRef(sliceFnVar, /*isMut=*/false));
            sch.genericVars.push_back(sliceFnVar);
            fnSchemas_["slice_get_ref"] = std::move(sch);
        }

        // Phase 12 built-in: `yield_now(v: i64) -> Future<i64> ! { async }`.
        // The leaf suspending primitive (stays monomorphic i64). Its Future
        // returns Pending on the first poll and Ready(v) on the second, so
        // awaiting it genuinely suspends the enclosing async fn exactly once.
        // Carries the `async` effect so any fn that awaits it must declare
        // `async` (which `async fn`s do implicitly).
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, futureI64Ty);
            sch.declaredEffects.add("async");
            fnSchemas_["yield_now"] = std::move(sch);
        }
        // Phase 12 built-in, Phase 17b generic: `block_on<T>(f: Future<T>)
        // -> T`. The single-threaded executor: busy-polls `f` until it
        // reports Ready, returning the value. T is inferred from the awaited
        // future (e.g. `block_on(ab())` where `ab -> bool` yields a bool).
        // Codegen lazily specializes the executor per T (DataLayout-sized
        // Poll<T>), mirroring vec_*. block_on itself is synchronous (no
        // `async` effect): it drives the future rather than suspending on it.
        {
            TypePtr blockOnVar = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction({makeFuture(blockOnVar)}, blockOnVar);
            sch.genericVars.push_back(blockOnVar);
            fnSchemas_["block_on"] = std::move(sch);
        }
        // Phase 12 built-in: `poll_count() -> i64`. Reads the global poll
        // counter incremented on every future poll. Lets tests observe that
        // the Pending path is actually taken (criterion b).
        {
            FnSchema sch;
            sch.signature = makeFunction({}, makeInt());
            fnSchemas_["poll_count"] = std::move(sch);
        }

        // Phase 18 built-in: `sleep_ms(n: i64) -> Future<i64> ! { async }`.
        // A real wall-clock timer leaf future: its poll computes a deadline
        // `now + n ms` on the first poll, then returns Pending until the
        // monotonic clock reaches it, then Ready(n). Like `yield_now` it
        // carries the `async` effect (awaiting it suspends the enclosing fn),
        // but unlike `yield_now` it genuinely waits on time and registers its
        // deadline with the executor so the reactor can sleep instead of spin.
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, futureI64Ty);
            sch.declaredEffects.add("async");
            fnSchemas_["sleep_ms"] = std::move(sch);
        }
        // Phase 18 built-in, generic: `spawn<T>(f: Future<T>) -> i64`.
        // Registers `f` as a concurrent task with the process-global executor
        // and returns an i64 task handle. Synchronous (no `async` effect): it
        // only enqueues; the task makes progress when the executor is driven
        // (by `block_on`/`join`). Codegen specializes per T so the handle's
        // result slot is sized for T (read back by `join<T>`).
        {
            TypePtr spawnVar = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction({makeFuture(spawnVar)}, makeInt());
            sch.genericVars.push_back(spawnVar);
            fnSchemas_["spawn"] = std::move(sch);
        }
        // Phase 18 built-in, generic: `join<T>(handle: i64) -> T`.
        // Drives the executor until the task named by `handle` completes, then
        // yields its result. Synchronous (it runs the executor, not suspends).
        // T is inferred from the binding context; codegen reads the handle's
        // result slot as T.
        {
            TypePtr joinVar = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, joinVar);
            sch.genericVars.push_back(joinVar);
            fnSchemas_["join"] = std::move(sch);
        }

        // Phase 19 built-in: OS threads on pthread.
        //
        //   thread_spawn(f: fn() -> i64) -> i64 ! { io }
        // Spawns a real OS thread running the fn VALUE `f` and returns an
        // opaque thread handle. `io`-effecting (it has an observable effect
        // beyond pure computation — like spawning a task). The argument is a
        // `fn() -> i64` value (a top-level fn, a let-bound fn value, or a
        // closure). NOTE the compile-time Send rule enforced in checkCall: a
        // closure passed here must not capture anything BY REFERENCE (FnMut) —
        // a by-ref capture aliases a stack slot across the thread boundary (a
        // data race + use-after-free once the spawning frame exits), so it is
        // rejected. All captures into a thread must be by value (Send / moved).
        // Effect-polymorphic over the closure's effects: spelled
        // `thread_spawn<e>(f: fn() -> i64 ! {e}) -> i64 ! {io, e}`. The row var
        // `e` lets a closure that itself performs effects (e.g. `io` via
        // `mutex_lock`, or `alloc`) be accepted, and propagates those effects
        // to the caller (they DO happen as a consequence of the spawn, even if
        // on the new thread) — so the spawning fn must declare them, just like
        // calling the closure directly would require. The unconditional `io`
        // is the spawn's own effect.
        {
            TypePtr rowVar = makeFreshVar();
            TypePtr fnParam = makeFunction({}, makeInt(),
                                           /*effectLabels=*/{}, rowVar);
            FnSchema sch;
            sch.signature = makeFunction({fnParam}, makeInt());
            sch.declaredEffects.add("io");
            sch.declaredEffects.add("e"); // row-var name (flows the closure's)
            sch.effectRowVars.emplace_back("e", rowVar);
            fnSchemas_["thread_spawn"] = std::move(sch);
        }
        //   thread_join(handle: i64) -> i64 ! { io }
        // Joins the thread named by `handle` and returns the i64 value its fn
        // produced. `io`-effecting (it blocks on / observes another thread).
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["thread_join"] = std::move(sch);
        }
        // Phase 19 built-in: `Mutex<i64>` represented as an i64 HANDLE (a
        // pointer to a heap `{ pthread_mutex_t, i64 value }`). The handle is
        // i64 — therefore Copy — so it can be captured BY VALUE into each
        // thread's closure, giving every thread the same underlying lock +
        // cell (the cross-thread sharing mechanism).
        //
        //   mutex_new(v: i64) -> i64 ! { alloc }   (heap-allocates the block)
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            sch.declaredEffects.add("alloc");
            fnSchemas_["mutex_new"] = std::move(sch);
        }
        //   mutex_lock(h: i64) -> i64 ! { io }
        //   mutex_unlock(h: i64) -> i64 ! { io }
        // Acquire / release the lock; `io`-effecting (cross-thread sync is an
        // observable side effect). Return 0 (an i64 so they compose).
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_lock"] = std::move(sch);
        }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_unlock"] = std::move(sch);
        }
        //   mutex_get(h: i64) -> i64           (read the guarded cell)
        //   mutex_set(h: i64, v: i64) -> i64   (write the guarded cell)
        // The caller is expected to hold the lock. `io`-effecting for the
        // write (a shared-memory mutation other threads observe); the read is
        // left pure so reading under a lock needs no extra annotation.
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, makeInt());
            fnSchemas_["mutex_get"] = std::move(sch);
        }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt(), makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_set"] = std::move(sch);
        }

        // Phase 23 built-in: real panic + unwinding.
        //
        //   panic(msg: String) -> i64 ! { panic }
        // Prints `msg` to stderr and unwinds (running Drop cleanups) to the
        // nearest enclosing `catch`, or terminates the process with exit code
        // 101 if there is none. Carries the `panic` effect, so any caller must
        // either declare `! { panic }` or wrap the call in `catch`. `msg` is
        // taken BY VALUE so a string LITERAL (`panic("boom")`) — itself a
        // `String` value `{ptr,len,0}` — passes directly without an explicit
        // `&` (mirrors `string_push_str`'s by-value `other`). The i64 return
        // type lets it sit in expression position (`if c { a } else {
        // panic("..") }`); it never actually returns.
        {
            FnSchema sch;
            sch.signature = makeFunction({stringTy}, makeInt());
            sch.declaredEffects.add("panic");
            fnSchemas_["panic"] = std::move(sch);
        }
        //   catch(f: fn() -> i64 ! {e}, recover: i64) -> i64 ! { e \ panic }
        // Runs `f()` under a recovery boundary: returns its value on normal
        // completion, or `recover` if `f` (or anything it transitively calls)
        // panics — after the unwinder has run the Drop cleanups of the frames
        // between the panic and here. `catch` is the panic HANDLER, so it
        // CLEARS the `panic` effect of `f` for its own caller (checkCall strips
        // `panic` from this call's recorded effect contribution); all OTHER
        // effects of `f` (io/alloc/...) still flow through, via the effect-row
        // var `e` — exactly like `thread_spawn`. A non-panicking `f` is
        // perfectly legal too (the boundary is simply never triggered).
        {
            TypePtr rowVar = makeFreshVar();
            TypePtr fnParam = makeFunction({}, makeInt(),
                                           /*effectLabels=*/{}, rowVar);
            FnSchema sch;
            sch.signature = makeFunction({fnParam, makeInt()}, makeInt());
            sch.declaredEffects.add("e"); // flows f's effects MINUS panic
            sch.effectRowVars.emplace_back("e", rowVar);
            fnSchemas_["catch"] = std::move(sch);
        }
#if defined(__linux__)
        // Phase 18 stretch (Linux/epoll ONLY): real fd-readiness primitives.
        // Registered only on Linux — the codegen reactor is epoll-based and
        // `#if`-guarded, and macOS (kqueue) support is documented as deferred,
        // so on macOS these names simply don't exist (a program using them
        // won't typecheck there, by design, rather than silently stubbing).
        //
        //   pipe_make() -> i64           : create an OS pipe; returns a handle
        //                                  packing (write_fd << 32 | read_fd).
        //   pipe_send(h: i64, b: i64)->i64: write one byte `b` to the pipe.
        //   read_pipe(h: i64) -> Future<i64> ! { async } : a leaf future that
        //                                  becomes Ready(byte) when the read
        //                                  end is readable; the executor blocks
        //                                  in epoll_wait until then.
        {
            FnSchema sch;
            sch.signature = makeFunction({}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["pipe_make"] = std::move(sch);
        }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt(), makeInt()}, makeInt());
            sch.declaredEffects.add("io");
            fnSchemas_["pipe_send"] = std::move(sch);
        }
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, futureI64Ty);
            sch.declaredEffects.add("async");
            fnSchemas_["read_pipe"] = std::move(sch);
        }
#endif

        // Pass 1a: register every struct and enum decl. To allow free
        // cross-references (struct field of enum type, enum payload of
        // struct type), we do this in two phases:
        //   (i)  create opaque Type entries for every struct/enum just by
        //        name; the inner field/variant vectors stay empty.
        //   (ii) resolve the field types / variant payload types now that
        //        every name is bound. Mutate the entries in-place.
        //
        // Phase (i): opaque registration. Each struct/enum reserves an
        // entry with empty body and (for generics) a fresh schema Var per
        // declared type parameter; the schema's `typeArgs` stays empty
        // (typeArgs are filled only at *use* sites, never on the schema).
        for (const auto& sd : program.structs) {
            if (structSchemas_.count(sd.name) ||
                enumSchemas_.count(sd.name)) {
                error("struct redefined: " + sd.name, sd.line, sd.column);
                continue;
            }
            structSchemas_[sd.name] =
                buildSchemaShell<StructSchema>(sd.name, sd.genericParams,
                                                 /*isStruct=*/true);
        }
        for (const auto& ed : program.enums) {
            if (structSchemas_.count(ed.name) ||
                enumSchemas_.count(ed.name)) {
                error("enum redefined: " + ed.name, ed.line, ed.column);
                continue;
            }
            enumSchemas_[ed.name] =
                buildSchemaShell<EnumSchema>(ed.name, ed.genericParams,
                                              /*isStruct=*/false);
        }

        // Phase (ii): resolve struct field types and enum variant payloads.
        // For generic types the schema's per-decl generic env is active
        // while resolving the field / payload TypeRefs, so a `T` in
        // `struct Box<T> { value: T }` resolves to the schema Var.
        for (const auto& sd : program.structs) {
            auto it = structSchemas_.find(sd.name);
            if (it == structSchemas_.end()) continue; // duplicate
            if (!it->second.type->structFields.empty()) continue;
            GenericEnv genEnv = buildGenericEnv(sd.genericParams,
                                                  it->second.genericVars);
            currentGenericEnv_ = &genEnv;
            std::vector<std::pair<std::string, TypePtr>> resolvedFields;
            resolvedFields.reserve(sd.fields.size());
            std::unordered_set<std::string> seen;
            for (const auto& f : sd.fields) {
                if (!seen.insert(f.name).second) {
                    error("duplicate field '" + f.name + "' in struct '" +
                              sd.name + "'",
                          sd.line, sd.column);
                    continue;
                }
                resolvedFields.emplace_back(f.name, resolveTypeRef(f.type));
            }
            it->second.type->structFields = std::move(resolvedFields);
            currentGenericEnv_ = nullptr;
        }

        for (const auto& ed : program.enums) {
            auto it = enumSchemas_.find(ed.name);
            if (it == enumSchemas_.end()) continue; // duplicate
            if (!it->second.type->enumVariants.empty()) continue;
            GenericEnv genEnv = buildGenericEnv(ed.genericParams,
                                                  it->second.genericVars);
            currentGenericEnv_ = &genEnv;
            std::vector<EnumVariantType> resolvedVariants;
            resolvedVariants.reserve(ed.variants.size());
            std::unordered_set<std::string> seenVariant;
            for (unsigned vi = 0; vi < ed.variants.size(); ++vi) {
                const auto& v = ed.variants[vi];
                if (!seenVariant.insert(v.name).second) {
                    error("duplicate variant '" + v.name + "' in enum '" +
                              ed.name + "'",
                          v.line, v.column);
                    continue;
                }
                // Cross-enum uniqueness: globally each variant name binds
                // to exactly one enum (Phase 2.2 simplification — no path
                // syntax).
                auto existing = variantIndex_.find(v.name);
                if (existing != variantIndex_.end()) {
                    error("variant '" + v.name +
                              "' is already defined in enum '" +
                              existing->second.first +
                              "'; cannot redefine in enum '" + ed.name + "'",
                          v.line, v.column);
                    continue;
                }
                std::vector<TypePtr> payload;
                payload.reserve(v.payloadTypes.size());
                for (const auto& pt : v.payloadTypes) {
                    payload.push_back(resolveTypeRef(pt));
                }
                EnumVariantType evt;
                evt.name = v.name;
                evt.payloadTypes = payload;
                resolvedVariants.push_back(std::move(evt));
                variantIndex_[v.name] = {ed.name,
                                         static_cast<unsigned>(
                                             resolvedVariants.size() - 1)};
            }
            it->second.type->enumVariants = std::move(resolvedVariants);
            currentGenericEnv_ = nullptr;
        }

        // Phase 13b / 17b: now that the prelude's generic `Option<T>` enum is
        // fully resolved, register `hashmap_get<V>(m: &HashMap<V>, k: i64)
        // -> Option<V>`. We instantiate Option with the SAME schema Var V the
        // HashMap is generic over, so a single call-site unification pins V
        // (from the map) and flows it into the returned `Option<V>` the
        // call-site `match` destructures. Only registered when `Option` is in
        // scope (always true via the prelude, but guarded so a hand-rolled
        // program lacking Option doesn't crash here).
        if (enumSchemas_.count("Option") && structSchemas_.count("HashMap")) {
            const EnumSchema& optSchema = enumSchemas_["Option"];
            TypePtr optV;
            if (!optSchema.genericVars.empty()) {
                std::unordered_map<int, TypePtr> subst;
                subst[optSchema.genericVars[0]->varId] = hmValVar;
                optV = instantiate(optSchema.type, subst);
                optV->typeArgs = {hmValVar};
            } else {
                optV = optSchema.type;
            }
            // hashmap_get<K,V>(m: &HashMap<K,V>, k: K) -> Option<V>
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashMapInst, /*isMut=*/false), hmKeyVar}, optV);
            sch.genericVars.push_back(hmKeyVar);
            sch.genericVars.push_back(hmValVar);
            fnSchemas_["hashmap_get"] = std::move(sch);

            // Phase 34: hashmap_get_ref<K,V>(m: &HashMap<K,V>, k: K)
            // -> Option<&V> — a borrow into the value slot (read-without-move).
            if (!optSchema.genericVars.empty()) {
                TypePtr refV = makeRef(hmValVar, /*isMut=*/false);
                std::unordered_map<int, TypePtr> subst;
                subst[optSchema.genericVars[0]->varId] = refV;
                TypePtr optRefV = instantiate(optSchema.type, subst);
                optRefV->typeArgs = {refV};
                FnSchema rsch;
                rsch.signature = makeFunction(
                    {makeRef(hashMapInst, /*isMut=*/false), hmKeyVar}, optRefV);
                rsch.genericVars.push_back(hmKeyVar);
                rsch.genericVars.push_back(hmValVar);
                fnSchemas_["hashmap_get_ref"] = std::move(rsch);
            }
        }

        // Pass 1c: register trait declarations. Each trait gets a global
        // entry with its method signatures, used later to validate impl
        // blocks and to type-check method calls through bounded generic
        // params.
        for (const auto& td : program.traits) {
            if (traits_.count(td.name)) {
                error("trait redefined: " + td.name, td.line, td.column);
                continue;
            }
            // Phase 21a: record the trait's generic type-param names (in
            // declaration order). Reject duplicates and names that shadow a
            // built-in / declared type, mirroring the fn/struct rules.
            std::vector<std::string> tparams;
            std::unordered_set<std::string> seenTp;
            for (const auto& gp : td.genericParams) {
                if (!seenTp.insert(gp.name).second) {
                    error("duplicate generic parameter '" + gp.name +
                              "' on trait '" + td.name + "'",
                          gp.line, gp.column);
                    continue;
                }
                if (gp.name == "Self" || gp.name == "i64" ||
                    gp.name == "bool" || structSchemas_.count(gp.name) ||
                    enumSchemas_.count(gp.name)) {
                    error("trait generic parameter '" + gp.name +
                              "' shadows an existing type",
                          gp.line, gp.column);
                }
                tparams.push_back(gp.name);
            }
            traitGenericParams_[td.name] = std::move(tparams);
            // Phase 21b: record the trait's associated-type names. Reject
            // duplicates and a clash with a generic-trait param name (both live
            // in the trait's type namespace).
            std::vector<std::string> atypes;
            std::unordered_set<std::string> seenAt;
            for (const auto& at : td.assocTypes) {
                if (!seenAt.insert(at.name).second) {
                    error("duplicate associated type '" + at.name +
                              "' in trait '" + td.name + "'",
                          at.line, at.column);
                    continue;
                }
                bool clashesParam = false;
                for (const auto& gp : td.genericParams)
                    if (gp.name == at.name) { clashesParam = true; break; }
                if (clashesParam) {
                    error("associated type '" + at.name + "' in trait '" +
                              td.name + "' clashes with a generic parameter",
                          at.line, at.column);
                    continue;
                }
                atypes.push_back(at.name);
            }
            traitAssocTypes_[td.name] = std::move(atypes);
            std::unordered_set<std::string> seenMethod;
            std::vector<ast::MethodSig> uniqueMethods;
            uniqueMethods.reserve(td.methods.size());
            for (const auto& m : td.methods) {
                if (!seenMethod.insert(m.name).second) {
                    error("duplicate method '" + m.name + "' in trait '" +
                              td.name + "'",
                          m.line, m.column);
                    continue;
                }
                if (m.params.empty() || m.params[0].name != "self" ||
                    m.params[0].type.name != "Self") {
                    error("trait method '" + m.name + "' must take `self` "
                          "as its first parameter",
                          m.line, m.column);
                }
                uniqueMethods.push_back(m);
            }
            traits_[td.name] = std::move(uniqueMethods);
        }

        // Pass 1d: register impl blocks. We resolve the implementing type
        // and validate that each impl method's signature matches the
        // trait's after substituting Self -> implementing type.
        // monomorphic-only in Phase 3.3 MVP: forType must resolve to a
        // concrete (no Vars) Struct or Enum.
        for (std::size_t implIdx = 0; implIdx < program.impls.size();
             ++implIdx) {
            const auto& impl = program.impls[implIdx];
            // Phase 15: inherent impls (`impl Type { ... }`) carry an empty
            // trait name and have no trait signature to validate methods
            // against. Trait impls keep the Phase 3.3 path (lookup + match).
            const bool inherent = impl.isInherent();
            const std::vector<ast::MethodSig>* traitMethods = nullptr;
            if (!inherent) {
                auto traitIt = traits_.find(impl.traitName);
                if (traitIt == traits_.end()) {
                    error("impl references unknown trait '" + impl.traitName +
                              "'",
                          impl.line, impl.column);
                    continue;
                }
                traitMethods = &traitIt->second;
                // Phase 21a: the impl must supply exactly as many trait type
                // args as the trait declares generic params. (A non-generic
                // trait keeps the 0/0 case unchanged.)
                std::size_t want = traitGenericParams_[impl.traitName].size();
                if (impl.traitTypeArgs.size() != want) {
                    error("impl of trait '" + impl.traitName + "' supplies " +
                              std::to_string(impl.traitTypeArgs.size()) +
                              " type arg(s), but the trait declares " +
                              std::to_string(want),
                          impl.line, impl.column);
                }
            }
            // Phase 40: resolve forType with the impl's generic params in
            // scope so `impl<T> .. for Pair<T>` yields Pair<Var> (typeName
            // "Pair") instead of erroring on unknown `T`.
            GenericEnv implEnv0 = implParamEnv(impl);
            const GenericEnv* savedEnv0 = currentGenericEnv_;
            currentGenericEnv_ = &implEnv0;
            currentVarBound_.clear();
            currentVarAllBounds_.clear();
            exposeImplGenericBounds(impl, implEnv0);
            TypePtr forTy = resolveTypeRef(impl.forType);
            currentVarBound_.clear();
            currentVarAllBounds_.clear();
            currentGenericEnv_ = savedEnv0;
            TypePtr rfor = resolve(forTy);
            std::string typeName;
            if (rfor->kind == TypeKind::Struct) typeName = rfor->structName;
            else if (rfor->kind == TypeKind::Enum) typeName = rfor->enumName;
            else if (rfor->kind == TypeKind::Int) typeName = "i64";
            else if (rfor->kind == TypeKind::Float) typeName = "f64";  // Phase 44
            else if (rfor->kind == TypeKind::Bool) typeName = "bool";
            else {
                error("impl for unsupported type " + typeToString(forTy),
                      impl.forType.line, impl.forType.column);
                continue;
            }
            // Phase 21b: associated types. For a trait impl, resolve each
            // `type Item = T;` (with `Self` -> forType and the trait's generic
            // params bound to the impl's trait-args, so a definition may name
            // `Self`/`T`), validate exact coverage of the trait's declared
            // associated types, and stash the concrete choices for use-site
            // resolution of `Self::Item` / `C::Item`.
            if (!inherent) {
                const std::vector<std::string>& wantAssoc =
                    traitAssocTypes_.count(impl.traitName)
                        ? traitAssocTypes_[impl.traitName]
                        : std::vector<std::string>{};
                std::unordered_set<std::string> wantSet(wantAssoc.begin(),
                                                        wantAssoc.end());
                GenericEnv assocEnv;
                assocEnv["Self"] = forTy;
                bindTraitParamsForImpl(impl, assocEnv);
                const GenericEnv* savedEnv = currentGenericEnv_;
                currentGenericEnv_ = &assocEnv;
                std::unordered_map<std::string, TypePtr> resolvedAssoc;
                std::unordered_set<std::string> seenAssoc;
                for (const auto& at : impl.assocTypes) {
                    if (!wantSet.count(at.name)) {
                        error("impl of trait '" + impl.traitName + "' for '" +
                                  typeName + "' defines associated type '" +
                                  at.name + "' which the trait does not declare",
                              at.line, at.column);
                        continue;
                    }
                    if (!seenAssoc.insert(at.name).second) {
                        error("associated type '" + at.name +
                                  "' defined more than once in impl of '" +
                                  impl.traitName + "' for '" + typeName + "'",
                              at.line, at.column);
                        continue;
                    }
                    resolvedAssoc[at.name] = resolveTypeRef(at.type);
                }
                currentGenericEnv_ = savedEnv;
                for (const auto& want : wantAssoc) {
                    if (!resolvedAssoc.count(want)) {
                        error("impl of trait '" + impl.traitName + "' for '" +
                                  typeName + "' is missing associated type '" +
                                  want + "'",
                              impl.line, impl.column);
                    }
                }
                if (!resolvedAssoc.empty())
                    implAssocTypes_[typeName][impl.traitName] =
                        std::move(resolvedAssoc);
            } else if (!impl.assocTypes.empty()) {
                error("inherent impl cannot define associated types",
                      impl.assocTypes[0].line, impl.assocTypes[0].column);
            }
            // Key inherent impls under an internal-only sentinel so user-defined
            // trait names can never collide with inherent impl registrations.
            const std::string implKey = inherent ? kInherentImplSentinel
                                                 : impl.traitName;
            ImplRegistration reg;
            reg.traitName = impl.traitName; // empty for inherent
            reg.typeName = typeName;
            // Validate methods + collect.
            std::unordered_set<std::string> seenMethod;
            for (const auto& fn : impl.methods) {
                if (!seenMethod.insert(fn.name).second) {
                    error("duplicate method '" + fn.name + "' in impl",
                          fn.line, fn.column);
                    continue;
                }
                if (!inherent) {
                    const ast::MethodSig* traitMethod = nullptr;
                    for (const auto& m : *traitMethods) {
                        if (m.name == fn.name) { traitMethod = &m; break; }
                    }
                    if (!traitMethod) {
                        error("method '" + fn.name +
                                  "' is not declared in trait '" +
                                  impl.traitName + "'",
                              fn.line, fn.column);
                        continue;
                    }
                }
                reg.methods[fn.name] = &fn;
            }
            // Verify a trait impl covers every trait method (no missing
            // impls). Inherent impls have no obligation here.
            if (!inherent) {
                for (const auto& m : *traitMethods) {
                    if (!reg.methods.count(m.name)) {
                        error("impl of '" + impl.traitName + "' for '" +
                                  typeName +
                                  "' is missing method '" + m.name + "'",
                              impl.line, impl.column);
                    }
                }
            }
            implMethodByType_[typeName][implKey] = std::move(reg);
            // Also build a quick (typeName, methodName) -> impl-method lookup
            // for the method-call resolver. A method name that exists on both
            // an inherent impl and a trait impl would collide here; we report
            // it rather than silently shadowing.
            for (const auto& [mname, mfn] :
                 implMethodByType_[typeName][implKey].methods) {
                auto& slot = methodImplLookup_[typeName][mname];
                if (slot.second != nullptr && slot.second != mfn) {
                    error("method '" + mname + "' is defined more than once "
                          "for type '" + typeName + "'",
                          mfn->line, mfn->column);
                }
                slot = {impl.traitName, mfn};
            }
        }

        // Pass 1a3 (Phase 25): register `const fn` names and `const` item
        // names BEFORE signature resolution (pass 1b). A fn *signature* may use
        // a const-expr array length (`fn f(a: [i64; N])` / `[i64; sq(2)]`), and
        // `resolveTypeRef` evaluates that length on the spot — so the const
        // item `N` and any const fn it calls must already be findable. Only the
        // name->AST mapping is needed here; evaluation stays on-demand.
        for (const auto& fn : program.functions) {
            if (fn.isConst && !constFns_.count(fn.name)) constFns_[fn.name] = &fn;
        }
        for (const auto& cd : program.consts) {
            if (constDecls_.count(cd.name)) {
                error("constant redefined: " + cd.name, cd.line, cd.column);
                continue;
            }
            constDecls_[cd.name] = &cd;
        }

        // Pass 1b: register every fn signature so calls can see siblings
        // (and the function can recurse into itself). For each fn, allocate
        // a fresh Var per generic parameter and resolve param / return type
        // refs with those Vars in scope, so the schema's Function type
        // references them wherever the source mentions the name. Also
        // register the schema's per-genericVar bound so method calls on a
        // bounded param type-check against the bound trait's methods.
        for (const auto& fn : program.functions) {
            if (fnSchemas_.count(fn.name)) {
                error("function redefined: " + fn.name, fn.line, fn.column);
                // Skip the second decl entirely — keep the first schema.
                continue;
            }
            // Phase 10a: classify which names are effect-row vars (appear
            // inside a `! { ... }` row) BEFORE allocating type Vars, so an
            // effect-row generic param does NOT enter `genVars` (codegen's
            // monomorphization list). Effects are compile-time only.
            std::unordered_set<std::string> rowVarNames = classifyEffectRowVars(
                fn.params, fn.returnType, fn.effects, fn.genericParams,
                fn.name);

            std::unordered_map<std::string, TypePtr> genEnv;
            std::vector<TypePtr> genVars;
            std::vector<std::string> genBounds;
            std::vector<std::vector<std::string>> genExtraBounds;
            std::vector<std::pair<std::string, TypePtr>> effectRowVars;
            genVars.reserve(fn.genericParams.size());
            genBounds.reserve(fn.genericParams.size());
            genExtraBounds.reserve(fn.genericParams.size());
            // Phase 21a: the AST TypeParam backing each genVars[i], so a
            // second pass can resolve its parameterized-bound args once every
            // type param is registered in genEnv (a bound may reference a
            // later-declared param). nullptr-safe: only set for type vars.
            std::vector<const ast::TypeParam*> genBoundParam;
            std::unordered_set<std::string> seenGp;
            for (const auto& gp : fn.genericParams) {
                if (!seenGp.insert(gp.name).second) {
                    error("duplicate generic parameter '" + gp.name +
                              "' on fn '" + fn.name + "'",
                          gp.line, gp.column);
                    continue;
                }
                if (gp.name == "i64" || gp.name == "bool" ||
                    structSchemas_.count(gp.name) ||
                    enumSchemas_.count(gp.name)) {
                    error("generic parameter '" + gp.name +
                              "' shadows an existing type",
                          gp.line, gp.column);
                }
                TypePtr v = makeFreshVar();
                genEnv[gp.name] = v;
                if (rowVarNames.count(gp.name)) {
                    // Explicit effect-row generic param: tracked separately,
                    // kept out of genVars/genBounds (no monomorphization, no
                    // trait bound).
                    effectRowVars.emplace_back(gp.name, v);
                } else {
                    if (!gp.bound.empty() && !traits_.count(gp.bound)) {
                        error("unknown trait bound '" + gp.bound + "' on '" +
                                  gp.name + "'",
                              gp.line, gp.column);
                    }
                    // Phase 28: validate each additional bound (`T: A + B`).
                    for (const auto& eb : gp.extraBounds) {
                        if (!eb.empty() && !traits_.count(eb)) {
                            error("unknown trait bound '" + eb + "' on '" +
                                      gp.name + "'",
                                  gp.line, gp.column);
                        }
                    }
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
                    genExtraBounds.push_back(gp.extraBounds);
                    genBoundParam.push_back(&gp);
                }
            }
            // Implicit row vars (named only in fn-type rows, not in `<...>`)
            // each get a fresh Var, registered in genEnv for resolution.
            for (const auto& name : rowVarNames) {
                if (genEnv.count(name)) continue; // explicit, handled above
                TypePtr v = makeFreshVar();
                genEnv[name] = v;
                effectRowVars.emplace_back(name, v);
            }
            currentGenericEnv_ = &genEnv;
            currentEffectRowVarNames_ = &rowVarNames;

            // Phase 21a: now that every type param is in genEnv, resolve each
            // parameterized bound's trait-args + validate the bound's arity
            // against the trait's declared generic-param count.
            std::vector<std::vector<TypePtr>> genBoundArgs(genBounds.size());
            for (std::size_t i = 0; i < genBoundParam.size(); ++i) {
                const ast::TypeParam* gp = genBoundParam[i];
                if (!gp || gp->bound.empty()) continue;
                std::size_t want = traitGenericParams_.count(gp->bound)
                                       ? traitGenericParams_[gp->bound].size()
                                       : 0;
                // Phase 21a: an OMITTED arg list on a generic-trait bound
                // (`<I: Iterator>` when `Iterator` takes a `T`) means "infer
                // every element" — bind each trait param to a fresh Var so the
                // element type is inferred from how the method's result is used
                // in the body. This keeps the pre-21a `<I: Iterator>` spelling
                // working after migrating `Iterator` to `Iterator<T>`. A
                // NON-empty arg list must match the trait's arity exactly.
                std::vector<TypePtr> resolved;
                if (gp->boundTypeArgs.empty()) {
                    for (std::size_t k = 0; k < want; ++k)
                        resolved.push_back(makeFreshVar());
                } else {
                    if (gp->boundTypeArgs.size() != want) {
                        error("trait bound '" + gp->bound + "' on '" +
                                  gp->name + "' expects " +
                                  std::to_string(want) + " type arg(s), got " +
                                  std::to_string(gp->boundTypeArgs.size()),
                              gp->line, gp->column);
                    }
                    resolved.reserve(gp->boundTypeArgs.size());
                    for (const auto& a : gp->boundTypeArgs)
                        resolved.push_back(resolveTypeRef(a));
                }
                genBoundArgs[i] = std::move(resolved);
            }

            // Phase 21b: expose each bounded param's trait to `resolveTypeRef`
            // so a `C::Item` projection in this signature resolves C's bound.
            currentVarBound_.clear();
            currentVarAllBounds_.clear();
            for (std::size_t i = 0; i < genVars.size(); ++i) {
                recordVarBounds(genVars[i]->varId,
                                i < genBounds.size() ? genBounds[i]
                                                     : std::string{},
                                i < genExtraBounds.size()
                                    ? genExtraBounds[i]
                                    : std::vector<std::string>{});
            }

            std::vector<TypePtr> argTypes;
            argTypes.reserve(fn.params.size());
            for (const auto& p : fn.params) {
                argTypes.push_back(resolveTypeRef(p.type));
            }
            TypePtr ret = resolveTypeRef(fn.returnType);

            currentGenericEnv_ = nullptr;
            currentEffectRowVarNames_ = nullptr;
            currentVarBound_.clear();
            currentVarAllBounds_.clear();

            FnSchema schema;
            schema.signature = makeFunction(std::move(argTypes), ret);
            schema.genericVars = std::move(genVars);
            schema.genericBounds = std::move(genBounds);
            schema.genericExtraBounds = std::move(genExtraBounds);
            schema.genericBoundArgs = std::move(genBoundArgs);
            // Note: effect-row vars are deliberately NOT added to
            // genericVars — that list drives codegen monomorphization, and
            // effects are compile-time only (zero runtime cost). Row vars
            // are substituted separately at call sites via `effectRowVars`.
            schema.effectRowVars = std::move(effectRowVars);
            schema.declaredEffects = buildEffectSet(fn.effects,
                                                       fn.genericParams,
                                                       fn.name, &rowVarNames);
            // Phase 6.1 / 17b: `async fn -> T` wraps the declared return type
            // in `Future<T>`. The body still emits the inner type T (which we
            // stash on the schema for codegen); callers see `Future<T>` and
            // must `.await` (or `block_on`) to unwrap, recovering T.
            if (fn.isAsync) {
                schema.declaredEffects.add("async");
                schema.isAsync = true;
                schema.asyncInnerType = schema.signature->ret;
                schema.signature->ret = makeFuture(schema.signature->ret);
            }
            schema.isPub = fn.isPub;
            // Note (Phase 25): a `const fn` was already registered in
            // `constFns_` by pass 1a3 (so array-length const-exprs in
            // signatures can call it). It keeps its normal FnSchema here too,
            // so a runtime call type-checks + codegens exactly like any fn.
            fnSchemas_[fn.name] = std::move(schema);
        }
        // Pass 1c (Phase 24): register every `extern "C" fn` as a callable
        // FnSchema so calls to it type-check against the declared signature
        // like any other fn. Extern fns are monomorphic (no generic params),
        // opaque, and carry the `io` effect by default.
        registerExternFns(program);
        // Pass 1d (Phase 25): now that every fn schema exists, flag a `const`
        // whose name collides with a function. (Const items + const fns were
        // registered for name resolution before pass 1b.)
        for (const auto& cd : program.consts) {
            if (fnSchemas_.count(cd.name)) {
                error("constant '" + cd.name +
                          "' collides with a function of the same name",
                      cd.line, cd.column);
            }
        }
        // Pass 1f (Phase 25): type-check + compile-time-evaluate every const
        // item. This populates `constValues_` (so later array lengths /
        // expression uses read a memoized value) and records the folded value
        // for codegen. Done after all signatures exist so a const initializer
        // can call a const fn declared later in the file.
        for (const auto& cd : program.consts) {
            checkConstItem(cd);
        }
        // Pass 1e: register each impl method as a regular fn schema under
        // its mangled name so call routing through MethodCallExpr can
        // re-use the existing fn-call machinery. The mangled name encodes
        // (trait, implementing-type, method) so it never clashes with a
        // user-declared free fn. Self gets rewritten to the implementing
        // type during signature resolution.
        for (const auto& impl : program.impls) {
            for (const auto& fn : impl.methods) {
                std::string mangled =
                    implMethodMangledName(impl.traitName, impl.forType,
                                            fn.name);
                if (fnSchemas_.count(mangled)) continue; // duplicate-impl
                std::unordered_map<std::string, TypePtr> genEnv;
                std::vector<TypePtr> genVars;
                std::vector<std::string> genBounds;
                std::vector<std::vector<std::string>> genExtraBounds;
                std::vector<const ast::TypeParam*> genBoundParam;
                // Phase 40: the IMPL's own generic params come FIRST in the
                // method's effective generic vars (the convention checkImplMethod
                // + codegen rely on), so `impl<T: Clone> .. for Pair<T>` makes
                // each method generic over T, with T's bound in force in the
                // body and T inferred from the receiver at a call.
                for (const auto& gp : impl.genericParams) {
                    TypePtr v = makeFreshVar();
                    genEnv[gp.name] = v;
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
                    genExtraBounds.push_back(gp.extraBounds);
                    genBoundParam.push_back(&gp);
                }
                for (const auto& gp : fn.genericParams) {
                    TypePtr v = makeFreshVar();
                    genEnv[gp.name] = v;
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
                    genExtraBounds.push_back(gp.extraBounds); // Phase 28
                    genBoundParam.push_back(&gp);
                }
                // Bind Self to the impl's forType while resolving params /
                // return. This lets the impl write `self: Self -> i64`
                // and have it land as `self: ConcreteType -> i64`. genEnv
                // already holds the impl's generic params (Phase 40), so
                // `Pair<T>` resolves T to its Var — set it active first.
                currentGenericEnv_ = &genEnv;
                // Phase 46: expose the impl's bounds before resolving forType so
                // an `impl<K: Hash + Eq, V> .. for HashMap<K,V>` method schema
                // passes the key gate on `Self = HashMap<K,V>`.
                currentVarBound_.clear();
                currentVarAllBounds_.clear();
                exposeImplGenericBounds(impl, genEnv);
                TypePtr selfTy = resolveTypeRef(impl.forType);
                genEnv["Self"] = selfTy;
                // Phase 21a: bind the trait's generic params to this impl's
                // concrete trait-args, so a method signature mentioning `T`
                // (e.g. `-> Option<T>` from `Iterator<T>`) resolves to the
                // impl's arg (`Option<i64>` for `impl Iterator<i64>`).
                bindTraitParamsForImpl(impl, genEnv);
                // Phase 10a: classify effect-row vars for impl methods too,
                // so a method may take fn-typed params with effect rows.
                std::unordered_set<std::string> rowVarNames =
                    classifyEffectRowVars(fn.params, fn.returnType, fn.effects,
                                          fn.genericParams, fn.name);
                std::vector<std::pair<std::string, TypePtr>> effectRowVars;
                for (const auto& name : rowVarNames) {
                    auto git = genEnv.find(name);
                    TypePtr v = git != genEnv.end() ? git->second
                                                    : makeFreshVar();
                    if (git == genEnv.end()) genEnv[name] = v;
                    effectRowVars.emplace_back(name, v);
                }
                currentGenericEnv_ = &genEnv;
                currentEffectRowVarNames_ = &rowVarNames;
                // Phase 21a: resolve any parameterized-bound trait args on the
                // method's own generic params (rare, but kept consistent with
                // free fns).
                std::vector<std::vector<TypePtr>> genBoundArgs(
                    genBounds.size());
                for (std::size_t i = 0; i < genBoundParam.size(); ++i) {
                    const ast::TypeParam* gp = genBoundParam[i];
                    if (!gp || gp->bound.empty() ||
                        gp->boundTypeArgs.empty())
                        continue;
                    std::vector<TypePtr> resolved;
                    for (const auto& a : gp->boundTypeArgs)
                        resolved.push_back(resolveTypeRef(a));
                    genBoundArgs[i] = std::move(resolved);
                }
                // Phase 45: expose the method's generic bounds (impl + fn
                // params) so a `HashMap<K,V>` param/return with `K: Hash + Eq`
                // passes keyIsHashable while the schema's signature resolves.
                currentVarBound_.clear();
                currentVarAllBounds_.clear();
                for (std::size_t i = 0; i < genVars.size(); ++i) {
                    recordVarBounds(genVars[i]->varId,
                                    i < genBounds.size() ? genBounds[i]
                                                         : std::string{},
                                    i < genExtraBounds.size()
                                        ? genExtraBounds[i]
                                        : std::vector<std::string>{});
                }
                std::vector<TypePtr> argTypes;
                for (const auto& p : fn.params) {
                    argTypes.push_back(resolveTypeRef(p.type));
                }
                TypePtr ret = resolveTypeRef(fn.returnType);
                currentGenericEnv_ = nullptr;
                currentEffectRowVarNames_ = nullptr;
                currentVarBound_.clear();
                currentVarAllBounds_.clear();
                FnSchema sch;
                sch.signature = makeFunction(std::move(argTypes), ret);
                sch.genericVars = std::move(genVars);
                sch.genericBounds = std::move(genBounds);
                sch.genericExtraBounds = std::move(genExtraBounds);
                sch.genericBoundArgs = std::move(genBoundArgs);
                for (const auto& [name, v] : effectRowVars) {
                    bool dup = false;
                    for (const auto& gv : sch.genericVars)
                        if (gv.get() == v.get()) { dup = true; break; }
                    if (!dup) sch.genericVars.push_back(v);
                }
                sch.effectRowVars = std::move(effectRowVars);
                sch.declaredEffects = buildEffectSet(fn.effects,
                                                       fn.genericParams,
                                                       fn.name, &rowVarNames);
                if (fn.isAsync) sch.declaredEffects.add("async");
                fnSchemas_[mangled] = std::move(sch);
                implMethodMangled_[&fn] = mangled;
            }
        }

        // Pass 2: type-check each fn body.
        for (const auto& fn : program.functions) {
            checkFunction(fn);
        }
        // Pass 2 (impl methods): same, with Self bound to the impl's
        // forType. (Phase 40: checkImplMethod recomputes selfTy from its own
        // env so a generic impl's `Self` uses the schema's impl-param Vars;
        // this `selfTy` is the concrete/non-generic fallback. Resolve it with
        // the impl params in scope so `Pair<T>` doesn't error here.)
        for (const auto& impl : program.impls) {
            GenericEnv implEnv2 = implParamEnv(impl);
            const GenericEnv* savedEnv2 = currentGenericEnv_;
            currentGenericEnv_ = &implEnv2;
            currentVarBound_.clear();
            currentVarAllBounds_.clear();
            exposeImplGenericBounds(impl, implEnv2);
            TypePtr selfTy = resolveTypeRef(impl.forType);
            currentVarBound_.clear();
            currentVarAllBounds_.clear();
            currentGenericEnv_ = savedEnv2;
            for (const auto& fn : impl.methods) {
                checkImplMethod(fn, impl, selfTy);
            }
        }
        // Phase 4: effect-inference pass. For each fn body, collect the
        // union of every callee's declared effects and verify it's a
        // subset of the fn's own declared effects.
        for (const auto& fn : program.functions) checkEffects(fn, fn.name);
        for (const auto& impl : program.impls) {
            for (const auto& fn : impl.methods) {
                auto it = implMethodMangled_.find(&fn);
                if (it != implMethodMangled_.end()) {
                    checkEffects(fn, it->second);
                }
            }
        }
        // Validate trait method signatures don't declare unknown effects
        // (the dispatching impl method's effects already get checked
        // above via its FnSchema; the trait sig's effects are advisory
        // until Phase 4.3 ties row-vars in).
        for (const auto& td : program.traits) {
            for (const auto& m : td.methods) {
                (void)buildEffectSet(m.effects, {}, td.name + "::" + m.name);
            }
        }
        TypeCheckResult result;
        result.errors = std::move(errors_);
        result.exprTypes = std::move(exprTypes_);
        // Phase 25: hand the folded const values to codegen.
        for (const auto& [exprPtr, cv] : constExprValues_) {
            result.constExprValues[exprPtr] = ConstFolded{cv.isBool, cv.i};
        }
        result.structs = std::move(structSchemas_);
        result.enums = std::move(enumSchemas_);
        result.variantIndex = std::move(variantIndex_);
        result.matchTrees = std::move(matchTrees_);
        result.matchBindingTypes = std::move(matchBindingTypes_);
        result.usesFileIo = usesFileIo_;
        result.fnSchemas = std::move(fnSchemas_);
        result.callInstantiations = std::move(callInstantiations_);
        result.methodResolutions = std::move(methodResolutions_);
        result.dynCoercions = std::move(dynCoercions_);
        result.dynVtablesNeeded = std::move(dynVtablesNeeded_);
        result.assocProjections = std::move(assocProjections_);
        result.implAssocTypes = std::move(implAssocTypes_);
        return result;
    }

private:
    using Scope = std::unordered_map<std::string, TypePtr>;
    using GenericEnv = std::unordered_map<std::string, TypePtr>;

    std::unordered_map<std::string, FnSchema> fnSchemas_;
    std::unordered_map<std::string, StructSchema> structSchemas_;
    std::unordered_map<std::string, EnumSchema> enumSchemas_;

    // --- Phase 25: compile-time constants + const evaluation ----------------
    //
    // A const-evaluated value is i64 or bool (the MVP scalar set). `isBool`
    // distinguishes the two; `i` holds the i64 (or 0/1 for a bool).
    struct ConstValue {
        bool isBool = false;
        std::int64_t i = 0;
    };
    // Raised (and caught at const-eval entry points) when an initializer /
    // array length / const fn cannot be evaluated at compile time, or hits a
    // bound / overflow / div-by-zero. Carries a source position so the error
    // points at the offending construct.
    struct ConstEvalError {
        std::string message;
        std::size_t line = 1;
        std::size_t column = 1;
    };
    // Top-level `const` items by name -> their AST decl (for on-demand eval).
    std::unordered_map<std::string, const ast::ConstDecl*> constDecls_;
    // Memoized evaluated values of top-level consts (name -> value).
    std::unordered_map<std::string, ConstValue> constValues_;
    // Names currently mid-evaluation — used to detect cyclic references
    // (`const A = B; const B = A;`).
    std::unordered_set<std::string> constEvalInProgress_;
    // Phase 25: every Expr that resolved to a compile-time constant (a use of
    // a `const` name, or a const-fn call / arithmetic in a const context that
    // codegen should emit as a folded literal). Flows to codegen via the
    // TypeCheckResult so a `const` reaches the backend as an immediate, not a
    // runtime load. Keyed by the use-site Expr*.
    std::unordered_map<const ast::Expr*, ConstValue> constExprValues_;
    // `const fn` ASTs by name (registered alongside fnSchemas_), so the
    // evaluator can find + run a const fn body. A non-const fn is absent here
    // and calling it in a const context is a clear error.
    std::unordered_map<std::string, const ast::FnDecl*> constFns_;
    // Bounds for the evaluator (a runaway const fn -> clear error, not a hang).
    // `kConstEvalMaxDepth` caps nested const-fn CALL depth (so the native C++
    // recursion stays well under the stack limit while still allowing deep but
    // legitimate bounded recursion). `kConstEvalMaxSteps` is the global work
    // budget that catches non-terminating-yet-shallow loops of evaluation.
    static constexpr int kConstEvalMaxDepth = 1000;
    static constexpr long kConstEvalMaxSteps = 5'000'000;
    long constEvalSteps_ = 0;

    // Trait declarations: traitName -> ordered list of method signatures.
    std::unordered_map<std::string, std::vector<ast::MethodSig>> traits_;
    // Phase 21a: traitName -> the trait's generic type-param names, in
    // declaration order (empty for a non-generic trait). Method signatures
    // mention these names in type position; binding them to concrete args
    // (an impl's traitTypeArgs, or a bound's args) materializes a method's
    // element type. Kept separate from `traits_` so the dyn / non-generic
    // paths read `traits_` exactly as before.
    std::unordered_map<std::string, std::vector<std::string>>
        traitGenericParams_;
    // Phase 21b: traitName -> its associated-type names (declaration order).
    // `trait Container { type Item; ... }` records {"Item"}. Empty for a trait
    // with no associated types. Distinct from `traitGenericParams_`: associated
    // types are chosen per-impl (`type Item = i64;`), not supplied at the use
    // site like generic-trait args.
    std::unordered_map<std::string, std::vector<std::string>> traitAssocTypes_;
    // Phase 21b: per (implementing-type-name, trait-name), the concrete type
    // each associated type resolves to in that impl. Resolved with `Self` bound
    // to the impl's forType (and the trait's generic params bound to the impl's
    // trait-args). `Self::Item` inside an impl method, and `C::Item` through a
    // `C: Trait` bound at a monomorphic instance, both read this table.
    std::unordered_map<
        std::string,
        std::unordered_map<std::string,
                           std::unordered_map<std::string, TypePtr>>>
        implAssocTypes_;
    // Phase 21b: during signature resolution of a generic fn / impl method, the
    // bound trait of each in-scope generic param's schema Var, keyed by var id.
    // Lets `resolveTypeRef` resolve a `C::Item` projection (C a Var) to the
    // correct trait before the fn's schema is fully built. Cleared after each
    // signature resolves. Persisted long-term in `assocProjections_` for the
    // projection Vars that survive into bodies / codegen.
    std::unordered_map<int, std::string> currentVarBound_;
    // Phase 45: the FULL set of trait bounds on each generic Var in scope
    // (primary `.bound` + every `.extraBound`). currentVarBound_ keeps only the
    // primary for `C::Item` projection; container-op gates like keyIsHashable
    // need the whole set so a `K: Hash + Eq` param satisfies HashMap's key
    // requirement from inside a generic body. Cleared/populated in lockstep
    // with currentVarBound_.
    std::unordered_map<int, std::unordered_set<std::string>> currentVarAllBounds_;
    // Register every bound of a generic Var (primary + extras) into the two
    // bound maps. Safe to call with an empty primary (extras still recorded).
    void recordVarBounds(int varId, const std::string& primary,
                         const std::vector<std::string>& extras) {
        if (!primary.empty()) {
            currentVarBound_[varId] = primary;
            currentVarAllBounds_[varId].insert(primary);
        }
        for (const auto& e : extras)
            if (!e.empty()) currentVarAllBounds_[varId].insert(e);
    }
    // Phase 45/46: expose an impl's generic-param bounds (looked up by name in
    // `env`) into the bound maps. Must run BEFORE resolving `impl.forType` so an
    // `impl<K: Hash + Eq, V> .. for HashMap<K,V>` passes the key-hashable gate
    // on its own forType — otherwise the impl can't even be registered.
    void exposeImplGenericBounds(const ast::ImplDecl& impl,
                                 const GenericEnv& env) {
        for (const auto& gp : impl.genericParams) {
            auto it = env.find(gp.name);
            if (it == env.end()) continue;
            TypePtr v = resolve(it->second);
            if (v->kind == TypeKind::Var)
                recordVarBounds(v->varId, gp.bound, gp.extraBounds);
        }
    }
    // Phase 21b: placeholder-Var id -> how to resolve that `C::Item` projection
    // at codegen. Survives into the TypeCheckResult.
    std::unordered_map<int, AssocProjection> assocProjections_;
    // Impl registration: per implementing-type-name, per-trait-name, the
    // method-AST table. Indexed twice so method-call resolution can hop
    // typeName -> impl in O(1) and a missing method tells us the impl
    // doesn't claim that method even if the trait declares it.
    struct ImplRegistration {
        std::string traitName;
        std::string typeName;
        std::unordered_map<std::string, const ast::FnDecl*> methods;
    };
    std::unordered_map<std::string,
                       std::unordered_map<std::string, ImplRegistration>>
        implMethodByType_;
    // Flat lookup: typeName -> methodName -> (traitName, impl FnDecl*).
    // Lets `x.foo()` find foo by name without scanning every impl block.
    std::unordered_map<std::string,
                       std::unordered_map<
                           std::string,
                           std::pair<std::string, const ast::FnDecl*>>>
        methodImplLookup_;
    // Per-MethodCallExpr resolution (Concrete, BoundedGeneric, or Dyn).
    std::unordered_map<const ast::MethodCallExpr*, ResolvedMethod>
        methodResolutions_;
    // Phase 11: per-Expr `&T`->`&dyn`/`Box<T>`->`Box<dyn>` coercions and the
    // set of (trait,type) vtables those coercions require.
    std::unordered_map<const ast::Expr*, DynCoercion> dynCoercions_;
    std::vector<std::pair<std::string, std::string>> dynVtablesNeeded_;
    std::unordered_set<std::string> dynVtableSeen_; // dedupe key "Trait/Type"
    std::unordered_set<std::string> dynSafetyReported_; // dedupe dyn-safety
    // FnDecl* of an impl method -> its mangled FnSchema key.
    std::unordered_map<const ast::FnDecl*, std::string> implMethodMangled_;
    // Active during Pass-2 body-checking of an impl method: maps the
    // generic-param names of the enclosing fn to schema Vars, with `Self`
    // included so the body can mention `Self` in type positions.
    // Reused by `currentGenericEnv_` mechanics — no separate member.
    // Variant name -> {enumName, index within that enum}.
    std::unordered_map<std::string, std::pair<std::string, unsigned>>
        variantIndex_;
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes_;
    std::unordered_map<const ast::CallExpr*, std::vector<TypePtr>>
        callInstantiations_;
    // Phase 10a: per-call-site effect contribution, in source-level label
    // names (concrete built-ins and/or effect-row-var names). Populated
    // during body-checking (`checkCall` / `checkMethodCall`) where the
    // instantiated, unified types are available; consumed by the later
    // effect pass via `collectEffects`. Absent => fall back to the callee's
    // statically declared effects (the pre-Phase-10a behavior).
    std::unordered_map<const ast::Expr*, EffectSet> exprEffects_;
    // Phase 10a: name -> Var for the effect-row variables of the fn whose
    // body is currently being checked. Lets us map a resolved Type Var back
    // to the row-var name it stands for (so a still-polymorphic row var
    // contributes its name, e.g. `e`, to the enclosing fn's effect set).
    std::unordered_map<int, std::string> currentEffectRowVarById_;
    std::unordered_map<const ast::MatchExpr*,
                       std::unique_ptr<pattern_match::DecisionTree>>
        matchTrees_;
    // Phase 29: per match-arm pattern-binding types (name -> type), for
    // codegen drop of droppable payload bindings.
    std::unordered_map<const ast::MatchArm*,
                       std::unordered_map<std::string, TypePtr>>
        matchBindingTypes_;
    // Phase 30: set when the program calls a file-I/O / CLI-args builtin, so
    // codegen emits that (libc-referencing) runtime only on demand.
    bool usesFileIo_ = false;
    std::vector<TypeError> errors_;
    // Phase 13a: monotonic counter for fresh `for`-loop iterator slot names
    // (`__for_it_N`) when desugaring `for x in <Iterator>`.
    int forSlotCounter_ = 0;
    std::vector<Scope> scopes_;
    // Phase 9: parallel to scopes_; names of `let mut` (reassignable)
    // bindings in each scope. A name absent here is immutable.
    std::vector<std::unordered_set<std::string>> mutScopes_;
    std::vector<std::unordered_map<std::string, bool>> byRefClosureScopes_;
    // Phase 9: loop context stack. One entry per enclosing loop, innermost
    // last. `isValueLoop` is true for `loop` (where `break <value>` is
    // allowed); `while`/`for` are unit loops. `breakType` accumulates the
    // unified type of all `break <value>` expressions seen so far (null
    // until the first valued break).
    struct LoopCtx {
        bool isValueLoop = false;
        TypePtr breakType; // null = no valued break yet
        bool sawValuelessBreak = false;
    };
    std::vector<LoopCtx> loopStack_;
    TypePtr currentReturnType_;
    // Phase 12: true while checking the body of an `async fn`. `.await` is
    // only legal inside an async fn (it suspends the enclosing future, which
    // only exists if that fn IS a future). Set in checkFunction /
    // checkImplMethod, read by the AwaitExpr check.
    bool currentFnIsAsync_ = false;
    // Generic-param-name -> Type Var, scoped to the current fn declaration.
    // Set during Pass 1b sig resolution and during Pass 2 body checking
    // (with a fresh-instantiation copy for body checking, so accidental
    // specialization in a body — e.g. `x + 1` in `fn id<T>(x: T) -> T` —
    // doesn't taint the stored schema).
    const GenericEnv* currentGenericEnv_ = nullptr;
    // Phase 10a: names of generic params that are effect-row variables in
    // the signature currently being resolved. Active alongside
    // `currentGenericEnv_`; lets `resolveTypeRef` distinguish a row-var name
    // from a concrete label when building a function type's effect row.
    const std::unordered_set<std::string>* currentEffectRowVarNames_ = nullptr;

    void error(std::string msg, std::size_t line, std::size_t col) {
        errors_.push_back({std::move(msg), line, col});
    }

    // Phase 4: built-in effect labels recognized by the typechecker.
    // Anything else in an `! { ... }` row must match a row-variable name
    // declared in the fn's generic-parameter list (Phase 4.3).
    static bool isBuiltinEffect(const std::string& l) {
        return l == "alloc" || l == "io" || l == "panic" ||
               l == "async" || l == "unwind";
    }

    // Phase 10a classification: collect, from a signature, the names that
    // appear in *type* position and the names that appear inside a *function-
    // type* effect row (`fn(...) -> ... ! { e }`). The latter are the
    // syntactic sites that introduce effect-row variables (a row var must
    // flow through a fn-typed value to be observable). A label in a fn's own
    // top-level effect row that is neither a built-in nor backed by such a
    // use (or a declared generic param) stays an unknown-label error.
    static void collectNameUses(const ast::TypeRef& tr,
                                std::unordered_set<std::string>& inType,
                                std::unordered_set<std::string>& inFnEffect) {
        if (tr.isFn) {
            for (const auto& p : tr.fnParams)
                collectNameUses(p, inType, inFnEffect);
            if (tr.fnRet) collectNameUses(*tr.fnRet, inType, inFnEffect);
            for (const auto& l : tr.fnEffects)
                if (!isBuiltinEffect(l)) inFnEffect.insert(l);
            return;
        }
        // A bare/applied type name uses `tr.name` in type position. (Built-
        // in `i64`/`bool` and concrete struct/enum names are filtered out
        // later against the actual generic-param list.)
        inType.insert(tr.name);
        for (const auto& a : tr.typeArgs)
            collectNameUses(a, inType, inFnEffect);
    }

    // Compute, for one fn decl, the set of names that are effect-row
    // variables. Classification rule (Phase 10a, decidable): a name is an
    // effect-row variable iff it appears inside a `! { ... }` row anywhere
    // in the signature and is not a built-in effect. In practice this means
    // either (a) it is named in a function-type effect row `fn(...) ! {e}`
    // (which introduces it — explicitly listing it in `<...>` is optional),
    // or (b) it is a declared generic parameter mentioned in an effect row.
    // A name used in both a type position and an effect position is an error.
    std::unordered_set<std::string> classifyEffectRowVars(
        const std::vector<ast::Param>& params,
        const ast::TypeRef& returnType,
        const ast::EffectRow& effects,
        const std::vector<ast::TypeParam>& genericParams,
        const std::string& fnName) {
        std::unordered_set<std::string> inType;
        std::unordered_set<std::string> inFnEffect; // names in fn-type rows
        for (const auto& p : params)
            collectNameUses(p.type, inType, inFnEffect);
        collectNameUses(returnType, inType, inFnEffect);

        // Row vars: every name used in a function-type effect row, plus any
        // declared generic param that appears in an effect row (the decl's
        // own row or a fn-type row).
        std::unordered_set<std::string> rowVars = inFnEffect;
        std::unordered_set<std::string> declRowLabels;
        for (const auto& l : effects.labels)
            if (!isBuiltinEffect(l)) declRowLabels.insert(l);
        for (const auto& g : genericParams) {
            bool usedInEffect =
                inFnEffect.count(g.name) > 0 || declRowLabels.count(g.name) > 0;
            bool usedInType = inType.count(g.name) > 0;
            if (usedInEffect && usedInType) {
                error("generic parameter '" + g.name + "' on fn '" + fnName +
                          "' is used both as a type and as an effect-row "
                          "variable; pick one role",
                      g.line, g.column);
            }
            if (usedInEffect) rowVars.insert(g.name);
        }
        return rowVars;
    }

    // Phase 4.2: walk a function body and collect the effects induced by
    // every call inside it (direct CallExpr to a free fn, MethodCallExpr
    // to an impl method, and constructor calls which are pure). For
    // unknown callees (the typechecker already errored on them) we skip.
    void collectEffects(const ast::Expr& e, EffectSet& out) {
        if (dynamic_cast<const ast::IntLitExpr*>(&e)) return;
        if (dynamic_cast<const ast::FloatLitExpr*>(&e)) return;
        if (dynamic_cast<const ast::BoolLitExpr*>(&e)) return;
        if (dynamic_cast<const ast::StringLitExpr*>(&e)) return;
        if (dynamic_cast<const ast::IdentExpr*>(&e)) return;
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            collectEffects(*bin->lhs, out);
            collectEffects(*bin->rhs, out);
            return;
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            collectEffects(*un->operand, out);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args) collectEffects(*a, out);
            // Phase 10a: prefer the per-call-site contribution recorded
            // during body-checking (it reflects instantiated effect-row
            // vars — e.g. `apply(ioInc)` yields `io`, `apply(pureInc)`
            // yields nothing). Fall back to the callee's statically declared
            // effects only for fns with no effect-row vars (where declared
            // == actual), preserving pre-Phase-10a behavior. Indirect calls
            // (callee not in fnSchemas_) rely entirely on the recorded set.
            auto eit = exprEffects_.find(call);
            if (eit != exprEffects_.end()) {
                out.unionWith(eit->second);
            } else {
                auto fnIt = fnSchemas_.find(call->callee);
                if (fnIt != fnSchemas_.end() &&
                    fnIt->second.effectRowVars.empty()) {
                    // Phase 12: calling an `async fn` constructs an inert
                    // Future and does not perform `async` in the caller (only
                    // `.await`/`block_on` realize it). Union the callee's
                    // declared effects but strip `async` for async fns.
                    for (const auto& l : fnIt->second.declaredEffects.labels) {
                        if (l == "async" && fnIt->second.isAsync) continue;
                        out.add(l);
                    }
                }
            }
            // Constructor calls don't go through fnSchemas_; they're
            // pure (no effect added).
            return;
        }
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            // Phase 17a: an indirect call through a fn-value expression. The
            // callee sub-expression and the args contribute their own
            // effects; the call itself contributes the fn value's effect row,
            // recorded per-site during checkCallValue.
            collectEffects(*cv->callee, out);
            for (const auto& a : cv->args) collectEffects(*a, out);
            auto eit = exprEffects_.find(cv);
            if (eit != exprEffects_.end()) out.unionWith(eit->second);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            collectEffects(*mc->receiver, out);
            for (const auto& a : mc->args) collectEffects(*a, out);
            // Resolve the impl method via the typechecker's recorded
            // resolution and union in its declared effects.
            auto mit = methodResolutions_.find(mc);
            if (mit != methodResolutions_.end()) {
                ast::TypeRef forTy;
                forTy.name = mit->second.concreteTypeName;
                std::string mangled = implMethodMangledName(
                    mit->second.traitName, forTy, mit->second.methodName);
                auto sit = fnSchemas_.find(mangled);
                if (sit != fnSchemas_.end()) {
                    out.unionWith(sit->second.declaredEffects);
                }
            }
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            collectEffects(*ie->cond, out);
            collectEffects(*ie->thenBranch, out);
            collectEffects(*ie->elseBranch, out);
            return;
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            for (const auto& stmt : block->stmts) {
                if (auto* let = dynamic_cast<const ast::LetStmt*>(stmt.get())) {
                    collectEffects(*let->value, out);
                } else if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(
                               stmt.get())) {
                    if (ret->value) collectEffects(*ret->value, out);
                } else if (auto* as = dynamic_cast<const ast::AssignStmt*>(
                               stmt.get())) {
                    collectEffects(*as->target, out);
                    collectEffects(*as->value, out);
                } else if (auto* es = dynamic_cast<const ast::ExprStmt*>(
                               stmt.get())) {
                    collectEffects(*es->expr, out);
                }
            }
            if (block->tail) collectEffects(*block->tail, out);
            return;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            collectEffects(*we->cond, out);
            collectEffects(*we->body, out);
            return;
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            collectEffects(*le->body, out);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::ForExpr*>(&e)) {
            collectEffects(*fe->iter, out);
            // Phase 13a: a desugared `for` drives `__it.next()` each
            // iteration; union the resolved `next` impl's declared effects
            // (so an iterator whose `next` performs `io` makes the loop `io`).
            if (fe->iteratorDesugar && fe->nextCall) {
                collectEffects(*fe->nextCall, out);
            }
            collectEffects(*fe->body, out);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            collectEffects(*re->start, out);
            collectEffects(*re->end, out);
            return;
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e)) {
            if (be->value) collectEffects(*be->value, out);
            return;
        }
        if (dynamic_cast<const ast::ContinueExpr*>(&e)) {
            return;
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields) collectEffects(*v, out);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            collectEffects(*fe->object, out);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            collectEffects(*me->scrutinee, out);
            for (const auto& arm : me->arms) collectEffects(*arm.body, out);
            return;
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            collectEffects(*te->operand, out);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            collectEffects(*re->operand, out);
            return;
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            // Phase 13b: a slice's sub-expressions (the Vec + the bounds) may
            // carry effects; the slicing itself is pure (no alloc — it views
            // the existing buffer).
            collectEffects(*se->operand, out);
            collectEffects(*se->start, out);
            collectEffects(*se->end, out);
            return;
        }
        // Phase 22: arrays / tuples are stack value types (no alloc); their
        // sub-expressions' effects flow through. Indexing / tuple-field access
        // are themselves pure reads.
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            for (const auto& el : al->elements) collectEffects(*el, out);
            return;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            for (const auto& el : tl->elements) collectEffects(*el, out);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            collectEffects(*ix->object, out);
            collectEffects(*ix->index, out);
            return;
        }
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e)) {
            collectEffects(*tf->object, out);
            return;
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            // Phase 11: the boxed value's effects flow through. (We don't
            // synthesize `alloc` for the malloc itself — consistent with
            // struct literals / closures, whose heap use is also implicit.)
            collectEffects(*bn->value, out);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            // Phase 12: awaiting a future is itself an `async` effect (it may
            // suspend the enclosing future), in addition to whatever effects
            // the awaited future-producing operand performs.
            out.add("async");
            collectEffects(*ae->operand, out);
            return;
        }
        if (dynamic_cast<const ast::ClosureExpr*>(&e)) {
            // Phase 10b: defining a closure is pure. The effects of the
            // closure's body are encapsulated in the closure value's
            // Function-type effect row; they only reach the enclosing fn
            // when the closure is *called* (an indirect call, whose effect
            // contribution is recorded against that call site, not here).
            return;
        }
    }

    void checkEffects(const ast::FnDecl& fn,
                       const std::string& schemaKey) {
        auto it = fnSchemas_.find(schemaKey);
        if (it == fnSchemas_.end() || !fn.body) return;
        EffectSet inferred;
        collectEffects(*fn.body, inferred);
        const EffectSet& declared = it->second.declaredEffects;
        for (const auto& l : inferred.labels) {
            if (!declared.contains(l)) {
                error("function '" + fn.name +
                          "' uses effect `" + l +
                          "` but does not declare it; add `! { " + l +
                          (declared.labels.empty()
                               ? " }"
                               : ", ... }") +
                          "` to the signature",
                      fn.line, fn.column);
            }
        }
    }

    EffectSet buildEffectSet(const ast::EffectRow& row,
                              const std::vector<ast::TypeParam>& genericParams,
                              const std::string& fnName,
                              const std::unordered_set<std::string>*
                                  rowVarNames = nullptr) {
        EffectSet result;
        for (const auto& l : row.labels) {
            if (isBuiltinEffect(l)) {
                result.add(l);
                continue;
            }
            // Effect-row variable: an explicit generic parameter, or (Phase
            // 10a) any name classified as a row var by `classifyEffectRowVars`
            // (which includes implicit row vars introduced by first use in an
            // effect row). Such names are recorded by name in the effect set
            // and bound to a schema Var at instantiation.
            bool isRowVar = rowVarNames && rowVarNames->count(l) > 0;
            if (!isRowVar) {
                for (const auto& gp : genericParams) {
                    if (gp.name == l) { isRowVar = true; break; }
                }
            }
            if (isRowVar) {
                result.add(l);
            } else {
                error("unknown effect label `" + l + "` on fn '" + fnName +
                          "' (built-ins: alloc, io, panic, async, unwind; or "
                          "declare `" + l + "` as a generic effect-row "
                          "parameter)",
                      row.line, row.column);
            }
        }
        return result;
    }

    // Mangle an impl method into a globally-unique fn name. Format:
    // `__impl_<Trait>_for_<Type>__<method>`. Phase 3.3 MVP keys impls
    // by the implementing type's *base* name only, so `impl X for
    // Option<i64>` and `impl X for Option<bool>` are not both
    // expressible — duplicate-impl detection rejects the second. A
    // later phase that wants per-typeArgs impls will extend this
    // mangling and the (typeName, trait) lookup to include typeArgs.
    static constexpr const char* kInherentImplSentinel = "__kd_inherent_impl";

    // Phase 21a: for a trait impl, resolve the trait's generic params to the
    // impl's concrete trait-args and insert them into `env`. `env` should
    // already bind `Self` (so a trait-arg like `Self` would resolve), and is
    // the active `currentGenericEnv_` so resolveTypeRef sees `Self`. The
    // trait-args are resolved in that same env. No-op for inherent /
    // non-generic-trait impls (their traitTypeArgs are empty). Trait-param
    // names never clash with `Self` (validated at trait registration).
    // Phase 40: a generic env mapping the impl's own generic params to fresh
    // Vars, so `resolveTypeRef(impl.forType)` (e.g. `Pair<T>`) resolves T
    // instead of erroring on an unknown type. Used where we only need the
    // forType's NAME / shape (registration, pass-2 selfTy).
    GenericEnv implParamEnv(const ast::ImplDecl& impl) {
        GenericEnv env;
        for (const auto& gp : impl.genericParams)
            env[gp.name] = makeFreshVar();
        return env;
    }

    void bindTraitParamsForImpl(const ast::ImplDecl& impl, GenericEnv& env) {
        if (impl.traitName.empty()) return;
        auto pit = traitGenericParams_.find(impl.traitName);
        if (pit == traitGenericParams_.end()) return;
        const std::vector<std::string>& names = pit->second;
        const GenericEnv* saved = currentGenericEnv_;
        currentGenericEnv_ = &env; // so trait-args can see Self
        for (std::size_t i = 0;
             i < names.size() && i < impl.traitTypeArgs.size(); ++i) {
            env[names[i]] = resolveTypeRef(impl.traitTypeArgs[i]);
        }
        currentGenericEnv_ = saved;
    }

    std::string implMethodMangledName(const std::string& trait,
                                       const ast::TypeRef& forType,
                                       const std::string& method) {
        // Inherent impls carry an empty trait name. Mangle them under an
        // internal-only sentinel so they cannot collide with user traits.
        // Codegen mirrors this in `implMethodMangle`.
        const std::string t = trait.empty() ? kInherentImplSentinel : trait;
        return "__impl_" + t + "_for_" + forType.name + "__" + method;
    }

    void checkImplMethod(const ast::FnDecl& fn, const ast::ImplDecl& impl,
                         const TypePtr& selfTy) {
        auto it = implMethodMangled_.find(&fn);
        if (it == implMethodMangled_.end()) return;
        auto sit = fnSchemas_.find(it->second);
        if (sit == fnSchemas_.end()) return;
        const FnSchema& schema = sit->second;
        GenericEnv genEnv;
        // Phase 40: schema.genericVars is [impl params] ++ [fn params] (the
        // sig-resolution order) — bind both positionally so an impl param `T`
        // (with its bound) is in scope in the method body.
        std::size_t gvi = 0;
        for (const auto& gp : impl.genericParams) {
            if (gvi < schema.genericVars.size())
                genEnv[gp.name] = schema.genericVars[gvi++];
        }
        for (const auto& gp : fn.genericParams) {
            if (gvi < schema.genericVars.size())
                genEnv[gp.name] = schema.genericVars[gvi++];
        }
        // Phase 45/46: expose every generic param's bounds (impl params, then fn
        // params) BEFORE resolving forType / the body, so the method's container
        // ops — and the `Self = HashMap<K,V>` resolution itself — see a
        // `K: Hash + Eq` promise. This is why a generic-impl method may build,
        // query, or be defined `for HashMap<K,V>`.
        currentVarBound_.clear();
        currentVarAllBounds_.clear();
        {
            auto expose = [&](const ast::TypeParam& gp) {
                auto git = genEnv.find(gp.name);
                if (git == genEnv.end()) return;
                TypePtr v = resolve(git->second);
                if (v->kind == TypeKind::Var)
                    recordVarBounds(v->varId, gp.bound, gp.extraBounds);
            };
            for (const auto& gp : impl.genericParams) expose(gp);
            for (const auto& gp : fn.genericParams) expose(gp);
        }
        // Phase 40: for a generic impl, recompute Self from THIS env so it uses
        // the schema's impl-param Vars (the passed selfTy was resolved with
        // throwaway Vars). For a non-generic impl this is identical to selfTy.
        TypePtr selfTyLocal = selfTy;
        if (!impl.genericParams.empty()) {
            currentGenericEnv_ = &genEnv;
            selfTyLocal = resolveTypeRef(impl.forType);
        }
        genEnv["Self"] = selfTyLocal;
        // Phase 21a: bind the trait's generic params to this impl's concrete
        // trait-args (matches the schema-registration env), so a method body
        // referencing `T` checks against the impl's element type.
        bindTraitParamsForImpl(impl, genEnv);
        // Phase 10a: restore effect-row-var env so fn-typed params resolve
        // and effect collection can name still-polymorphic row vars.
        std::unordered_set<std::string> rowVarNames;
        currentEffectRowVarById_.clear();
        for (const auto& [name, var] : schema.effectRowVars) {
            rowVarNames.insert(name);
            genEnv[name] = var;
            TypePtr rv = resolve(var);
            if (rv->kind == TypeKind::Var)
                currentEffectRowVarById_[rv->varId] = name;
        }
        currentGenericEnv_ = &genEnv;
        currentEffectRowVarNames_ = &rowVarNames;
        currentFnIsAsync_ = fn.isAsync;
        pushScope();
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);
        // PR#25: reject `-> &T` reference return types. kardashev has no
        // lifetime system, so a returned reference can only ever borrow a
        // parameter or a local — and the latter is a guaranteed
        // use-after-free once the frame unwinds. Until lifetimes exist,
        // functions must return owned values. (`&[T]` slices are a separate
        // Struct kind and are not caught here.)
        if (resolve(currentReturnType_)->kind == TypeKind::Ref) {
            error("function '" + fn.name +
                      "' cannot return a reference (no lifetime system yet); "
                      "return an owned value instead",
                  fn.returnType.line, fn.returnType.column);
        }
        TypePtr bodyType = checkBlock(*fn.body);
        if (fn.body->tail) {
            if (closureEscapesByRef(*fn.body->tail)) {
                error("cannot return a closure that captures a variable by "
                      "reference (FnMut); its environment would point into the "
                      "dead stack frame",
                      fn.body->tail->line, fn.body->tail->column);
            }
            if (!coerceOrUnify(*fn.body->tail, bodyType, currentReturnType_)) {
                error("impl method '" + fn.name + "' body type " +
                          typeToString(bodyType) +
                          " does not match declared return type " +
                          typeToString(currentReturnType_),
                      fn.body->tail->line, fn.body->tail->column);
            }
        }
        popScope();
        currentReturnType_.reset();
        currentFnIsAsync_ = false;
        currentGenericEnv_ = nullptr;
        currentEffectRowVarNames_ = nullptr;
        currentEffectRowVarById_.clear();
        currentVarBound_.clear();
        currentVarAllBounds_.clear();
    }

    // Allocate a schema shell: an opaque Type (Struct or Enum kind, empty
    // body) plus one fresh schema Var per declared generic parameter.
    // Used in Pass 1a so cross-references can resolve names without
    // tripping on missing entries; bodies fill in fields/variants in Pass
    // 1b under the schema's generic env.
    template <typename Schema>
    Schema buildSchemaShell(const std::string& name,
                              const std::vector<ast::TypeParam>& genericParams,
                              bool isStruct) {
        Schema s;
        s.type = isStruct ? makeStruct(name, {}) : makeEnum(name, {});
        s.genericVars.reserve(genericParams.size());
        // One fresh Var per declared generic param, positionally; duplicate
        // names get one Var each (and we report the duplicate below) so the
        // genericVars vector stays index-aligned with `genericParams`.
        for (std::size_t i = 0; i < genericParams.size(); ++i) {
            s.genericVars.push_back(makeFreshVar());
        }
        // Reject obvious shadowing here (i64/bool plus already-bound types
        // are reported once for each generic param in the order they
        // appear).
        std::unordered_set<std::string> seenGp;
        for (const auto& gp : genericParams) {
            if (!seenGp.insert(gp.name).second) {
                error("duplicate generic parameter '" + gp.name +
                          "' on type '" + name + "'",
                      gp.line, gp.column);
            }
            if (gp.name == "i64" || gp.name == "bool") {
                error("generic parameter '" + gp.name +
                          "' shadows a built-in type",
                      gp.line, gp.column);
            }
        }
        return s;
    }

    GenericEnv buildGenericEnv(
        const std::vector<ast::TypeParam>& genericParams,
        const std::vector<TypePtr>& genericVars) {
        GenericEnv env;
        std::size_t n = std::min(genericParams.size(), genericVars.size());
        for (std::size_t i = 0; i < n; ++i) {
            env[genericParams[i].name] = genericVars[i];
        }
        return env;
    }

    // Build a fresh instantiation of a struct/enum schema. Each schema
    // genericVar is replaced by a fresh Var; the resulting Type has
    // typeArgs filled with those fresh Vars and field/variant payload
    // types substituted accordingly. Callers (struct lit, ctor, pattern)
    // unify the typeArgs / payloads of the freshly-instantiated Type with
    // their concrete expectations, so the fresh Vars resolve to the
    // intended concretes after unification.
    TypePtr freshInstantiateStruct(const StructSchema& schema) {
        if (schema.genericVars.empty()) return schema.type;
        std::unordered_map<int, TypePtr> subst;
        std::vector<TypePtr> args;
        args.reserve(schema.genericVars.size());
        for (const auto& gv : schema.genericVars) {
            TypePtr fresh = makeFreshVar();
            subst[gv->varId] = fresh;
            args.push_back(fresh);
        }
        TypePtr inst = instantiate(schema.type, subst);
        // `instantiate` already preserves typeArgs when struct field types
        // changed, but a schema's typeArgs are always empty. Set them now
        // so unification sees the fresh-instance identity.
        inst->typeArgs = std::move(args);
        return inst;
    }

    TypePtr freshInstantiateEnum(const EnumSchema& schema) {
        if (schema.genericVars.empty()) return schema.type;
        std::unordered_map<int, TypePtr> subst;
        std::vector<TypePtr> args;
        args.reserve(schema.genericVars.size());
        for (const auto& gv : schema.genericVars) {
            TypePtr fresh = makeFreshVar();
            subst[gv->varId] = fresh;
            args.push_back(fresh);
        }
        TypePtr inst = instantiate(schema.type, subst);
        inst->typeArgs = std::move(args);
        return inst;
    }

    // Instantiate a struct/enum schema with explicit concrete typeArgs
    // already in hand (e.g. from a `Vec<i64>` annotation). Returns a fresh
    // Type whose typeArgs are exactly the caller-supplied types and whose
    // fields/payloads have been substituted accordingly.
    TypePtr instantiateStructWithArgs(const StructSchema& schema,
                                       std::vector<TypePtr> typeArgs) {
        if (schema.genericVars.empty()) return schema.type;
        std::unordered_map<int, TypePtr> subst;
        for (std::size_t i = 0;
             i < schema.genericVars.size() && i < typeArgs.size(); ++i) {
            subst[schema.genericVars[i]->varId] = typeArgs[i];
        }
        TypePtr inst = instantiate(schema.type, subst);
        inst->typeArgs = std::move(typeArgs);
        return inst;
    }

    TypePtr instantiateEnumWithArgs(const EnumSchema& schema,
                                     std::vector<TypePtr> typeArgs) {
        if (schema.genericVars.empty()) return schema.type;
        std::unordered_map<int, TypePtr> subst;
        for (std::size_t i = 0;
             i < schema.genericVars.size() && i < typeArgs.size(); ++i) {
            subst[schema.genericVars[i]->varId] = typeArgs[i];
        }
        TypePtr inst = instantiate(schema.type, subst);
        inst->typeArgs = std::move(typeArgs);
        return inst;
    }

    // Phase 21b: resolve an associated-type projection `Base::Assoc`.
    //   - If `Base` resolves to a concrete type (e.g. `Self` inside an impl
    //     method, or a named struct/enum), find the trait among that type's
    //     impls that declares `Assoc` and return the impl's chosen type. This
    //     fully handles `Self::Item` (Self is concretely bound in impl methods).
    //   - If `Base` resolves to a trait-bounded generic param Var (`C::Item`),
    //     the concrete type isn't known until monomorphization: allocate a
    //     placeholder Var and record an AssocProjection for codegen to resolve.
    TypePtr resolveAssocProjection(const ast::TypeRef& tr) {
        // Resolve the base path segment via the active generic env (Self / a
        // generic param) or as a named type.
        TypePtr base;
        if (currentGenericEnv_) {
            auto git = currentGenericEnv_->find(tr.name);
            if (git != currentGenericEnv_->end()) base = git->second;
        }
        if (!base) {
            // Fall back to resolving `tr.name` as a standalone type name.
            ast::TypeRef baseRef;
            baseRef.name = tr.name;
            baseRef.line = tr.line;
            baseRef.column = tr.column;
            base = resolveTypeRef(baseRef);
        }
        TypePtr rb = resolve(base);
        if (rb->kind == TypeKind::Var) {
            // `C::Item`: find C's bound trait, confirm it declares `Item`, and
            // record a projection placeholder for codegen.
            auto bit = currentVarBound_.find(rb->varId);
            std::string traitName =
                bit != currentVarBound_.end() ? bit->second : std::string{};
            if (traitName.empty()) {
                error("associated type projection '" + tr.name + "::" +
                          tr.assocName +
                          "' requires a trait bound on '" + tr.name + "'",
                      tr.line, tr.column);
                return makeInt();
            }
            auto ait = traitAssocTypes_.find(traitName);
            bool declared =
                ait != traitAssocTypes_.end() &&
                std::find(ait->second.begin(), ait->second.end(),
                          tr.assocName) != ait->second.end();
            if (!declared) {
                error("trait '" + traitName + "' (bound of '" + tr.name +
                          "') has no associated type '" + tr.assocName + "'",
                      tr.line, tr.column);
                return makeInt();
            }
            TypePtr placeholder = makeFreshVar();
            assocProjections_[placeholder->varId] =
                AssocProjection{rb->varId, traitName, tr.assocName};
            return placeholder;
        }
        // Concrete base. Determine its type name, then find a trait among its
        // impls that declares `assocName` and look up the impl's choice.
        std::string typeName;
        if (rb->kind == TypeKind::Struct) typeName = rb->structName;
        else if (rb->kind == TypeKind::Enum) typeName = rb->enumName;
        else if (rb->kind == TypeKind::Int) typeName = "i64";
        else if (rb->kind == TypeKind::Float) typeName = "f64";  // Phase 44
        else if (rb->kind == TypeKind::Bool) typeName = "bool";
        else {
            error("associated type projection on unsupported base type " +
                      typeToString(base),
                  tr.line, tr.column);
            return makeInt();
        }
        auto tyIt = implAssocTypes_.find(typeName);
        if (tyIt != implAssocTypes_.end()) {
            // Prefer a trait that actually declares `assocName`.
            for (const auto& [traitName, table] : tyIt->second) {
                auto entry = table.find(tr.assocName);
                if (entry != table.end()) return entry->second;
            }
        }
        error("type '" + typeName + "' has no associated type '" +
                  tr.assocName + "' (no impl defines it)",
              tr.line, tr.column);
        return makeInt();
    }

    // Phase 24: resolve a type written in an `extern "C"` signature to the
    // kardashev-visible TypePtr that calls type-check against. The only
    // extern-specific spelling is `i32` (C `int`): it surfaces as a plain
    // kardashev `i64` so an i64 argument/result flows through transparently
    // (codegen narrows/widens at the call boundary). Everything else resolves
    // exactly like a normal type. Pointer-shaped arguments (`&String`,
    // `String`, `&[T]`, `&T`, `&mut T`) resolve to their usual kardashev
    // types and codegen lowers them to a C pointer at the boundary; we
    // validate here that any extern type is one codegen can actually map to a
    // C ABI value, so a misuse fails at the declaration with a clear message.
    TypePtr resolveExternType(const ast::TypeRef& tr, const std::string& fnName,
                              bool isReturn) {
        // `i32` is FFI-only sugar for "C int, value-compatible with i64".
        if (!tr.isRef && !tr.isFn && !tr.isSlice && !tr.isArray &&
            !tr.isTuple && tr.assocName.empty() && !tr.isDyn &&
            tr.name == "i32") {
            if (!tr.typeArgs.empty())
                error("i32 takes no type arguments", tr.line, tr.column);
            return makeInt();
        }
        TypePtr t = resolveTypeRef(tr);
        // Validate the resolved type is C-ABI representable. Allowed:
        //   Int / Bool (scalars), any Ref (-> C pointer), the built-in
        //   String / Slice value (-> data pointer), Unit (only as a return).
        TypePtr r = resolve(t);
        auto representable = [&](const TypePtr& ty) -> bool {
            TypePtr rr = resolve(ty);
            switch (rr->kind) {
            case TypeKind::Int:
            case TypeKind::Bool:
            case TypeKind::Ref: // any &T / &mut T / &[T] -> C pointer
                return true;
            case TypeKind::Unit:
                return isReturn; // void return is fine; a void *param* isn't
            case TypeKind::Struct:
                // Only the built-in String/Slice values have a defined C
                // mapping (pass the data pointer). Other structs by value have
                // no portable C ABI here.
                return rr->structName == "String" || rr->structName == "Slice";
            default:
                return false;
            }
        };
        if (!representable(r)) {
            error("type `" + typeToString(r) + "` is not supported in an "
                  "`extern \"C\"` signature for '" + fnName +
                  "' (allowed: i64, i32, bool, &T / &mut T / &[T] (C pointer), "
                  "String / &String (passes the data pointer)" +
                  std::string(isReturn ? ", or unit (void return)" : "") + ")",
                  tr.line, tr.column);
        }
        return t;
    }

    void registerExternFns(const ast::Program& program) {
        for (const auto& ef : program.externFns) {
            // ABI must be exactly "C" (only ABI supported this phase).
            if (ef.abi != "C") {
                error("unsupported ABI \"" + ef.abi +
                          "\" on `extern` fn '" + ef.name +
                          "' (only \"C\" is supported)",
                      ef.line, ef.column);
                // Continue: still register the schema so calls don't cascade
                // into "undefined function" errors on top of the ABI error.
            }
            // Collision handling: a user-declared extern that shadows a name
            // already in scope (a built-in like `print`/`malloc`, a user fn,
            // or another extern) is a hard error rather than a silent reuse —
            // the two could disagree on signature/effects.
            if (fnSchemas_.count(ef.name)) {
                error("`extern \"C\" fn` '" + ef.name +
                          "' collides with an existing function of the same "
                          "name (built-in, user fn, or another extern); rename "
                          "it or drop the redeclaration",
                      ef.line, ef.column);
                continue;
            }
            std::vector<TypePtr> argTypes;
            argTypes.reserve(ef.params.size());
            for (const auto& p : ef.params) {
                argTypes.push_back(
                    resolveExternType(p.type, ef.name, /*isReturn=*/false));
            }
            TypePtr ret =
                resolveExternType(ef.returnType, ef.name, /*isReturn=*/true);
            FnSchema schema;
            schema.signature = makeFunction(std::move(argTypes), ret);
            // Effects: default to `io` (opaque external call) unless the decl
            // wrote an explicit `! { ... }` row. An explicit `! { }` means the
            // user asserts the fn is pure. Only built-in concrete effects are
            // allowed (no effect-row variables on externs).
            if (ef.hasExplicitEffects) {
                for (const auto& l : ef.effects.labels) {
                    if (!isBuiltinEffect(l)) {
                        error("unknown effect label `" + l +
                                  "` on `extern \"C\" fn` '" + ef.name +
                                  "` (built-ins: alloc, io, panic, async, "
                                  "unwind)",
                              ef.effects.line, ef.effects.column);
                        continue;
                    }
                    schema.declaredEffects.add(l);
                }
            } else {
                schema.declaredEffects.add("io");
            }
            // Extern fns are reachable by bare name (no module path); mark pub
            // so a path-qualified call (rare) doesn't trip the visibility check.
            schema.isPub = true;
            fnSchemas_[ef.name] = std::move(schema);
        }
    }

    // Phase 28: is `tyIn` usable as a HashMap/HashSet key? i64 and String are
    // built-in hashable (the container hashes/compares them directly with
    // identity/FNV-1a + icmp/str_eq); a user struct or enum qualifies iff it
    // provides both an `impl Hash` and an `impl Eq`.
    bool keyIsHashable(const TypePtr& tyIn) {
        TypePtr ty = resolve(tyIn);
        if (ty->kind == TypeKind::Int) return true;
        if (ty->kind == TypeKind::Struct && ty->structName == "String")
            return true;
        // Phase 45: a still-generic key Var is hashable iff the body's bounds
        // promise it — `K: Hash + Eq`. This lets a generic fn/impl body build
        // and query a `HashMap<K,V>`; the concrete K (which DOES impl Hash+Eq,
        // checked at the call site against the bound) is substituted at
        // monomorphization, so codegen always sees a real hashable type.
        if (ty->kind == TypeKind::Var) {
            auto bit = currentVarAllBounds_.find(ty->varId);
            return bit != currentVarAllBounds_.end() &&
                   bit->second.count("Hash") && bit->second.count("Eq");
        }
        std::string name;
        if (ty->kind == TypeKind::Struct) name = ty->structName;
        else if (ty->kind == TypeKind::Enum) name = ty->enumName;
        else return false;
        auto it = implMethodByType_.find(name);
        if (it == implMethodByType_.end()) return false;
        return it->second.count("Hash") && it->second.count("Eq");
    }

    TypePtr resolveTypeRef(const ast::TypeRef& tr) {
        // Phase 13b: slice type `&[T]`. The `&` is part of the slice spelling
        // (a slice is its own fat-pointer borrow), so handle it before the
        // generic ref-peel below, which would otherwise wrap it in an extra
        // Ref. `typeArgs[0]` is the element type.
        if (tr.isSlice) {
            TypePtr elem =
                tr.typeArgs.empty() ? makeInt() : resolveTypeRef(tr.typeArgs[0]);
            return makeSlice(elem);
        }
        // Phase 22: a fixed-size array type `[T; N]`. The `&` (a reference to
        // an array) is handled by the generic ref-peel below — but since we
        // build the array type here we must apply the ref wrapper ourselves
        // when isRef is set on the array node.
        if (tr.isArray) {
            TypePtr elem =
                tr.typeArgs.empty() ? makeInt() : resolveTypeRef(tr.typeArgs[0]);
            // Phase 25: a const-expr length is evaluated at compile time. A
            // bare literal length (Phase 22) is already in `arrayLen`. A
            // negative / non-evaluable length is an error; length falls back
            // to 0 so resolution continues (the error is already reported).
            std::size_t len = tr.arrayLen;
            if (tr.arrayLenExpr) {
                std::int64_t n = 0;
                if (evalConstI64(*tr.arrayLenExpr, n)) {
                    if (n < 0) {
                        error("array length must be non-negative, got " +
                                  std::to_string(n),
                              tr.arrayLenExpr->line, tr.arrayLenExpr->column);
                        len = 0;
                    } else {
                        len = static_cast<std::size_t>(n);
                        // Cache the resolved length on the AST node so codegen
                        // (which re-resolves TypeRefs) reads it directly.
                        tr.arrayLen = len;
                    }
                } else {
                    len = 0;
                }
            }
            TypePtr arr = makeArray(elem, len);
            if (tr.isRef) return makeRef(arr, tr.refIsMut);
            return arr;
        }
        // Phase 22: a tuple type `(A, B, ...)`.
        if (tr.isTuple) {
            std::vector<TypePtr> elems;
            elems.reserve(tr.tupleElems.size());
            for (const auto& el : tr.tupleElems)
                elems.push_back(resolveTypeRef(el));
            TypePtr tup = makeTuple(std::move(elems));
            if (tr.isRef) return makeRef(tup, tr.refIsMut);
            return tup;
        }
        // Phase 2.4b: peel off the reference wrapper and wrap the inner
        // resolution. Recursive in case nested refs are introduced later.
        if (tr.isRef && !tr.isFn) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(resolveTypeRef(inner), tr.refIsMut);
        }
        // Phase 21b: an associated-type projection `Base::Assoc`.
        if (!tr.assocName.empty()) {
            return resolveAssocProjection(tr);
        }
        // Phase 11: `dyn Trait` — validate the trait exists and is object-
        // safe (dyn-safe), then build the unsized trait-object type. Only
        // ever appears behind a `&`/`Box` (the Ref peel above / Box branch
        // below handle the pointer); a bare `dyn Trait` value is rejected at
        // its use site by codegen since it never maps to a sized LLVM type.
        if (tr.isDyn) {
            auto it = traits_.find(tr.name);
            if (it == traits_.end()) {
                error("`dyn` of unknown trait '" + tr.name + "'",
                      tr.line, tr.column);
                return makeInt();
            }
            // Phase 21a: generic trait objects (`dyn Iterator<T>`) are NOT
            // supported this phase — monomorphization keeps generic-trait
            // method calls static. Reject with a clear message rather than
            // emitting an unsound vtable. Non-generic `dyn Trait` is unchanged.
            auto pit = traitGenericParams_.find(tr.name);
            if (pit != traitGenericParams_.end() && !pit->second.empty()) {
                error("`dyn " + tr.name + "` is not supported: trait '" +
                          tr.name +
                          "' has generic type parameters (generic trait "
                          "objects aren't supported; use a generic param with "
                          "a trait bound like `<I: " + tr.name + "<...>>` "
                          "instead)",
                      tr.line, tr.column);
                return makeInt();
            }
            checkObjectSafe(tr.name, tr.line, tr.column);
            return makeDyn(tr.name);
        }
        // Phase 11: `Box<T>` — the built-in heap-owned pointer. Built-in,
        // single type arg; the inner T may itself be `dyn Trait` (a heap
        // trait object). This only applies when the user hasn't declared
        // their own `Box` type (a user `struct Box<T>` shadows the built-in,
        // preserving existing programs that define one).
        if (tr.name == "Box" && !tr.isFn &&
            !structSchemas_.count("Box") && !enumSchemas_.count("Box")) {
            if (tr.typeArgs.size() != 1) {
                error("Box expects exactly 1 type argument, got " +
                          std::to_string(tr.typeArgs.size()),
                      tr.line, tr.column);
                return makeBox(makeInt());
            }
            return makeBox(resolveTypeRef(tr.typeArgs[0]));
        }
        // Phase 10a: a function type `fn(P...) -> R ! { effects }`. Build a
        // Function Type carrying the effect row. Concrete labels go in
        // `effectLabels`; a single effect-row variable (the polymorphic
        // tail) becomes the `effectRowVar`, resolved against the enclosing
        // fn's generic env. A `&fn(...)` reference wraps the result.
        if (tr.isFn) {
            std::vector<TypePtr> argTys;
            argTys.reserve(tr.fnParams.size());
            for (const auto& p : tr.fnParams) argTys.push_back(resolveTypeRef(p));
            TypePtr retTy = tr.fnRet ? resolveTypeRef(*tr.fnRet) : makeUnit();
            std::vector<std::string> labels;
            TypePtr rowVar;
            for (const auto& l : tr.fnEffects) {
                if (isBuiltinEffect(l)) {
                    if (std::find(labels.begin(), labels.end(), l) ==
                        labels.end())
                        labels.push_back(l);
                    continue;
                }
                bool isRowVar = currentEffectRowVarNames_ &&
                                currentEffectRowVarNames_->count(l) > 0;
                if (isRowVar && currentGenericEnv_) {
                    auto git = currentGenericEnv_->find(l);
                    if (git != currentGenericEnv_->end()) {
                        // All occurrences of the same row-var name in one
                        // signature share the schema Var, so the function-
                        // type rows are linked through unification.
                        rowVar = git->second;
                    }
                } else {
                    error("unknown effect label `" + l +
                              "` in function type (built-ins: alloc, io, "
                              "panic, async, unwind; or declare `" + l +
                              "` as a generic effect-row parameter)",
                          tr.line, tr.column);
                }
            }
            TypePtr fnTy = makeFunction(std::move(argTys), retTy,
                                        std::move(labels), rowVar);
            if (tr.isRef) return makeRef(fnTy, tr.refIsMut);
            return fnTy;
        }
        // Generic params from the enclosing fn/struct/enum decl.
        if (currentGenericEnv_) {
            auto git = currentGenericEnv_->find(tr.name);
            if (git != currentGenericEnv_->end()) {
                if (!tr.typeArgs.empty()) {
                    error("type parameter '" + tr.name +
                              "' cannot take type arguments",
                          tr.line, tr.column);
                }
                return git->second;
            }
        }
        if (tr.name == "i64") {
            if (!tr.typeArgs.empty())
                error("i64 takes no type arguments", tr.line, tr.column);
            return makeInt();
        }
        if (tr.name == "f64") {
            if (!tr.typeArgs.empty())
                error("f64 takes no type arguments", tr.line, tr.column);
            return makeFloat();
        }
        // Phase 16: `unit` is the type of a function with no `-> T` return
        // annotation (the parser synthesizes a "unit" TypeRef there). Maps to
        // the unit type — codegen lowers it to `void`.
        if (tr.name == "unit") {
            if (!tr.typeArgs.empty())
                error("unit takes no type arguments", tr.line, tr.column);
            return makeUnit();
        }
        if (tr.name == "bool") {
            if (!tr.typeArgs.empty())
                error("bool takes no type arguments", tr.line, tr.column);
            return makeBool();
        }
        std::vector<TypePtr> argTypes;
        argTypes.reserve(tr.typeArgs.size());
        for (const auto& a : tr.typeArgs) argTypes.push_back(resolveTypeRef(a));
        // Phase 28: `HashMap<K, V>` is generic over BOTH the key and the value.
        // The key K must be a hashable type: i64 or String (built-in Hash+Eq),
        // or a user struct/enum that impls both Hash and Eq. Both args are kept
        // (the schema is generic over K and V); codegen stores K in the bucket
        // entry and dispatches hashing/equality on K.
        if (tr.name == "HashMap" && argTypes.size() == 2) {
            if (!keyIsHashable(argTypes[0])) {
                error("HashMap key type must implement Hash + Eq (use i64, "
                      "String, or a user type with `impl Hash` and `impl Eq`), "
                      "got " + typeToString(argTypes[0]),
                      tr.line, tr.column);
            }
        }
        // Phase 28: a HashSet's element must be hashable too (it is the key).
        if (tr.name == "HashSet" && argTypes.size() == 1) {
            if (!keyIsHashable(argTypes[0])) {
                error("HashSet element type must implement Hash + Eq (use i64, "
                      "String, or a user type with `impl Hash` and `impl Eq`), "
                      "got " + typeToString(argTypes[0]),
                      tr.line, tr.column);
            }
        }
        if (auto sit = structSchemas_.find(tr.name);
            sit != structSchemas_.end()) {
            if (sit->second.genericVars.size() != argTypes.size()) {
                error("struct '" + tr.name + "' expects " +
                          std::to_string(sit->second.genericVars.size()) +
                          " type arg(s), got " +
                          std::to_string(argTypes.size()),
                      tr.line, tr.column);
                return sit->second.type;
            }
            return instantiateStructWithArgs(sit->second, std::move(argTypes));
        }
        if (auto eit = enumSchemas_.find(tr.name);
            eit != enumSchemas_.end()) {
            if (eit->second.genericVars.size() != argTypes.size()) {
                error("enum '" + tr.name + "' expects " +
                          std::to_string(eit->second.genericVars.size()) +
                          " type arg(s), got " +
                          std::to_string(argTypes.size()),
                      tr.line, tr.column);
                return eit->second.type;
            }
            return instantiateEnumWithArgs(eit->second, std::move(argTypes));
        }
        error("unknown type: " + tr.name, tr.line, tr.column);
        return makeInt(); // fallback so downstream code keeps running
    }

    TypePtr lookupLocal(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    }

    // Phase 9: scope helpers keep `mutScopes_` in lockstep with `scopes_`
    // so mutability lookups respect the same shadowing rules as types.
    void pushScope() {
        scopes_.push_back({});
        mutScopes_.push_back({});
        byRefClosureScopes_.push_back({});
    }
    void popScope() {
        scopes_.pop_back();
        if (!mutScopes_.empty()) mutScopes_.pop_back();
        if (!byRefClosureScopes_.empty()) byRefClosureScopes_.pop_back();
    }
    void markMut(const std::string& name) {
        if (!mutScopes_.empty()) mutScopes_.back().insert(name);
    }
    // True iff `name`'s nearest-enclosing binding was declared `let mut`.
    bool isMutLocal(const std::string& name) {
        // Walk scopes_ + mutScopes_ together from innermost out; the first
        // scope that defines `name` determines mutability (shadowing).
        std::size_t n = scopes_.size();
        for (std::size_t i = n; i-- > 0;) {
            if (scopes_[i].count(name)) {
                return i < mutScopes_.size() && mutScopes_[i].count(name) > 0;
            }
        }
        return false;
    }

    void markByRefClosureLocal(const std::string& name, bool byRef) {
        if (!byRefClosureScopes_.empty()) byRefClosureScopes_.back()[name] = byRef;
    }

    bool isByRefClosureLocal(const std::string& name) const {
        for (std::size_t i = byRefClosureScopes_.size(); i-- > 0;) {
            auto it = byRefClosureScopes_[i].find(name);
            if (it != byRefClosureScopes_[i].end()) return it->second;
        }
        return false;
    }


    // Look up a variant by name. Returns a fresh enum-instance Type
    // (typeArgs filled with fresh Vars) plus an index into that instance's
    // variants list. Callers unify the returned Type's typeArgs / payload
    // types with their concrete inputs; the fresh Vars then resolve to
    // the right concretes via the union-find chain. Returns
    // {nullptr, npos} on unknown name.
    struct VariantLookup {
        TypePtr enumInstance;
        unsigned variantIdx = static_cast<unsigned>(-1);
    };
    VariantLookup lookupVariant(const std::string& name) {
        auto it = variantIndex_.find(name);
        if (it == variantIndex_.end()) return {};
        auto schemaIt = enumSchemas_.find(it->second.first);
        if (schemaIt == enumSchemas_.end()) return {};
        const EnumSchema& schema = schemaIt->second;
        if (it->second.second >= schema.type->enumVariants.size()) return {};
        VariantLookup vl;
        vl.enumInstance = freshInstantiateEnum(schema);
        vl.variantIdx = it->second.second;
        return vl;
    }

    void checkFunction(const ast::FnDecl& fn) {
        auto sit = fnSchemas_.find(fn.name);
        if (sit == fnSchemas_.end()) return; // duplicate decl, schema absent
        const FnSchema& schema = sit->second;

        // Body-checking uses the SCHEMA Vars directly (not fresh
        // instantiation copies). This makes codegen's job tractable: every
        // Var the typechecker records in `exprTypes` and `callInstantiations`
        // either resolves to a concrete type or to a schema Var of the
        // enclosing fn — codegen can substitute the latter through the
        // instance's typeArgs.
        //
        // Trade-off: if a generic body inadvertently constrains a type
        // parameter (e.g. `fn id<T>(x: T) -> T { x + 1 }` unifies T with
        // i64), the schema is mutated and the fn silently becomes
        // monomorphic. That's caught at the next call site whose arg
        // doesn't unify with the now-concrete type. A dedicated check that
        // genericVars stay free after body-checking lands in Phase 3.3
        // alongside trait bounds.
        GenericEnv genEnv;
        for (std::size_t i = 0;
             i < fn.genericParams.size() && i < schema.genericVars.size();
             ++i) {
            genEnv[fn.genericParams[i].name] = schema.genericVars[i];
        }
        // Phase 10a: reconstruct the effect-row-var name set and the
        // Var-id -> name map so (a) `resolveTypeRef` builds fn-typed params
        // with the right row var, and (b) effect collection can recover a
        // row var's source name when it is still polymorphic in this body.
        std::unordered_set<std::string> rowVarNames;
        currentEffectRowVarById_.clear();
        for (const auto& [name, var] : schema.effectRowVars) {
            rowVarNames.insert(name);
            TypePtr rv = resolve(var);
            if (rv->kind == TypeKind::Var)
                currentEffectRowVarById_[rv->varId] = name;
        }
        currentGenericEnv_ = &genEnv;
        currentEffectRowVarNames_ = &rowVarNames;
        currentFnIsAsync_ = fn.isAsync;
        // Phase 21b: expose bounded params' traits so a `C::Item` projection in
        // the body / return type resolves C's bound (mirrors Pass 1b).
        currentVarBound_.clear();
        currentVarAllBounds_.clear();
        for (std::size_t i = 0;
             i < fn.genericParams.size() && i < schema.genericVars.size();
             ++i) {
            TypePtr v = resolve(schema.genericVars[i]);
            if (v->kind == TypeKind::Var)
                recordVarBounds(v->varId, fn.genericParams[i].bound,
                                fn.genericParams[i].extraBounds);
        }

        pushScope();
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);
        // PR#25: reject `-> &T` reference return types (top-level fns). No
        // lifetime system, so a returned reference borrows a param or a
        // local — the latter being a guaranteed use-after-free. Slices
        // (`&[T]`, a Struct kind) are not caught here.
        if (resolve(currentReturnType_)->kind == TypeKind::Ref) {
            error("function '" + fn.name +
                      "' cannot return a reference (no lifetime system yet); "
                      "return an owned value instead",
                  fn.returnType.line, fn.returnType.column);
        }

        TypePtr bodyType = checkBlock(*fn.body);
        // If the block has a tail expression, it must match the declared
        // return type. If not (body ends with a stmt — typically a
        // `return`), we rely on the per-`return` check inside checkStmt.
        if (fn.body->tail) {
            // Phase 17a: reject returning an FnMut closure (see ReturnStmt).
            if (closureEscapesByRef(*fn.body->tail)) {
                error("cannot return a closure that captures a variable by "
                      "reference (FnMut); its environment would point into the "
                      "dead stack frame",
                      fn.body->tail->line, fn.body->tail->column);
            }
            // Phase 11: coerce a thin-pointer tail into a `&dyn`/`Box<dyn>`
            // return type (otherwise plain unification).
            if (!coerceOrUnify(*fn.body->tail, bodyType, currentReturnType_)) {
                error("function '" + fn.name + "' body type " +
                          typeToString(bodyType) +
                          " does not match declared return type " +
                          typeToString(currentReturnType_),
                      fn.body->tail->line, fn.body->tail->column);
            }
        }
        popScope();
        currentReturnType_.reset();
        currentFnIsAsync_ = false;
        currentGenericEnv_ = nullptr;
        currentEffectRowVarNames_ = nullptr;
        currentEffectRowVarById_.clear();
        currentVarBound_.clear();
        currentVarAllBounds_.clear();
    }

    // --- Phase 25: the compile-time evaluator -------------------------------
    //
    // Evaluates a const-expr over the supported subset to an i64/bool value:
    // int/bool literals, the arithmetic/comparison binops, unary `-`/`!`,
    // `if/else`, `let`, and calls to `const fn`s (recursively). Anything else
    // (I/O, heap, loops, method calls, structs, ...) is a clear error. The
    // evaluator is bounded by depth (kConstEvalMaxDepth) and a global step
    // budget (kConstEvalMaxSteps); integer overflow and division/modulo by
    // zero are compile errors. Errors are thrown as ConstEvalError and caught
    // at the public entry points (evalConst / evalArrayLen) which turn them
    // into a typecheck diagnostic.

    [[noreturn]] void constFail(const std::string& msg, const ast::Expr& e) {
        throw ConstEvalError{msg, e.line, e.column};
    }

    void constTick(const ast::Expr& e) {
        if (++constEvalSteps_ > kConstEvalMaxSteps) {
            constFail("const evaluation exceeded the step budget (" +
                          std::to_string(kConstEvalMaxSteps) +
                          " steps) — possible non-terminating const fn",
                      e);
        }
    }

    // Evaluate a top-level const item by name, memoizing the result and
    // detecting cyclic references. Returns the ConstValue or throws.
    ConstValue evalConstByName(const std::string& name, const ast::Expr& site) {
        auto cached = constValues_.find(name);
        if (cached != constValues_.end()) return cached->second;
        auto declIt = constDecls_.find(name);
        if (declIt == constDecls_.end()) {
            constFail("'" + name + "' is not a constant", site);
        }
        if (!constEvalInProgress_.insert(name).second) {
            constFail("cyclic constant definition: '" + name +
                          "' depends on itself",
                      site);
        }
        ConstValue v = evalConstExpr(*declIt->second->value, /*env=*/nullptr,
                                     /*depth=*/0);
        constEvalInProgress_.erase(name);
        constValues_[name] = v;
        return v;
    }

    // The recursive evaluator. `env` (when non-null) maps in-scope local /
    // parameter names (inside a const fn body) to their evaluated values;
    // null means a top-level const initializer / array length (only other
    // consts + const-fn calls are reachable). `depth` bounds recursion.
    using ConstEnv = std::unordered_map<std::string, ConstValue>;

    ConstValue evalConstExpr(const ast::Expr& e, ConstEnv* env, int depth) {
        constTick(e);
        // `depth` counts const-fn CALL nesting (not AST-node nesting), so the
        // limit corresponds to the call-stack depth and lets a legitimately
        // deep-but-bounded recursion (e.g. a 100-deep countdown) run while
        // still catching true infinite recursion before it overflows the
        // native C++ stack. Intra-expression recursion below passes `depth`
        // unchanged; only `evalConstCall` increments it.
        if (depth > kConstEvalMaxDepth) {
            constFail("const evaluation exceeded the recursion-depth limit (" +
                          std::to_string(kConstEvalMaxDepth) +
                          " nested const-fn calls) — possible infinitely "
                          "recursive const fn",
                      e);
        }
        if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(&e)) {
            return {false, lit->value};
        }
        if (auto* bl = dynamic_cast<const ast::BoolLitExpr*>(&e)) {
            return {true, bl->value ? 1 : 0};
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            if (env) {
                auto it = env->find(id->name);
                if (it != env->end()) return it->second;
            }
            // Not a local — must be another const.
            return evalConstByName(id->name, e);
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            ConstValue v = evalConstExpr(*un->operand, env, depth);
            if (un->op == ast::UnaryOp::Neg) {
                if (v.isBool)
                    constFail("unary `-` requires i64 in a const expr", e);
                if (v.i == INT64_MIN)
                    constFail("integer overflow in const expr (negating "
                              "i64::MIN)",
                              e);
                return {false, -v.i};
            }
            // Not (bool -> bool).
            if (!v.isBool)
                constFail("unary `!` requires bool in a const expr", e);
            return {true, v.i ? 0 : 1};
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return evalConstBinary(*bin, env, depth);
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            ConstValue c = evalConstExpr(*ie->cond, env, depth);
            if (!c.isBool)
                constFail("`if` condition must be bool in a const expr", e);
            return evalConstExpr(c.i ? *ie->thenBranch : *ie->elseBranch, env,
                                 depth);
        }
        if (auto* blk = dynamic_cast<const ast::BlockExpr*>(&e)) {
            return evalConstBlock(*blk, env, depth);
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            return evalConstCall(*call, env, depth);
        }
        constFail("expression is not allowed in a const context (only int/"
                  "bool literals, arithmetic/comparison/unary operators, "
                  "`if`/`else`, `let`, and calls to `const fn`s are "
                  "const-evaluable)",
                  e);
    }

    ConstValue evalConstBinary(const ast::BinaryExpr& bin, ConstEnv* env,
                               int depth) {
        ConstValue l = evalConstExpr(*bin.lhs, env, depth);
        ConstValue r = evalConstExpr(*bin.rhs, env, depth);
        using Op = ast::BinOp;
        // Comparisons produce bool; arithmetic produces i64.
        switch (bin.op) {
        case Op::Eq:
            return {true, (l.i == r.i) ? 1 : 0};
        case Op::NotEq:
            return {true, (l.i != r.i) ? 1 : 0};
        case Op::And: // Phase 33: bool && bool (both operands treated as 0/1)
            return {true, (l.i != 0 && r.i != 0) ? 1 : 0};
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            if (l.isBool || r.isBool)
                constFail("ordering comparison requires i64 operands in a "
                          "const expr",
                          bin);
            bool b = false;
            if (bin.op == Op::Lt) b = l.i < r.i;
            else if (bin.op == Op::Le) b = l.i <= r.i;
            else if (bin.op == Op::Gt) b = l.i > r.i;
            else b = l.i >= r.i;
            return {true, b ? 1 : 0};
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
            if (l.isBool || r.isBool)
                constFail("arithmetic requires i64 operands in a const expr",
                          bin);
            std::int64_t out = 0;
            bool of = false;
            if (bin.op == Op::Add) of = __builtin_add_overflow(l.i, r.i, &out);
            else if (bin.op == Op::Sub)
                of = __builtin_sub_overflow(l.i, r.i, &out);
            else if (bin.op == Op::Mul)
                of = __builtin_mul_overflow(l.i, r.i, &out);
            else if (bin.op == Op::Mod) {
                if (r.i == 0) constFail("modulo by zero in const expr", bin);
                // i64::MIN % -1 is 0 (and would overflow in two's complement).
                out = (l.i == INT64_MIN && r.i == -1) ? 0 : (l.i % r.i);
            }
            else { // Div
                if (r.i == 0) constFail("division by zero in const expr", bin);
                // i64::MIN / -1 overflows.
                if (l.i == INT64_MIN && r.i == -1)
                    constFail("integer overflow in const expr (i64::MIN / -1)",
                              bin);
                out = l.i / r.i;
            }
            if (of) constFail("integer overflow in const expr", bin);
            return {false, out};
        }
        }
        constFail("unsupported operator in const expr", bin);
    }

    ConstValue evalConstBlock(const ast::BlockExpr& blk, ConstEnv* outer,
                              int depth) {
        // A block opens a fresh scope layered over the caller's env. Only
        // `let` (with a const-evaluable RHS) and a tail expression are
        // supported; a non-tail expr stmt with no effect is a no-op, but a
        // block whose tail is missing (unit value) can't yield an i64/bool.
        ConstEnv local = outer ? *outer : ConstEnv{};
        for (const auto& s : blk.stmts) {
            if (auto* let = dynamic_cast<const ast::LetStmt*>(s.get())) {
                if (!let->tupleNames.empty())
                    throw ConstEvalError{"tuple-destructuring `let` is not "
                                         "const-evaluable",
                                         let->line, let->column};
                if (!let->value)
                    throw ConstEvalError{"`let` without an initializer is not "
                                         "const-evaluable",
                                         let->line, let->column};
                ConstValue v = evalConstExpr(*let->value, &local, depth);
                local[let->name] = v;
            } else if (auto* ret =
                           dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                if (!ret->value)
                    throw ConstEvalError{"bare `return;` is not "
                                         "const-evaluable (a const fn must "
                                         "yield a value)",
                                         ret->line, ret->column};
                // An early `return e;` short-circuits the block's value.
                return evalConstExpr(*ret->value, &local, depth);
            } else if (auto* es =
                           dynamic_cast<const ast::ExprStmt*>(s.get())) {
                // Evaluate for its (absence of) effects; the value is dropped.
                // This also surfaces an error for a non-const-evaluable stmt.
                evalConstExpr(*es->expr, &local, depth);
            } else {
                throw ConstEvalError{"statement is not const-evaluable",
                                     s->line, s->column};
            }
        }
        if (!blk.tail)
            throw ConstEvalError{"a const block must end in a value "
                                 "expression",
                                 blk.line, blk.column};
        return evalConstExpr(*blk.tail, &local, depth);
    }

    ConstValue evalConstCall(const ast::CallExpr& call, ConstEnv* env,
                             int depth) {
        auto it = constFns_.find(call.callee);
        if (it == constFns_.end()) {
            constFail("call to '" + call.callee +
                          "' is not const-evaluable (only `const fn`s can be "
                          "called in a const context)",
                      call);
        }
        const ast::FnDecl& fn = *it->second;
        if (fn.params.size() != call.args.size()) {
            constFail("const fn '" + call.callee + "' expects " +
                          std::to_string(fn.params.size()) + " argument(s), "
                          "got " + std::to_string(call.args.size()),
                      call);
        }
        // Evaluate the arguments in the CALLER's env (same call depth), then
        // bind them by name into a fresh callee env (call-by-value). Entering
        // the callee BODY is the one place call depth increments.
        ConstEnv callee;
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            callee[fn.params[i].name] =
                evalConstExpr(*call.args[i], env, depth);
        }
        if (!fn.body) {
            constFail("const fn '" + call.callee + "' has no body", call);
        }
        return evalConstBlock(*fn.body, &callee, depth + 1);
    }

    // Public entry point: evaluate a const-expr that is required to be an
    // i64 (e.g. an array length). Catches ConstEvalError -> typecheck error,
    // returns false on failure (with `out` untouched).
    bool evalConstI64(const ast::Expr& e, std::int64_t& out) {
        try {
            constEvalSteps_ = 0;
            ConstValue v = evalConstExpr(e, /*env=*/nullptr, /*depth=*/0);
            if (v.isBool) {
                error("expected an i64 constant, got bool", e.line, e.column);
                return false;
            }
            out = v.i;
            constExprValues_[&e] = v;
            return true;
        } catch (const ConstEvalError& ce) {
            error(ce.message, ce.line, ce.column);
            return false;
        }
    }

    // Phase 25: type-check + evaluate a top-level `const NAME: T = init;`.
    // T must be i64 or bool (the MVP scalar set). The initializer is checked
    // for type agreement, then evaluated at compile time; the value is
    // memoized by name and recorded (keyed by the initializer expr) so codegen
    // emits it as a folded literal.
    void checkConstItem(const ast::ConstDecl& cd) {
        TypePtr declared = resolveTypeRef(cd.type);
        TypePtr rDeclared = resolve(declared);
        bool okType = rDeclared->kind == TypeKind::Int ||
                      rDeclared->kind == TypeKind::Bool;
        if (!okType) {
            error("a `const` must have type i64 or bool in this version, got " +
                      typeToString(declared),
                  cd.type.line, cd.type.column);
        }
        // Type-check the initializer expression (records exprTypes for codegen
        // + surfaces ordinary type errors), then unify with the declared type.
        if (cd.value) {
            TypePtr initTy = checkExpr(*cd.value);
            if (okType && !unify(initTy, declared)) {
                error("const initializer has type " + typeToString(initTy) +
                          ", but '" + cd.name + "' is declared " +
                          typeToString(declared),
                      cd.value->line, cd.value->column);
            }
        }
        // Compile-time evaluation (memoized by name; also records the folded
        // value at the initializer expr for codegen). Guarded so a cyclic /
        // already-failed const isn't re-reported here.
        if (constValues_.count(cd.name)) return;
        if (!cd.value) return;
        try {
            constEvalSteps_ = 0;
            ConstValue v = evalConstByName(cd.name, *cd.value);
            constExprValues_[cd.value.get()] = v;
        } catch (const ConstEvalError& ce) {
            // Make sure a half-finished evaluation doesn't wedge the
            // in-progress set for later look-ups.
            constEvalInProgress_.erase(cd.name);
            error(ce.message, ce.line, ce.column);
        }
    }

    TypePtr checkExpr(const ast::Expr& e) {
        TypePtr t = computeExprType(e);
        exprTypes_[&e] = t;
        return t;
    }

    TypePtr computeExprType(const ast::Expr& e) {
        if (dynamic_cast<const ast::IntLitExpr*>(&e)) {
            return makeInt();
        }
        if (dynamic_cast<const ast::FloatLitExpr*>(&e)) {
            return makeFloat();
        }
        if (dynamic_cast<const ast::BoolLitExpr*>(&e)) {
            return makeBool();
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            return checkUnary(*un);
        }
        if (dynamic_cast<const ast::StringLitExpr*>(&e)) {
            auto it = structSchemas_.find("String");
            return it != structSchemas_.end() ? it->second.type
                                              : makeStruct("String", {});
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            if (auto t = lookupLocal(id->name)) return t;
            // Phase 25: a use of a top-level `const` name. Resolve to the
            // const's declared type and record the folded value so codegen
            // emits a literal instead of a runtime load. A local of the same
            // name (above) intentionally shadows the const.
            auto cdIt = constDecls_.find(id->name);
            if (cdIt != constDecls_.end()) {
                TypePtr declared = resolveTypeRef(cdIt->second->type);
                try {
                    constEvalSteps_ = 0;
                    ConstValue v = evalConstByName(id->name, *id);
                    constExprValues_[id] = v;
                } catch (const ConstEvalError& ce) {
                    // The error is reported once at the const's own definition
                    // site (checkConstItem); avoid a duplicate here, but still
                    // give the use site a sensible type.
                }
                return declared;
            }
            auto fnIt = fnSchemas_.find(id->name);
            if (fnIt != fnSchemas_.end()) {
                // Bare Ident referring to a fn name used as a first-class
                // value (Phase 4.3). Instantiate so callers never see the
                // raw schema Vars escape, and (Phase 10a) attach the fn's
                // declared effects as the value's Function-type effect row,
                // so the effects survive being passed around / stored.
                std::unordered_map<int, TypePtr> subst;
                for (const auto& gv : fnIt->second.genericVars) {
                    subst[gv->varId] = makeFreshVar();
                }
                TypePtr sig = instantiate(fnIt->second.signature, subst);
                return attachDeclaredEffects(sig, fnIt->second);
            }
            // Fall through to variant table: a bare Ident resolving to a
            // unit constructor is the value of that constructor.
            auto vl = lookupVariant(id->name);
            if (vl.enumInstance) {
                const auto& variant =
                    vl.enumInstance->enumVariants[vl.variantIdx];
                if (!variant.payloadTypes.empty()) {
                    error("constructor " + id->name + " requires " +
                              std::to_string(variant.payloadTypes.size()) +
                              " argument(s)",
                          e.line, e.column);
                    return makeInt();
                }
                return vl.enumInstance;
            }
            error("unknown identifier: " + id->name, e.line, e.column);
            return makeInt();
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return checkBinary(*bin);
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            return checkCall(*call);
        }
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            return checkCallValue(*cv);
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            return checkIf(*ie);
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            return checkBlock(*block);
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            return checkStructLit(*sl);
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            return checkField(*fe);
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            return checkMatch(*me);
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            return checkWhile(*we);
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            return checkLoop(*le);
        }
        if (auto* fe = dynamic_cast<const ast::ForExpr*>(&e)) {
            return checkFor(*fe);
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            return checkRange(*re);
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e)) {
            return checkBreak(*be);
        }
        if (auto* ce = dynamic_cast<const ast::ContinueExpr*>(&e)) {
            return checkContinue(*ce);
        }
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            return checkClosure(*cl);
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            return checkTry(*te);
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            return checkMethodCall(*mc);
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            // Phase 11: `Box::new(v)` heap-allocates and produces `Box<T>`.
            TypePtr inner = checkExpr(*bn->value);
            return makeBox(inner);
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            // Phase 2.4b: `&x` produces `&T` where T is x's type. The
            // operand is normally a bare Ident (we don't yet support
            // borrowing temporaries — would require a stack-spill rule).
            TypePtr inner = checkExpr(*re->operand);
            return makeRef(inner, re->isMut);
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            return checkSlice(*se);
        }
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            return checkArrayLit(*al);
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            return checkIndex(*ix);
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            return checkTupleLit(*tl);
        }
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e)) {
            return checkTupleField(*tf);
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            // Phase 12 / 17b: `expr.await` requires its operand to be a
            // `Future<T>` (the poll/frame pair async fns and `yield_now`
            // produce) and yields `T` — the future's result type, read from
            // `typeArgs[0]`. `.await` is only legal inside an `async fn`: it
            // suspends the enclosing future by returning Pending to the
            // caller, which only exists if the enclosing fn IS a future.
            TypePtr opTy = checkExpr(*ae->operand);
            TypePtr r = resolve(opTy);
            if (r->kind != TypeKind::Struct || r->structName != "Future") {
                error("`.await` requires a Future value, got " +
                          typeToString(opTy),
                      ae->line, ae->column);
                return makeInt();
            }
            if (!currentFnIsAsync_) {
                error("`.await` is only allowed inside an `async fn`",
                      ae->line, ae->column);
            }
            // The result type is the future's T. Fall back to i64 if a bare
            // (typeArgs-less) Future leaks through (defensive — all Future
            // values now carry their result type).
            if (!r->typeArgs.empty()) return r->typeArgs[0];
            return makeInt();
        }
        error("unknown expression kind", e.line, e.column);
        return makeInt();
    }

    // Phase 11: dyn-safety (object-safety) check. A trait may be used as
    // `dyn Trait` only if every method takes a `self` receiver (no static /
    // associated fns) and no method is itself generic — otherwise a single
    // vtable slot couldn't name one concrete fn. Reports one error per
    // offending method; `traits_` already verified the `self`-first rule at
    // registration, but generic methods and any future no-self methods are
    // caught here so the diagnostic points at the `dyn` use site.
    void checkObjectSafe(const std::string& traitName, std::size_t line,
                         std::size_t col) {
        auto it = traits_.find(traitName);
        if (it == traits_.end()) return;
        // Report once per trait — `resolveTypeRef` may revisit a `dyn Trait`
        // annotation across passes (signature registration + body checking).
        if (!dynSafetyReported_.insert(traitName).second) return;
        for (const auto& m : it->second) {
            bool hasSelf = !m.params.empty() && m.params[0].name == "self";
            if (!hasSelf) {
                error("trait '" + traitName + "' is not dyn-safe: method '" +
                          m.name +
                          "' has no `self` receiver (static methods can't be "
                          "dispatched through a trait object)",
                      line, col);
            }
            // MethodSig has no generic-param list in the grammar today, so a
            // generic trait method isn't expressible; the rule is stated here
            // so it's enforced the moment generic methods land. (Kept as a
            // one-line note per the dyn-safety requirement.)
        }
    }

    // Phase 11: the trait-object coercion site logic. If `expected` is a
    // `&dyn Trait` / `Box<dyn Trait>` and `actual` is a matching thin pointer
    // (`&Concrete` / `Box<Concrete>`) whose pointee impls the trait, record a
    // DynCoercion on `srcExpr` and return true (no unification needed — the
    // shapes deliberately differ). Otherwise fall back to plain `unify`.
    bool coerceOrUnify(const ast::Expr& srcExpr, const TypePtr& actual,
                       const TypePtr& expected) {
        TypePtr e = resolve(expected);
        TypePtr a = resolve(actual);
        // Unwrap one pointer layer on each side, tracking whether it's a Box.
        bool expIsBox = e->kind == TypeKind::Box;
        bool expIsRef = e->kind == TypeKind::Ref;
        if (expIsBox || expIsRef) {
            TypePtr inner = resolve(e->refInner);
            if (inner->kind == TypeKind::Dyn) {
                bool actIsBox = a->kind == TypeKind::Box;
                bool actIsRef = a->kind == TypeKind::Ref;
                // Box<dyn> accepts Box<Concrete>; &dyn accepts &Concrete.
                if ((expIsBox && actIsBox) || (expIsRef && actIsRef)) {
                    TypePtr pointee = resolve(a->refInner);
                    std::string typeName = concreteTypeName(pointee);
                    if (!typeName.empty() &&
                        typeImplsTrait(typeName, inner->dynTraitName)) {
                        DynCoercion c;
                        c.traitName = inner->dynTraitName;
                        c.concreteTypeName = typeName;
                        c.isBox = expIsBox;
                        dynCoercions_[&srcExpr] = c;
                        requireVtable(inner->dynTraitName, typeName);
                        return true;
                    }
                }
                // Already a trait object of the right trait: identity.
                if (a->kind == e->kind) {
                    TypePtr ai = resolve(a->refInner);
                    if (ai->kind == TypeKind::Dyn &&
                        ai->dynTraitName == inner->dynTraitName) {
                        return true;
                    }
                }
            }
        }
        return unify(actual, expected);
    }

    // Phase 13a: classify how a method takes `self` from its first-arg type,
    // so the borrow checker can autoref the receiver place. A `&mut Self` slot
    // is ByMutRef, a `&Self` slot is ByRef, anything else (`Self`) ByValue.
    ResolvedMethod::SelfKind selfKindFromSlot(const TypePtr& slot) {
        if (!slot) return ResolvedMethod::SelfKind::ByValue;
        TypePtr s = resolve(slot);
        if (s->kind == TypeKind::Ref) {
            return s->refIsMut ? ResolvedMethod::SelfKind::ByMutRef
                               : ResolvedMethod::SelfKind::ByRef;
        }
        return ResolvedMethod::SelfKind::ByValue;
    }

    // Phase 13a: classify the self kind directly from a parsed method
    // signature's first param (used for trait-driven dispatch — dyn and
    // bounded-generic — where we have the MethodSig rather than a resolved
    // self slot). The parser lowers `&self`/`&mut self` to a `self: Self`
    // param with isRef / refIsMut set on the TypeRef.
    ResolvedMethod::SelfKind selfKindFromSig(const ast::MethodSig& sig) {
        if (sig.params.empty()) return ResolvedMethod::SelfKind::ByValue;
        const ast::TypeRef& t = sig.params[0].type;
        if (t.isRef) {
            return t.refIsMut ? ResolvedMethod::SelfKind::ByMutRef
                              : ResolvedMethod::SelfKind::ByRef;
        }
        return ResolvedMethod::SelfKind::ByValue;
    }

    // Phase 11: unify a receiver against an impl method's `self` slot,
    // tolerating one level of ref difference (autoref / autoderef). A `&self`
    // method's self slot is `&Concrete`; a by-value `self` method's is
    // `Concrete`. We accept a value receiver for a `&self` method and a ref
    // receiver for a by-value method by peeling the extra `&` on whichever
    // side has it. (Existing Phase 2.4b auto-deref already let `&T` receivers
    // call by-value methods at codegen; this keeps the type check in step.)
    bool unifySelf(const TypePtr& recvT, const TypePtr& selfSlot) {
        TypePtr r = resolve(recvT);
        TypePtr s = resolve(selfSlot);
        if (unify(r, s)) return true;
        // Peel a single pointer layer (Ref or Box) on the receiver so a
        // `Box<T>` / `&T` receiver matches a `T` (or `&T`) self slot.
        if ((r->kind == TypeKind::Ref || r->kind == TypeKind::Box) &&
            unifySelf(r->refInner, s)) {
            return true;
        }
        if (s->kind == TypeKind::Ref && r->kind != TypeKind::Ref) {
            return unify(r, s->refInner);
        }
        return false;
    }

    // The base type name for a concrete (sized) type, or "" if it isn't one
    // we can impl a trait on.
    std::string concreteTypeName(const TypePtr& t) {
        TypePtr r = resolve(t);
        if (r->kind == TypeKind::Struct) return r->structName;
        if (r->kind == TypeKind::Enum) return r->enumName;
        if (r->kind == TypeKind::Int) return "i64";
        else if (r->kind == TypeKind::Float) return "f64";  // Phase 44
        if (r->kind == TypeKind::Bool) return "bool";
        return {};
    }

    bool typeImplsTrait(const std::string& typeName,
                        const std::string& traitName) {
        auto it = implMethodByType_.find(typeName);
        if (it == implMethodByType_.end()) return false;
        return it->second.count(traitName) > 0;
    }

    void requireVtable(const std::string& traitName,
                       const std::string& typeName) {
        std::string key = traitName + "/" + typeName;
        if (dynVtableSeen_.insert(key).second) {
            dynVtablesNeeded_.emplace_back(traitName, typeName);
        }
    }

    // Phase 3.3: resolve `recv.method(args)` against trait impls.
    TypePtr checkMethodCall(const ast::MethodCallExpr& mc) {
        TypePtr recvT = checkExpr(*mc.receiver);
        TypePtr r = resolve(recvT);

        // Phase 11: dynamic dispatch. If the receiver is `&dyn Trait` or
        // `Box<dyn Trait>` (one pointer layer over a Dyn), the call goes
        // through the object's vtable at runtime instead of resolving to a
        // single impl. We type-check args against the *trait's* method
        // signature (Self = the receiver's trait-object type).
        if (r->kind == TypeKind::Ref || r->kind == TypeKind::Box) {
            TypePtr inner = resolve(r->refInner);
            if (inner->kind == TypeKind::Dyn) {
                return checkDynMethodCall(mc, inner->dynTraitName, recvT);
            }
        }
        // A bare `dyn Trait` value can't reach here normally (it's unsized),
        // but guard defensively.
        if (r->kind == TypeKind::Dyn) {
            return checkDynMethodCall(mc, r->dynTraitName, recvT);
        }

        // Phase 2.4b auto-deref: if the receiver is `&T` (or `&mut T`),
        // dispatch as though the receiver were the underlying `T`. Phase
        // 2.4c will refine this so impls of `Trait for &T` (when written
        // explicitly) take precedence over implicit deref. Phase 11: a
        // `Box<Concrete>` derefs the same way (method call through the box).
        while (r->kind == TypeKind::Ref || r->kind == TypeKind::Box)
            r = resolve(r->refInner);

        // Case A: receiver is a generic-param Var. The fn must have a
        // trait bound for this Var, the bound trait must declare the
        // method, and codegen will route to the correct impl per
        // monomorphic instance.
        if (r->kind == TypeKind::Var) {
            int rId = r->varId;
            // Phase 28: gather ALL of the var's bounds — the primary bound
            // (with its Phase-21a parameterized trait-args, e.g. the `<T>` in
            // `<I: Iterator<T>>`) plus any extra bounds (`T: A + B`, which are
            // non-parameterized). A method call resolves against whichever
            // bound trait declares it.
            std::vector<std::string> candBounds;
            std::vector<std::vector<TypePtr>> candArgs;
            for (const auto& [_n, schema] : fnSchemas_) {
                bool hit = false;
                for (std::size_t i = 0; i < schema.genericVars.size(); ++i) {
                    if (schema.genericVars[i]->varId != rId) continue;
                    if (i < schema.genericBounds.size() &&
                        !schema.genericBounds[i].empty()) {
                        candBounds.push_back(schema.genericBounds[i]);
                        candArgs.push_back(
                            i < schema.genericBoundArgs.size()
                                ? schema.genericBoundArgs[i]
                                : std::vector<TypePtr>{});
                    }
                    if (i < schema.genericExtraBounds.size()) {
                        for (const auto& eb : schema.genericExtraBounds[i]) {
                            if (eb.empty()) continue;
                            candBounds.push_back(eb);
                            candArgs.emplace_back();
                        }
                    }
                    hit = true;
                    break;
                }
                if (hit) break;
            }
            if (candBounds.empty()) {
                error("method call on unbounded generic parameter; add a "
                      "trait bound like `<T: Trait>`",
                      mc.line, mc.column);
                for (const auto& a : mc.args) checkExpr(*a);
                return makeFreshVar();
            }
            // Find the bound trait that declares the called method.
            const ast::MethodSig* sig = nullptr;
            std::string bound;
            std::vector<TypePtr> boundArgs;
            for (std::size_t k = 0; k < candBounds.size(); ++k) {
                auto traitIt = traits_.find(candBounds[k]);
                if (traitIt == traits_.end()) continue; // reported in pass 1a
                for (const auto& m : traitIt->second) {
                    if (m.name == mc.methodName) {
                        sig = &m;
                        bound = candBounds[k];
                        boundArgs = candArgs[k];
                        break;
                    }
                }
                if (sig) break;
            }
            if (!sig) {
                std::string boundsList;
                for (std::size_t k = 0; k < candBounds.size(); ++k) {
                    if (k) boundsList += " + ";
                    boundsList += candBounds[k];
                }
                error("no method '" + mc.methodName +
                          "' in the bound(s) '" + boundsList +
                          "' of the generic parameter",
                      mc.line, mc.column);
                return makeFreshVar();
            }
            // Type-check arg count + arg types against the trait's
            // signature, substituting Self -> r (the schema Var) and the
            // trait's generic params -> the bound's resolved args.
            return checkMethodCallAgainstSig(mc, *sig, r,
                                              /*concrete=*/false, bound,
                                              /*concreteTypeName=*/{}, rId,
                                              boundArgs);
        }

        // Case B: receiver has a concrete type (struct/enum/i64/bool).
        std::string typeName;
        if (r->kind == TypeKind::Struct) typeName = r->structName;
        else if (r->kind == TypeKind::Enum) typeName = r->enumName;
        else if (r->kind == TypeKind::Int) typeName = "i64";
        else if (r->kind == TypeKind::Float) typeName = "f64";  // Phase 44
        else if (r->kind == TypeKind::Bool) typeName = "bool";
        else {
            error("method call on unsupported receiver type " +
                      typeToString(recvT),
                  mc.line, mc.column);
            return makeFreshVar();
        }
        auto typeIt = methodImplLookup_.find(typeName);
        if (typeIt == methodImplLookup_.end()) {
            error("no impl for type '" + typeName + "' (method '" +
                      mc.methodName + "')",
                  mc.line, mc.column);
            return makeFreshVar();
        }
        auto methodIt = typeIt->second.find(mc.methodName);
        if (methodIt == typeIt->second.end()) {
            error("no impl provides method '" + mc.methodName +
                      "' for type '" + typeName + "'",
                  mc.line, mc.column);
            return makeFreshVar();
        }
        // The trait that supplies this method.
        const std::string& trait = methodIt->second.first;
        // Get the impl method's signature via its FnSchema (mangled name).
        ast::TypeRef forTypeRef;
        forTypeRef.name = typeName;
        std::string mangled =
            implMethodMangledName(trait, forTypeRef, mc.methodName);
        auto schemaIt = fnSchemas_.find(mangled);
        if (schemaIt == fnSchemas_.end()) {
            error("internal: missing impl method schema for " + mangled,
                  mc.line, mc.column);
            return makeFreshVar();
        }
        // Re-use the fn-call instantiation path so generic impl methods
        // (Phase 3.3 doesn't have them yet) would route through the same
        // schema mechanics.
        const FnSchema& schema = schemaIt->second;
        std::unordered_map<int, TypePtr> subst;
        for (const auto& gv : schema.genericVars) {
            subst[gv->varId] = makeFreshVar();
        }
        TypePtr instSig = instantiate(schema.signature, subst);
        // Unify receiver against the impl's `self` slot (first arg). Phase 11:
        // with `&self` methods the self slot is `&ConcreteType`; autoref/
        // autoderef one pointer layer so `value.method()` and `(&value)
        // .method()` both type-check against either receiver convention.
        if (!instSig->args.empty()) {
            if (!unifySelf(recvT, instSig->args[0])) {
                error("receiver type " + typeToString(recvT) +
                          " doesn't unify with impl `self` type " +
                          typeToString(instSig->args[0]),
                      mc.line, mc.column);
            }
        }
        const std::size_t expectedExtra =
            instSig->args.empty() ? 0 : instSig->args.size() - 1;
        if (expectedExtra != mc.args.size()) {
            error("method '" + mc.methodName + "' expects " +
                      std::to_string(expectedExtra) + " arg(s), got " +
                      std::to_string(mc.args.size()),
                  mc.line, mc.column);
        }
        const std::size_t n =
            std::min(expectedExtra, mc.args.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr argT = checkExpr(*mc.args[i]);
            if (!unify(argT, instSig->args[i + 1])) {
                error("argument " + std::to_string(i + 1) + " to method '" +
                          mc.methodName + "' has type " +
                          typeToString(argT) + ", expected " +
                          typeToString(instSig->args[i + 1]),
                      mc.args[i]->line, mc.args[i]->column);
            }
        }
        for (std::size_t i = n; i < mc.args.size(); ++i) {
            checkExpr(*mc.args[i]);
        }
        ResolvedMethod res;
        res.kind = ResolvedMethod::Concrete;
        res.traitName = trait;
        res.methodName = mc.methodName;
        res.concreteTypeName = typeName;
        res.receiverTypeArgs = r->typeArgs;
        res.selfKind = instSig->args.empty()
                           ? ResolvedMethod::SelfKind::ByValue
                           : selfKindFromSlot(instSig->args[0]);
        methodResolutions_[&mc] = std::move(res);
        return instSig->ret;
    }

    TypePtr checkMethodCallAgainstSig(const ast::MethodCallExpr& mc,
                                       const ast::MethodSig& sig,
                                       const TypePtr& receiverTy,
                                       bool concrete,
                                       const std::string& traitName,
                                       const std::string& concreteTypeName,
                                       int boundedVarId,
                                       const std::vector<TypePtr>& traitArgs =
                                           {}) {
        // Resolve sig's param/return types with Self -> receiverTy. Phase 21a:
        // also bind the bound trait's generic params to the resolved trait
        // args, so a method returning `Option<T>` (from `Iterator<T>`)
        // resolves `T` to the caller's own type param. Save/restore the
        // active env so we don't clobber the enclosing fn's generic env
        // (needed below to checkExpr the args).
        GenericEnv selfEnv;
        selfEnv["Self"] = receiverTy;
        auto pit = traitGenericParams_.find(traitName);
        if (pit != traitGenericParams_.end()) {
            const std::vector<std::string>& names = pit->second;
            for (std::size_t i = 0;
                 i < names.size() && i < traitArgs.size(); ++i) {
                selfEnv[names[i]] = traitArgs[i];
            }
        }
        const GenericEnv* savedEnv = currentGenericEnv_;
        currentGenericEnv_ = &selfEnv;
        std::vector<TypePtr> paramTypes;
        for (const auto& p : sig.params) {
            paramTypes.push_back(resolveTypeRef(p.type));
        }
        TypePtr retTy = resolveTypeRef(sig.returnType);
        // Restore the enclosing fn's generic env (not nullptr) so that
        // checkExpr on the args below can still resolve names that mention
        // the caller's own type params.
        currentGenericEnv_ = savedEnv;

        // paramTypes[0] is self (already a Self type). Skip and unify args.
        const std::size_t expectedExtra =
            paramTypes.empty() ? 0 : paramTypes.size() - 1;
        if (expectedExtra != mc.args.size()) {
            error("method '" + mc.methodName + "' expects " +
                      std::to_string(expectedExtra) + " arg(s), got " +
                      std::to_string(mc.args.size()),
                  mc.line, mc.column);
        }
        const std::size_t n =
            std::min(expectedExtra, mc.args.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr argT = checkExpr(*mc.args[i]);
            if (!unify(argT, paramTypes[i + 1])) {
                error("argument " + std::to_string(i + 1) + " to method '" +
                          mc.methodName + "' has type " +
                          typeToString(argT) + ", expected " +
                          typeToString(paramTypes[i + 1]),
                      mc.args[i]->line, mc.args[i]->column);
            }
        }
        for (std::size_t i = n; i < mc.args.size(); ++i) {
            checkExpr(*mc.args[i]);
        }
        ResolvedMethod res;
        res.kind = concrete ? ResolvedMethod::Concrete
                            : ResolvedMethod::BoundedGeneric;
        res.traitName = traitName;
        res.methodName = mc.methodName;
        res.concreteTypeName = concreteTypeName;
        res.boundedVarId = boundedVarId;
        res.selfKind = selfKindFromSig(sig);
        methodResolutions_[&mc] = std::move(res);
        return retTy;
    }

    // Phase 11: type-check a call on a `&dyn Trait` / `Box<dyn Trait>`
    // receiver. The method is looked up by name in the trait (declaration
    // order gives the vtable slot); args are checked against the trait
    // signature with `Self` bound to the trait-object type, and a `Dyn`
    // ResolvedMethod records the slot for codegen's indexed vtable call. The
    // method's declared return type (with Self -> dyn) is the call's type.
    TypePtr checkDynMethodCall(const ast::MethodCallExpr& mc,
                               const std::string& traitName,
                               const TypePtr& dynRecvTy) {
        auto traitIt = traits_.find(traitName);
        if (traitIt == traits_.end()) {
            error("internal: dyn dispatch on unknown trait '" + traitName + "'",
                  mc.line, mc.column);
            return makeFreshVar();
        }
        const ast::MethodSig* sig = nullptr;
        int slot = -1;
        for (std::size_t i = 0; i < traitIt->second.size(); ++i) {
            if (traitIt->second[i].name == mc.methodName) {
                sig = &traitIt->second[i];
                slot = static_cast<int>(i);
                break;
            }
        }
        if (!sig) {
            error("trait '" + traitName + "' has no method '" +
                      mc.methodName + "' (called on a trait object)",
                  mc.line, mc.column);
            for (const auto& a : mc.args) checkExpr(*a);
            return makeFreshVar();
        }
        // Resolve the signature's param/return types with Self -> the
        // trait-object type, so a method returning Self stays a trait object
        // and any `&self`/`self` receiver is irrelevant to the args.
        GenericEnv selfEnv;
        selfEnv["Self"] = dynRecvTy;
        const GenericEnv* savedEnv = currentGenericEnv_;
        currentGenericEnv_ = &selfEnv;
        std::vector<TypePtr> paramTypes;
        for (const auto& p : sig->params) {
            paramTypes.push_back(resolveTypeRef(p.type));
        }
        TypePtr retTy = resolveTypeRef(sig->returnType);
        currentGenericEnv_ = savedEnv;

        const std::size_t expectedExtra =
            paramTypes.empty() ? 0 : paramTypes.size() - 1;
        if (expectedExtra != mc.args.size()) {
            error("method '" + mc.methodName + "' expects " +
                      std::to_string(expectedExtra) + " arg(s), got " +
                      std::to_string(mc.args.size()),
                  mc.line, mc.column);
        }
        const std::size_t n = std::min(expectedExtra, mc.args.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr argT = checkExpr(*mc.args[i]);
            if (!unify(argT, paramTypes[i + 1])) {
                error("argument " + std::to_string(i + 1) + " to method '" +
                          mc.methodName + "' has type " +
                          typeToString(argT) + ", expected " +
                          typeToString(paramTypes[i + 1]),
                      mc.args[i]->line, mc.args[i]->column);
            }
        }
        for (std::size_t i = n; i < mc.args.size(); ++i) checkExpr(*mc.args[i]);

        ResolvedMethod res;
        res.kind = ResolvedMethod::Dyn;
        res.traitName = traitName;
        res.methodName = mc.methodName;
        res.dynMethodSlot = slot;
        res.selfKind = selfKindFromSig(*sig);
        methodResolutions_[&mc] = std::move(res);
        return retTy;
    }

    // Helper: find a variant by name in an enum's variant list. Returns
    // nullptr if not present.
    const EnumVariantType* findVariant(const TypePtr& enumType,
                                        const std::string& name) {
        for (const auto& v : enumType->enumVariants) {
            if (v.name == name) return &v;
        }
        return nullptr;
    }

    // Phase 3.4: `expr?` requires the operand to be a Result-shape enum
    // (variants `Ok(T)` and `Err(E)`, each with a single payload) and the
    // enclosing fn to return a Result-shape enum whose `Err` payload type
    // unifies with the operand's. The TryExpr evaluates to the operand's
    // `Ok` payload type when it doesn't early-return.
    TypePtr checkTry(const ast::TryExpr& te) {
        TypePtr operandT = checkExpr(*te.operand);
        TypePtr ro = resolve(operandT);
        if (ro->kind != TypeKind::Enum) {
            error("`?` requires a Result-shaped enum, got " +
                      typeToString(operandT),
                  te.line, te.column);
            return makeInt();
        }
        const EnumVariantType* okV = findVariant(ro, "Ok");
        const EnumVariantType* errV = findVariant(ro, "Err");
        if (!okV || !errV) {
            error("`?` operand enum '" + ro->enumName +
                      "' must have `Ok(T)` and `Err(E)` variants",
                  te.line, te.column);
            return makeInt();
        }
        if (okV->payloadTypes.size() != 1 || errV->payloadTypes.size() != 1) {
            error("`?` operand variants `Ok` / `Err` must each carry exactly "
                  "one payload",
                  te.line, te.column);
            return makeInt();
        }
        if (!currentReturnType_) {
            error("`?` used outside any function body", te.line, te.column);
            return okV->payloadTypes[0];
        }
        TypePtr rRet = resolve(currentReturnType_);
        if (rRet->kind != TypeKind::Enum) {
            error("`?` in fn whose return type is " +
                      typeToString(currentReturnType_) +
                      "; expected a Result-shaped enum",
                  te.line, te.column);
            return okV->payloadTypes[0];
        }
        const EnumVariantType* retErr = findVariant(rRet, "Err");
        if (!retErr || retErr->payloadTypes.size() != 1) {
            error("`?` requires the enclosing fn's return type '" +
                      rRet->enumName +
                      "' to have an `Err(E)` variant",
                  te.line, te.column);
            return okV->payloadTypes[0];
        }
        if (!unify(errV->payloadTypes[0], retErr->payloadTypes[0])) {
            error("`?` Err payload type " +
                      typeToString(errV->payloadTypes[0]) +
                      " does not match enclosing fn's Err payload type " +
                      typeToString(retErr->payloadTypes[0]),
                  te.line, te.column);
        }
        return okV->payloadTypes[0];
    }

    TypePtr checkStructLit(const ast::StructLitExpr& sl) {
        auto it = structSchemas_.find(sl.structName);
        if (it == structSchemas_.end()) {
            error("unknown struct: " + sl.structName, sl.line, sl.column);
            for (const auto& f : sl.fields) checkExpr(*f.second);
            return makeInt();
        }
        // Built-in runtime Future values are compiler-constructed and carry
        // executable state ({poll fn ptr, frame ptr}). Reject source-level
        // literals so users cannot forge invalid futures like `Future {}`.
        if (sl.structName == "Future") {
            error("`Future` values are runtime-only and cannot be constructed "
                  "with a struct literal",
                  sl.line, sl.column);
            for (const auto& f : sl.fields) checkExpr(*f.second);
            return freshInstantiateStruct(it->second);
        }
        // PR#21: `String` is an opaque built-in (heap {ptr,len,cap}); a
        // source-level `String { ... }` literal would let a user forge an
        // invalid buffer/length pair and trigger out-of-bounds reads in
        // print_str / str_len. Reject it like Future.
        if (sl.structName == "String") {
            error("cannot construct built-in opaque struct 'String' with a "
                  "struct literal; use string_new()",
                  sl.line, sl.column);
            for (const auto& f : sl.fields) checkExpr(*f.second);
            return freshInstantiateStruct(it->second);
        }
        // PR#24: `Vec` is likewise an opaque built-in (heap {data,len,cap});
        // a forged `Vec { ... }` literal could desync the buffer/length and
        // drive out-of-bounds access. Use vec_new().
        if (sl.structName == "Vec") {
            error("cannot construct built-in opaque struct 'Vec' with a "
                  "struct literal; use vec_new()",
                  sl.line, sl.column);
            for (const auto& f : sl.fields) checkExpr(*f.second);
            return freshInstantiateStruct(it->second);
        }
        // For generic structs, build a fresh instantiation so field-type
        // unification with each literal expr leaves the instance's
        // typeArgs in a fully-solved state.
        TypePtr instType = freshInstantiateStruct(it->second);
        std::unordered_map<std::string, TypePtr> declared;
        declared.reserve(instType->structFields.size());
        for (const auto& df : instType->structFields) {
            declared.emplace(df.first, df.second);
        }
        std::unordered_set<std::string> initialised;
        for (const auto& f : sl.fields) {
            TypePtr valT = checkExpr(*f.second);
            auto declIt = declared.find(f.first);
            if (declIt == declared.end()) {
                error("unknown field '" + f.first + "' for struct '" +
                          sl.structName + "'",
                      f.second->line, f.second->column);
                continue;
            }
            if (!initialised.insert(f.first).second) {
                error("duplicate field '" + f.first + "' in struct literal",
                      f.second->line, f.second->column);
                continue;
            }
            if (!unify(valT, declIt->second)) {
                error("field '" + f.first + "' of struct '" + sl.structName +
                          "' has type " + typeToString(declIt->second) +
                          ", got " + typeToString(valT),
                      f.second->line, f.second->column);
            }
        }
        for (const auto& df : instType->structFields) {
            if (!initialised.count(df.first)) {
                error("missing field '" + df.first + "' in struct '" +
                          sl.structName + "' literal",
                      sl.line, sl.column);
            }
        }
        return instType;
    }

    TypePtr checkField(const ast::FieldExpr& fe) {
        TypePtr objT = checkExpr(*fe.object);
        TypePtr r = resolve(objT);
        // Phase 2.4b auto-deref: `(&p).x` works the same as `p.x`. Peel
        // off any reference layer before the struct lookup.
        while (r->kind == TypeKind::Ref) r = resolve(r->refInner);
        if (r->kind != TypeKind::Struct) {
            error("field access on non-struct type " + typeToString(objT),
                  fe.line, fe.column);
            return makeInt();
        }
        for (const auto& f : r->structFields) {
            if (f.first == fe.fieldName) return f.second;
        }
        error("no field '" + fe.fieldName + "' on struct '" + r->structName +
                  "'",
              fe.line, fe.column);
        return makeInt();
    }

    TypePtr checkBinary(const ast::BinaryExpr& bin) {
        TypePtr lhs = checkExpr(*bin.lhs);
        TypePtr rhs = checkExpr(*bin.rhs);
        // Phase 33: `&&` is the only boolean binary op — both operands and the
        // result are bool (short-circuit; codegen only evaluates rhs if lhs).
        if (bin.op == ast::BinOp::And) {
            if (!unify(lhs, makeBool())) {
                error("logical `&&` expects bool on lhs, got " +
                          typeToString(lhs),
                      bin.lhs->line, bin.lhs->column);
            }
            if (!unify(rhs, makeBool())) {
                error("logical `&&` expects bool on rhs, got " +
                          typeToString(rhs),
                      bin.rhs->line, bin.rhs->column);
            }
            return makeBool();
        }
        const bool isComparison = (bin.op == ast::BinOp::Lt) ||
                                  (bin.op == ast::BinOp::Le) ||
                                  (bin.op == ast::BinOp::Gt) ||
                                  (bin.op == ast::BinOp::Ge) ||
                                  (bin.op == ast::BinOp::Eq) ||
                                  (bin.op == ast::BinOp::NotEq);
        const char* what = isComparison ? "comparison" : "arithmetic";
        // Phase 39: f64 arithmetic / comparison. If a side is already f64, both
        // sides must be f64 — there is NO implicit i64<->f64 coercion (use
        // to_f64 / float_to_int). `%` is integer-only.
        if (resolve(lhs)->kind == TypeKind::Float ||
            resolve(rhs)->kind == TypeKind::Float) {
            if (bin.op == ast::BinOp::Mod) {
                error("`%` (modulo) is not defined for f64", bin.line,
                      bin.column);
            }
            if (!unify(lhs, makeFloat())) {
                error(std::string(what) + " op expects f64 on lhs, got " +
                          typeToString(lhs),
                      bin.lhs->line, bin.lhs->column);
            }
            if (!unify(rhs, makeFloat())) {
                error(std::string(what) + " op expects f64 on rhs, got " +
                          typeToString(rhs),
                      bin.rhs->line, bin.rhs->column);
            }
            return isComparison ? makeBool() : makeFloat();
        }
        if (!unify(lhs, makeInt())) {
            error(std::string(what) + " op expects i64 on lhs, got " +
                      typeToString(lhs),
                  bin.lhs->line, bin.lhs->column);
        }
        if (!unify(rhs, makeInt())) {
            error(std::string(what) + " op expects i64 on rhs, got " +
                      typeToString(rhs),
                  bin.rhs->line, bin.rhs->column);
        }
        return isComparison ? makeBool() : makeInt();
    }

    // Phase 10a: build the first-class-value type of a declared fn — its
    // (already-instantiated) signature plus an effect row reflecting the
    // schema's declared effects. Concrete built-in labels become closed
    // row labels; a declared effect-row-var name becomes the row var tail
    // (mapped to the instantiated Var via `subst` when available, else a
    // fresh var). This is what makes `let f = ioInc; f(x)` still account
    // for `io`, and what lets a higher-order fn value stay polymorphic.
    TypePtr attachDeclaredEffects(const TypePtr& instSig,
                                  const FnSchema& schema,
                                  const std::unordered_map<int, TypePtr>*
                                      subst = nullptr) {
        TypePtr r = resolve(instSig);
        if (r->kind != TypeKind::Function) return instSig;
        std::vector<std::string> labels;
        TypePtr rowVar;
        for (const auto& l : schema.declaredEffects.labels) {
            if (isBuiltinEffect(l)) {
                if (std::find(labels.begin(), labels.end(), l) == labels.end())
                    labels.push_back(l);
                continue;
            }
            // An effect-row-var name: find its schema Var, then its
            // instantiated counterpart (if a subst was supplied).
            TypePtr schemaVar;
            for (const auto& [n, v] : schema.effectRowVars) {
                if (n == l) { schemaVar = v; break; }
            }
            if (schemaVar) {
                if (subst) {
                    auto it = subst->find(resolve(schemaVar)->varId);
                    rowVar = it != subst->end() ? it->second : makeFreshVar();
                } else {
                    rowVar = makeFreshVar();
                }
            }
        }
        return makeFunction(r->args, r->ret, std::move(labels), rowVar);
    }

    // Phase 10a: add a row var's effect contribution (in source-level
    // names) to `out`. If the var was solved during unification, its
    // concrete labels are added; if it is still polymorphic but is one of
    // the enclosing fn's declared effect-row vars, its name is added (so
    // the enclosing fn must itself declare that row var). A fully-free row
    // var (neither solved nor a known name) contributes nothing — it is
    // pure at this site.
    void addRowVarContribution(const TypePtr& rowVar, EffectSet& out) {
        TypePtr rv = resolve(rowVar);
        if (rv->kind != TypeKind::Var) return;
        if (rv->effectRowSolved) {
            for (const auto& l : rv->effectLabels) out.add(l);
            return;
        }
        auto it = currentEffectRowVarById_.find(rv->varId);
        if (it != currentEffectRowVarById_.end()) out.add(it->second);
    }

    // Phase 10a: record the effect contribution of an indirect call given
    // the resolved Function type of the fn value. Concrete row labels are
    // added directly; the row-var tail (if any) goes through
    // addRowVarContribution.
    void recordCallEffectsFromFnType(const ast::CallExpr& call,
                                     const TypePtr& fnTy) {
        EffectSet contrib;
        for (const auto& l : fnTy->effectLabels) contrib.add(l);
        if (fnTy->effectRowVar) addRowVarContribution(fnTy->effectRowVar, contrib);
        if (!contrib.labels.empty()) exprEffects_[&call] = contrib;
    }

    // Phase 10b: walk a closure body and collect the names that refer to
    // FREE variables — identifiers (or call callees) that are bound in an
    // enclosing local scope and are neither the closure's own params nor a
    // name bound within the body before use. `bound` tracks names that are
    // locally introduced as we descend (closure params, nested `let`s,
    // match/for pattern bindings, nested-closure params); a use of a name
    // not in `bound` is a capture iff `lookupLocal` finds it (i.e. it is an
    // enclosing local, not a global fn / constructor). Order of first
    // appearance is preserved via `order` so codegen's env layout is stable.
    void collectFreeVars(const ast::Expr& e,
                         std::unordered_set<std::string>& bound,
                         std::vector<std::string>& order,
                         std::unordered_set<std::string>& seen) {
        auto noteUse = [&](const std::string& name) {
            if (bound.count(name)) return;
            if (seen.count(name)) return;
            if (lookupLocal(name)) { // an enclosing local => capture
                seen.insert(name);
                order.push_back(name);
            }
        };
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            noteUse(id->name);
            return;
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            collectFreeVars(*bin->lhs, bound, order, seen);
            collectFreeVars(*bin->rhs, bound, order, seen);
            return;
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            collectFreeVars(*un->operand, bound, order, seen);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // The callee name can itself be a captured fn value.
            noteUse(call->callee);
            for (const auto& a : call->args)
                collectFreeVars(*a, bound, order, seen);
            return;
        }
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            // Phase 17a: the callee is an expression — it (and the args) may
            // reference free variables of the enclosing scope.
            collectFreeVars(*cv->callee, bound, order, seen);
            for (const auto& a : cv->args)
                collectFreeVars(*a, bound, order, seen);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            collectFreeVars(*mc->receiver, bound, order, seen);
            for (const auto& a : mc->args)
                collectFreeVars(*a, bound, order, seen);
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            collectFreeVars(*ie->cond, bound, order, seen);
            collectFreeVars(*ie->thenBranch, bound, order, seen);
            collectFreeVars(*ie->elseBranch, bound, order, seen);
            return;
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            // A block opens a fresh binding scope; names `let`-bound inside
            // it shadow captures for the remainder of the block. We add to
            // `bound` as we go and restore afterwards.
            std::vector<std::string> added;
            for (const auto& stmt : block->stmts) {
                if (auto* let = dynamic_cast<const ast::LetStmt*>(stmt.get())) {
                    collectFreeVars(*let->value, bound, order, seen);
                    // Phase 22: a tuple-destructuring let binds each element
                    // name (shadowing captures for the rest of the block).
                    if (!let->tupleNames.empty()) {
                        for (const auto& nm : let->tupleNames) {
                            if (nm == "_") continue;
                            if (bound.insert(nm).second) added.push_back(nm);
                        }
                    } else if (bound.insert(let->name).second) {
                        added.push_back(let->name);
                    }
                } else if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(
                               stmt.get())) {
                    if (ret->value)
                        collectFreeVars(*ret->value, bound, order, seen);
                } else if (auto* as = dynamic_cast<const ast::AssignStmt*>(
                               stmt.get())) {
                    collectFreeVars(*as->target, bound, order, seen);
                    collectFreeVars(*as->value, bound, order, seen);
                } else if (auto* es = dynamic_cast<const ast::ExprStmt*>(
                               stmt.get())) {
                    collectFreeVars(*es->expr, bound, order, seen);
                }
            }
            if (block->tail) collectFreeVars(*block->tail, bound, order, seen);
            for (const auto& n : added) bound.erase(n);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            collectFreeVars(*me->scrutinee, bound, order, seen);
            for (const auto& arm : me->arms) {
                std::vector<std::string> added;
                collectPatternBindings(*arm.pattern, bound, added);
                collectFreeVars(*arm.body, bound, order, seen);
                for (const auto& n : added) bound.erase(n);
            }
            return;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            collectFreeVars(*we->cond, bound, order, seen);
            collectFreeVars(*we->body, bound, order, seen);
            return;
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            collectFreeVars(*le->body, bound, order, seen);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::ForExpr*>(&e)) {
            collectFreeVars(*fe->iter, bound, order, seen);
            std::vector<std::string> added;
            collectPatternBindings(*fe->pattern, bound, added);
            collectFreeVars(*fe->body, bound, order, seen);
            for (const auto& n : added) bound.erase(n);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            collectFreeVars(*re->start, bound, order, seen);
            collectFreeVars(*re->end, bound, order, seen);
            return;
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e)) {
            if (be->value) collectFreeVars(*be->value, bound, order, seen);
            return;
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields)
                collectFreeVars(*v, bound, order, seen);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            collectFreeVars(*fe->object, bound, order, seen);
            return;
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            collectFreeVars(*te->operand, bound, order, seen);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            collectFreeVars(*re->operand, bound, order, seen);
            return;
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            collectFreeVars(*se->operand, bound, order, seen);
            collectFreeVars(*se->start, bound, order, seen);
            collectFreeVars(*se->end, bound, order, seen);
            return;
        }
        // Phase 22: array / tuple literals + index / tuple-field access.
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            for (const auto& el : al->elements)
                collectFreeVars(*el, bound, order, seen);
            return;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            for (const auto& el : tl->elements)
                collectFreeVars(*el, bound, order, seen);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            collectFreeVars(*ix->object, bound, order, seen);
            collectFreeVars(*ix->index, bound, order, seen);
            return;
        }
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e)) {
            collectFreeVars(*tf->object, bound, order, seen);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            collectFreeVars(*ae->operand, bound, order, seen);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            // Nested closure: its params shadow within its body. A name it
            // captures from OUR enclosing scope is also a capture of ours
            // (transitively). We add the nested params to `bound` so they
            // don't leak out as captures of the outer closure.
            std::vector<std::string> added;
            for (const auto& p : cl->params)
                if (bound.insert(p.name).second) added.push_back(p.name);
            collectFreeVars(*cl->body, bound, order, seen);
            for (const auto& n : added) bound.erase(n);
            return;
        }
        // IntLit / StringLit / Continue: no identifiers.
    }

    // Add the variable bindings a pattern introduces to `bound` (recording
    // newly-added names in `added` for later removal). Constructor names are
    // not bindings; only VarPats that aren't unit-variant constructors bind.
    void collectPatternBindings(const ast::Pattern& pat,
                                std::unordered_set<std::string>& bound,
                                std::vector<std::string>& added) {
        if (auto* vp = dynamic_cast<const ast::VarPat*>(&pat)) {
            if (variantIndex_.count(vp->name)) return; // unit ctor, not a bind
            if (bound.insert(vp->name).second) added.push_back(vp->name);
            return;
        }
        if (auto* cp = dynamic_cast<const ast::CtorPat*>(&pat)) {
            for (const auto& sp : cp->subpatterns)
                collectPatternBindings(*sp, bound, added);
            return;
        }
        if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
            for (const auto& sp : tp->elements)
                collectPatternBindings(*sp, bound, added);
            return;
        }
        // LitIntPat / WildPat: no bindings.
    }

    // Phase 17a: does the closure body mutate the free variable `name` — i.e.
    // assign to it (`name = ...`) or take a `&mut` of it (`&mut name`)? Such a
    // variable must be captured BY REFERENCE so the mutation is visible after
    // the call (FnMut). `bound` tracks names that shadow `name` as we descend
    // (closure params, nested `let`s, match/for pattern bindings, nested
    // closures' params) so a write to a *shadowing* `name` does not count.
    // Only the ROOT place of an assignment counts (`name = ...` and
    // `name.field = ...` both mutate the binding `name`; `other.f = name`
    // does not). Reads never count — they may stay by value.
    bool bodyMutatesCapture(const ast::Expr& e, const std::string& name,
                            std::unordered_set<std::string>& bound) {
        // The root binding of an assignable place (Ident or field chain).
        auto rootName = [](const ast::Expr& place) -> const std::string* {
            const ast::Expr* root = &place;
            // Phase 22: descend field / index / tuple-field projections — a
            // write to `arr[i]` / `t.0` of a captured value still mutates the
            // root binding.
            while (true) {
                if (auto* fe = dynamic_cast<const ast::FieldExpr*>(root))
                    root = fe->object.get();
                else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(root))
                    root = ix->object.get();
                else if (auto* tf =
                             dynamic_cast<const ast::TupleFieldExpr*>(root))
                    root = tf->object.get();
                else
                    break;
            }
            if (auto* id = dynamic_cast<const ast::IdentExpr*>(root))
                return &id->name;
            return nullptr;
        };
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            if (re->isMut) {
                if (auto* id =
                        dynamic_cast<const ast::IdentExpr*>(re->operand.get())) {
                    if (id->name == name && !bound.count(name)) return true;
                }
            }
            return bodyMutatesCapture(*re->operand, name, bound);
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return bodyMutatesCapture(*bin->lhs, name, bound) ||
                   bodyMutatesCapture(*bin->rhs, name, bound);
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e))
            return bodyMutatesCapture(*un->operand, name, bound);
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args)
                if (bodyMutatesCapture(*a, name, bound)) return true;
            return false;
        }
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            if (bodyMutatesCapture(*cv->callee, name, bound)) return true;
            for (const auto& a : cv->args)
                if (bodyMutatesCapture(*a, name, bound)) return true;
            return false;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            if (bodyMutatesCapture(*mc->receiver, name, bound)) return true;
            for (const auto& a : mc->args)
                if (bodyMutatesCapture(*a, name, bound)) return true;
            return false;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            return bodyMutatesCapture(*ie->cond, name, bound) ||
                   bodyMutatesCapture(*ie->thenBranch, name, bound) ||
                   bodyMutatesCapture(*ie->elseBranch, name, bound);
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            std::vector<std::string> added;
            bool hit = false;
            for (const auto& stmt : block->stmts) {
                if (auto* let = dynamic_cast<const ast::LetStmt*>(stmt.get())) {
                    if (bodyMutatesCapture(*let->value, name, bound)) hit = true;
                    // Phase 22: bind tuple-destructure names too.
                    if (!let->tupleNames.empty()) {
                        for (const auto& nm : let->tupleNames) {
                            if (nm == "_") continue;
                            if (bound.insert(nm).second) added.push_back(nm);
                        }
                    } else if (bound.insert(let->name).second) {
                        added.push_back(let->name);
                    }
                } else if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(
                               stmt.get())) {
                    if (ret->value &&
                        bodyMutatesCapture(*ret->value, name, bound))
                        hit = true;
                } else if (auto* as = dynamic_cast<const ast::AssignStmt*>(
                               stmt.get())) {
                    const std::string* rn = rootName(*as->target);
                    if (rn && *rn == name && !bound.count(name)) hit = true;
                    if (bodyMutatesCapture(*as->value, name, bound)) hit = true;
                    // The target may itself read sub-expressions (field
                    // indices etc.) but our places have no such reads in V1.
                } else if (auto* es = dynamic_cast<const ast::ExprStmt*>(
                               stmt.get())) {
                    if (bodyMutatesCapture(*es->expr, name, bound)) hit = true;
                }
            }
            if (block->tail && bodyMutatesCapture(*block->tail, name, bound))
                hit = true;
            for (const auto& n : added) bound.erase(n);
            return hit;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            if (bodyMutatesCapture(*me->scrutinee, name, bound)) return true;
            for (const auto& arm : me->arms) {
                std::vector<std::string> added;
                collectPatternBindings(*arm.pattern, bound, added);
                bool hit = bodyMutatesCapture(*arm.body, name, bound);
                for (const auto& n : added) bound.erase(n);
                if (hit) return true;
            }
            return false;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            return bodyMutatesCapture(*we->cond, name, bound) ||
                   bodyMutatesCapture(*we->body, name, bound);
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e))
            return bodyMutatesCapture(*le->body, name, bound);
        if (auto* fe = dynamic_cast<const ast::ForExpr*>(&e)) {
            if (bodyMutatesCapture(*fe->iter, name, bound)) return true;
            std::vector<std::string> added;
            collectPatternBindings(*fe->pattern, bound, added);
            bool hit = bodyMutatesCapture(*fe->body, name, bound);
            for (const auto& n : added) bound.erase(n);
            return hit;
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            return bodyMutatesCapture(*re->start, name, bound) ||
                   bodyMutatesCapture(*re->end, name, bound);
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e))
            return be->value ? bodyMutatesCapture(*be->value, name, bound)
                             : false;
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields)
                if (bodyMutatesCapture(*v, name, bound)) return true;
            return false;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e))
            return bodyMutatesCapture(*fe->object, name, bound);
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e))
            return bodyMutatesCapture(*se->operand, name, bound) ||
                   bodyMutatesCapture(*se->start, name, bound) ||
                   bodyMutatesCapture(*se->end, name, bound);
        // Phase 22: array / tuple literals + index / tuple-field access.
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            for (const auto& el : al->elements)
                if (bodyMutatesCapture(*el, name, bound)) return true;
            return false;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            for (const auto& el : tl->elements)
                if (bodyMutatesCapture(*el, name, bound)) return true;
            return false;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e))
            return bodyMutatesCapture(*ix->object, name, bound) ||
                   bodyMutatesCapture(*ix->index, name, bound);
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e))
            return bodyMutatesCapture(*tf->object, name, bound);
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e))
            return bodyMutatesCapture(*te->operand, name, bound);
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e))
            return bodyMutatesCapture(*ae->operand, name, bound);
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            // A nested closure shadows `name` with its own params; a write to
            // OUR `name` from inside it would itself be a by-ref capture of
            // the nested closure (transitively reaching our binding), so it
            // still counts as a mutation of `name` from our perspective.
            std::vector<std::string> added;
            for (const auto& p : cl->params)
                if (bound.insert(p.name).second) added.push_back(p.name);
            bool hit = bodyMutatesCapture(*cl->body, name, bound);
            for (const auto& n : added) bound.erase(n);
            return hit;
        }
        // IntLit / BoolLit / StringLit / Ident / Continue: never a mutation.
        return false;
    }

    // Phase 17a: true if the expression IS a closure (possibly parenthesized
    // away by the parser — parens are transparent in the AST) whose capture
    // list includes a by-reference capture. Used to reject returning such a
    // closure (its env would hold a dangling pointer into the dead frame).
    bool closureEscapesByRef(const ast::Expr& e) {
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            for (const auto& cap : cl->captures)
                if (cap.byRef) return true;
            return false;
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e))
            return isByRefClosureLocal(id->name);
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            if (block->tail) return closureEscapesByRef(*block->tail);
            return false;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            return closureEscapesByRef(*ie->thenBranch) ||
                   closureEscapesByRef(*ie->elseBranch);
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields)
                if (closureEscapesByRef(*v)) return true;
            return false;
        }
        return false;
    }

    // Phase 19 (Send): if `e` is a closure that captures something BY
    // REFERENCE (FnMut), return that capture's name; else "". Used to reject
    // sending such a closure across a thread boundary — a by-ref capture
    // aliases a stack slot of the spawning frame, which is both a data race
    // (two threads touching the same unsynchronized slot) and a
    // use-after-free once the spawning frame exits. (`checkClosure` has
    // already populated `captures` by the time this runs.)
    std::string closureByRefCaptureName(const ast::Expr& e) {
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            for (const auto& cap : cl->captures)
                if (cap.byRef) return cap.name;
        }
        return "";
    }

    // Phase 10b: type-check a capturing closure `|params| body`. Determines
    // the captured free variables (recorded on the AST node for codegen),
    // checks the body in a scope holding ONLY the params + captures (so an
    // un-captured enclosing local can't accidentally be referenced — it
    // would be an unknown identifier), infers param/return types and the
    // body's effect row, and returns a Function type carrying that row. The
    // closure value is first-class and interoperates with the Phase 4.3
    // fn-value machinery (indirect calls, if-selection, higher-order args).
    TypePtr checkClosure(const ast::ClosureExpr& cl) {
        // 1) Determine captures against the CURRENT (enclosing) scope.
        std::unordered_set<std::string> bound;
        for (const auto& p : cl.params) bound.insert(p.name);
        std::vector<std::string> order;
        std::unordered_set<std::string> seen;
        collectFreeVars(*cl.body, bound, order, seen);

        // Snapshot each capture's (resolved-enough) type from the enclosing
        // scope. We record the capture list on the node for codegen.
        //
        // Phase 17a: classify each capture as by-value (Phase 10b) or by-ref
        // (FnMut). A capture is by-ref iff the body mutates it (assigns to it
        // or takes `&mut` of it). By-ref captures store a pointer to the
        // enclosing alloca, so writes persist after the call. A by-ref
        // capture requires the enclosing binding be `let mut` (otherwise the
        // mutation is illegal). `captureMut[i]` records whether the capture's
        // enclosing binding was `mut`, so the body's scope marks it mutable
        // (so `name = ...` inside the body type-checks).
        cl.captures.clear();
        cl.captures.reserve(order.size());
        std::vector<std::pair<std::string, TypePtr>> captureTypes;
        std::vector<bool> captureMut; // parallel to captureTypes
        for (const auto& name : order) {
            TypePtr t = lookupLocal(name);
            if (!t) continue; // defensive — collectFreeVars only adds locals
            std::unordered_set<std::string> shadow;
            bool mutated = bodyMutatesCapture(*cl.body, name, shadow);
            bool enclosingMut = isMutLocal(name);
            TypePtr rt = resolve(t);
            // PR#18: only immediate scalar Copy types (i64, bool, unit) may
            // be captured by value. A `&T` captured by value is unsound for an
            // *escaping* closure (the env would outlive the borrowed stack
            // slot — Phase 17a's escape check only catches by-ref/FnMut
            // captures, not a by-value reference copy), so reject `&T`
            // by-value captures outright in this MVP.
            bool copyable = rt->kind == TypeKind::Int ||
                            rt->kind == TypeKind::Bool ||
                            rt->kind == TypeKind::Unit;
            if (mutated) {
                // FnMut: capture by reference. The mutation requires the
                // enclosing binding be `let mut`.
                if (!enclosingMut) {
                    error("closure mutates captured `" + name +
                              "`, but it is not declared `let mut` in the "
                              "enclosing scope; a by-reference (FnMut) capture "
                              "needs a mutable binding",
                          cl.line, cl.column);
                }
            } else if (!copyable) {
                // By-value capture: keep the Phase 10b Copy-only rule.
                // Capturing a non-Copy aggregate (struct / enum / &mut /
                // fn-value) by value would be a move the borrow checker cannot
                // currently see (it does not descend into closure bodies), so
                // we reject it rather than risk an untracked use-after-move.
                // (FnMut covers the mutate-in-place case; this branch is the
                // read-only non-Copy case, still deferred.)
                error("closure captures `" + name + "` of type " +
                          typeToString(t) +
                          ", but only Copy scalar types (i64, bool, unit) may "
                          "be captured by value in this MVP; reference/aggregate "
                          "captures are not yet supported",
                      cl.line, cl.column);
            }
            ast::ClosureCapture cap;
            cap.name = name;
            cap.type = t;
            cap.byRef = mutated;
            cl.captures.push_back(std::move(cap));
            captureTypes.emplace_back(name, t);
            captureMut.push_back(enclosingMut);
        }

        // 2) Resolve param types: annotated -> that type; else a fresh Var
        // that unification with the use-context (e.g. a higher-order fn's
        // param signature) will pin down.
        std::vector<TypePtr> paramTypes;
        paramTypes.reserve(cl.params.size());
        for (const auto& p : cl.params) {
            if (p.hasAnnotation) {
                paramTypes.push_back(resolveTypeRef(p.type));
            } else {
                paramTypes.push_back(makeFreshVar());
            }
        }

        // 3) Check the body in a fresh scope containing only params +
        // captures. A new scope frame on top of the existing stack would
        // also expose un-captured outer locals to the body; to enforce
        // capture-by-value semantics (and avoid silently reading a local
        // we didn't capture), we temporarily swap in a scope stack that
        // holds ONLY the params + captures.
        std::vector<Scope> savedScopes = std::move(scopes_);
        std::vector<std::unordered_set<std::string>> savedMut =
            std::move(mutScopes_);
        std::vector<LoopCtx> savedLoops = std::move(loopStack_);
        TypePtr savedRet = currentReturnType_;
        scopes_.clear();
        mutScopes_.clear();
        loopStack_.clear();
        pushScope();
        for (std::size_t i = 0; i < captureTypes.size(); ++i) {
            scopes_.back()[captureTypes[i].first] = captureTypes[i].second;
            // Phase 17a: a `let mut` capture stays mutable inside the body so
            // an assignment to it type-checks (and is lowered as a by-ref
            // capture writing through the pointer to the enclosing alloca).
            if (captureMut[i]) markMut(captureTypes[i].first);
        }
        for (std::size_t i = 0; i < cl.params.size(); ++i) {
            scopes_.back()[cl.params[i].name] = paramTypes[i];
        }
        // A closure body's `return` would conceptually return from the
        // closure; the MVP has no such tests, but binding currentReturnType_
        // to the body's inferred type would create a chicken-and-egg. Leave
        // it null so a stray `return` inside a closure body is reported as
        // "outside any function" only if it also uses `?`; a plain
        // `return e;` is checked loosely (its value type is free).
        currentReturnType_ = nullptr;
        TypePtr bodyType = checkExpr(*cl.body);

        // 4) Infer the body's effect row.
        EffectSet bodyEffects;
        collectEffects(*cl.body, bodyEffects);

        // Restore the enclosing context.
        scopes_ = std::move(savedScopes);
        mutScopes_ = std::move(savedMut);
        loopStack_ = std::move(savedLoops);
        currentReturnType_ = savedRet;

        // 5) Build the closure's Function type. Concrete built-in effect
        // labels become the closed row; effect-row-var names (e.g. a still-
        // polymorphic `e` captured from the enclosing fn) attach as the row
        // var tail so row polymorphism composes. The closure expression
        // itself contributes no effects to the enclosing fn (handled in
        // collectEffects); only CALLING it does.
        std::vector<std::string> labels;
        TypePtr rowVar;
        for (const auto& l : bodyEffects.labels) {
            if (isBuiltinEffect(l)) {
                if (std::find(labels.begin(), labels.end(), l) == labels.end())
                    labels.push_back(l);
            } else if (currentGenericEnv_) {
                // A row-var name flowing out of the body (e.g. via an
                // indirect call to a captured effect-polymorphic fn).
                auto git = currentGenericEnv_->find(l);
                if (git != currentGenericEnv_->end()) rowVar = git->second;
            }
        }
        return makeFunction(std::move(paramTypes), bodyType, std::move(labels),
                            rowVar);
    }

    TypePtr checkCall(const ast::CallExpr& call) {
        // Phase 30: note any use of a file-I/O / CLI-args builtin so codegen
        // emits that runtime (which references libc free/fopen) ONLY for
        // programs that actually touch it — keeping I/O-free programs (and the
        // codegen unit tests) free of that machinery.
        if (call.callee == "fs_read_into" || call.callee == "fs_write_raw" ||
            call.callee == "fs_exists" || call.callee == "arg_count" ||
            call.callee == "arg_get") {
            usesFileIo_ = true;
        }
        // Phase 4.3: first-class fn values. If the call's callee name
        // resolves to a local binding with a Function type, this is an
        // indirect call through a fn pointer. We type-check the arg
        // list against the binding's signature and return its ret type.
        // Bare names still resolve through fnSchemas_ first (so a let-
        // shadowing a top-level fn doesn't accidentally indirect-call
        // when the user expected the direct one — symmetry with the
        // expression-eval path).
        if (auto local = lookupLocal(call.callee)) {
            TypePtr r = resolve(local);
            if (r->kind == TypeKind::Function) {
                if (r->args.size() != call.args.size()) {
                    error("indirect call to '" + call.callee +
                              "' expects " +
                              std::to_string(r->args.size()) +
                              " arg(s), got " +
                              std::to_string(call.args.size()),
                          call.line, call.column);
                }
                const std::size_t n =
                    std::min(r->args.size(), call.args.size());
                for (std::size_t i = 0; i < n; ++i) {
                    TypePtr argType = checkExpr(*call.args[i]);
                    if (!unify(argType, r->args[i])) {
                        error("argument " + std::to_string(i + 1) +
                                  " of indirect call to '" + call.callee +
                                  "' has type " + typeToString(argType) +
                                  ", expected " + typeToString(r->args[i]),
                              call.args[i]->line, call.args[i]->column);
                    }
                }
                for (std::size_t i = n; i < call.args.size(); ++i) {
                    checkExpr(*call.args[i]);
                }
                // Phase 10a: an indirect call performs the effects carried
                // by the fn value's type. Record them (as source-level
                // names) for the effect pass: solved/concrete labels pass
                // through; an unsolved row var that is one of the enclosing
                // fn's effect-row vars contributes its name.
                recordCallEffectsFromFnType(call, r);
                return r->ret;
            }
        }
        auto fnIt = fnSchemas_.find(call.callee);
        if (fnIt != fnSchemas_.end()) {
            const FnSchema& schema = fnIt->second;
            // Phase 7.3b: path-qualified call sites (`foo::bar()`) must
            // resolve to a `pub` fn. Bare-name calls still work via
            // Phase 7.1's flat-merge so existing intra-module code is
            // unaffected. Built-in stdlib fns (`print`, `vec_*`) are
            // declared via the typechecker's own registration and not
            // marked pub, but they're addressed without paths so this
            // check never fires on them.
            if (call.wasPath && !schema.isPub) {
                error("function '" + call.callee +
                          "' is not declared `pub`; cannot reach it via "
                          "path-qualified syntax",
                      call.line, call.column);
            }
            // Instantiate the schema with a fresh Var per generic param.
            // Substitution is empty for monomorphic fns (instantiate is a
            // no-op in that case), keeping the hot path zero-cost.
            std::unordered_map<int, TypePtr> subst;
            std::vector<TypePtr> typeArgs;
            typeArgs.reserve(schema.genericVars.size());
            for (const auto& gv : schema.genericVars) {
                TypePtr fresh = makeFreshVar();
                subst[gv->varId] = fresh;
                typeArgs.push_back(fresh);
            }
            // Phase 10a: also freshen effect-row vars so each call site gets
            // its own row var to bind (they live outside genericVars to stay
            // out of monomorphization, but still need per-call instantiation
            // so effect inference doesn't leak across call sites).
            for (const auto& [name, rv] : schema.effectRowVars) {
                TypePtr resolvedRv = resolve(rv);
                if (resolvedRv->kind == TypeKind::Var &&
                    !subst.count(resolvedRv->varId)) {
                    subst[resolvedRv->varId] = makeFreshVar();
                }
            }
            // Phase 21b: a `C::Item` projection in the callee's signature is a
            // placeholder Var keyed on `C`'s schema Var. Give it a fresh Var
            // per call so the call's result type isn't shared across sites;
            // after arg unification (below) we'll resolve `C` to a concrete
            // type at this site and pin the fresh Var to the impl's chosen
            // associated type. Only projections over THIS callee's generic
            // params apply.
            std::vector<std::pair<TypePtr, AssocProjection>> callProjections;
            {
                std::unordered_set<int> calleeVarIds;
                for (const auto& gv : schema.genericVars)
                    calleeVarIds.insert(gv->varId);
                for (const auto& [phId, proj] : assocProjections_) {
                    if (!calleeVarIds.count(proj.baseVarId)) continue;
                    TypePtr fresh = makeFreshVar();
                    subst[phId] = fresh;
                    callProjections.emplace_back(fresh, proj);
                }
            }
            TypePtr instSig = instantiate(schema.signature, subst);
            if (instSig->args.size() != call.args.size()) {
                error("function '" + call.callee + "' expects " +
                          std::to_string(instSig->args.size()) +
                          " arg(s), got " + std::to_string(call.args.size()),
                      call.line, call.column);
            }
            const std::size_t n =
                std::min(instSig->args.size(), call.args.size());
            for (std::size_t i = 0; i < n; ++i) {
                TypePtr argType = checkExpr(*call.args[i]);
                // Phase 11: a `&Concrete`/`Box<Concrete>` arg coerces into a
                // `&dyn Trait`/`Box<dyn Trait>` parameter.
                if (!coerceOrUnify(*call.args[i], argType, instSig->args[i])) {
                    error("argument " + std::to_string(i + 1) + " to '" +
                              call.callee + "' has type " +
                              typeToString(argType) + ", expected " +
                              typeToString(instSig->args[i]),
                          call.args[i]->line, call.args[i]->column);
                }
            }
            for (std::size_t i = n; i < call.args.size(); ++i) {
                checkExpr(*call.args[i]);
            }
            // Phase 21b: now that args are unified, the callee's generic params
            // are pinned. Resolve each `C::Item` projection at this call site:
            // C's call-fresh Var (subst[baseVar]) resolves to a concrete type;
            // look up that type's impl's chosen associated type and unify the
            // projection's fresh result Var with it. Leaves it unbound (an
            // error already reported elsewhere) if C isn't concrete here.
            for (const auto& [resultVar, proj] : callProjections) {
                auto baseIt = subst.find(proj.baseVarId);
                if (baseIt == subst.end()) continue;
                TypePtr concrete = resolve(baseIt->second);
                std::string typeName;
                if (concrete->kind == TypeKind::Struct)
                    typeName = concrete->structName;
                else if (concrete->kind == TypeKind::Enum)
                    typeName = concrete->enumName;
                else if (concrete->kind == TypeKind::Int) typeName = "i64";
                else if (concrete->kind == TypeKind::Float) typeName = "f64";  // Phase 44
                else if (concrete->kind == TypeKind::Bool) typeName = "bool";
                else continue; // still a Var / unsupported — leave unbound
                auto tyIt = implAssocTypes_.find(typeName);
                if (tyIt == implAssocTypes_.end()) continue;
                auto trIt = tyIt->second.find(proj.traitName);
                if (trIt == tyIt->second.end()) continue;
                auto aIt = trIt->second.find(proj.assocName);
                if (aIt == trIt->second.end()) continue;
                unify(resultVar, aIt->second);
            }
            // Phase 19 (Send / compile-time data-race freedom): the closure
            // handed to `thread_spawn` must be `Send` — i.e. it must not
            // capture anything BY REFERENCE. A by-ref (FnMut) capture aliases a
            // stack slot of the spawning frame across the new thread; that is a
            // data race (two threads touching one unsynchronized slot) AND a
            // use-after-free once the spawning frame returns. By-VALUE captures
            // (i64 / bool / &T, incl. a Mutex i64 handle) are moved into the
            // thread and are fine. This is the enforced Send floor.
            if (call.callee == "thread_spawn" && !call.args.empty()) {
                std::string offending =
                    closureByRefCaptureName(*call.args[0]);
                if (!offending.empty()) {
                    error("cannot send a by-reference capture across a thread "
                          "boundary: closure passed to `thread_spawn` captures "
                          "`" + offending +
                              "` by reference (FnMut), which would alias the "
                              "spawning frame's stack across threads (data race "
                              "+ use-after-free). Capture it by value (move a "
                              "Copy value, or share via a Mutex handle) instead",
                          call.args[0]->line, call.args[0]->column);
                }
            }
            // PR#20: reject a unit element type for the Vec builtins. The
            // codegen vec_* specialization sizes the element via DataLayout;
            // a zero-sized unit element makes the stride 0 and crashes the
            // LLVM lowering. typeArgs[0] is now solved to the element type.
            if ((call.callee == "vec_new" || call.callee == "vec_push" ||
                 call.callee == "vec_get" || call.callee == "vec_len") &&
                !typeArgs.empty() &&
                resolve(typeArgs[0])->kind == TypeKind::Unit) {
                error("Vec element type cannot be unit", call.line,
                      call.column);
            }
            if (!schema.genericVars.empty()) {
                callInstantiations_[&call] = std::move(typeArgs);
            }
            // Phase 10a: record this call's effect contribution. Concrete
            // declared effects pass through; a declared effect-row-var name
            // resolves to its instantiated row var (just unified against the
            // actual argument), whose solved labels — or, if still
            // polymorphic, the enclosing fn's row-var name — are added. This
            // is what makes `apply(ioInc)` contribute `io` while
            // `apply(pureInc)` contributes nothing.
            {
                EffectSet contrib;
                for (const auto& l : schema.declaredEffects.labels) {
                    // Phase 12: calling an `async fn` only CONSTRUCTS its
                    // Future (inert until polled), so it does not perform the
                    // `async` effect in the caller — `.await` (or `block_on`)
                    // does. This lets a synchronous `main` call
                    // `block_on(work())` without itself being async, while an
                    // `async fn` that `.await`s still gets `async` from the
                    // AwaitExpr collection.
                    if (l == "async" && schema.isAsync) continue;
                    if (isBuiltinEffect(l)) { contrib.add(l); continue; }
                    TypePtr schemaVar;
                    for (const auto& [n, v] : schema.effectRowVars)
                        if (n == l) { schemaVar = v; break; }
                    if (!schemaVar) continue;
                    auto it = subst.find(resolve(schemaVar)->varId);
                    if (it == subst.end()) continue;
                    addRowVarContribution(it->second, contrib);
                }
                // Phase 23: `catch` is the panic HANDLER. Whatever effects the
                // callback `f` performs flow through via the row var `e`, but
                // its `panic` effect is CAUGHT here, so it must NOT propagate to
                // catch's caller — strip it from the recorded contribution.
                // (A fn whose only `panic` is inside a `catch` therefore need
                // not declare `panic` itself.)
                if (call.callee == "catch") {
                    contrib.labels.erase(
                        std::remove(contrib.labels.begin(),
                                    contrib.labels.end(), "panic"),
                        contrib.labels.end());
                }
                if (!contrib.labels.empty()) exprEffects_[&call] = contrib;
            }
            return instSig->ret;
        }
        // Not a fn — fall back to variant constructor.
        auto vl = lookupVariant(call.callee);
        if (vl.enumInstance) {
            const auto& variant =
                vl.enumInstance->enumVariants[vl.variantIdx];
            if (variant.payloadTypes.size() != call.args.size()) {
                error("constructor " + call.callee + " expects " +
                          std::to_string(variant.payloadTypes.size()) +
                          " arg(s), got " +
                          std::to_string(call.args.size()),
                      call.line, call.column);
            }
            const std::size_t n =
                std::min(variant.payloadTypes.size(), call.args.size());
            for (std::size_t i = 0; i < n; ++i) {
                TypePtr argType = checkExpr(*call.args[i]);
                if (!unify(argType, variant.payloadTypes[i])) {
                    error("argument " + std::to_string(i + 1) +
                              " to constructor " + call.callee +
                              " has type " + typeToString(argType) +
                              ", expected " +
                              typeToString(variant.payloadTypes[i]),
                          call.args[i]->line, call.args[i]->column);
                }
            }
            for (std::size_t i = n; i < call.args.size(); ++i) {
                checkExpr(*call.args[i]);
            }
            return vl.enumInstance;
        }

        error("unknown function: " + call.callee, call.line, call.column);
        for (const auto& a : call.args) checkExpr(*a);
        return makeInt();
    }

    // Phase 17a: type-check a call whose callee is an arbitrary EXPRESSION
    // (a parenthesized expr or a field access), e.g. `(s.f)(x)`. The callee
    // must resolve to a `Function` type; we unify the args against its
    // params and yield its return — the same indirect-call discipline
    // `checkCall` uses for fn-typed locals, lifted to a general callee expr.
    TypePtr checkCallValue(const ast::CallValueExpr& cv) {
        TypePtr calleeT = resolve(checkExpr(*cv.callee));
        if (calleeT->kind != TypeKind::Function) {
            error("called value is not a function (its type is " +
                      typeToString(calleeT) + ")",
                  cv.callee->line, cv.callee->column);
            for (const auto& a : cv.args) checkExpr(*a);
            return makeInt();
        }
        if (calleeT->args.size() != cv.args.size()) {
            error("indirect call expects " +
                      std::to_string(calleeT->args.size()) + " arg(s), got " +
                      std::to_string(cv.args.size()),
                  cv.line, cv.column);
        }
        const std::size_t n = std::min(calleeT->args.size(), cv.args.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr argType = checkExpr(*cv.args[i]);
            if (!unify(argType, calleeT->args[i])) {
                error("argument " + std::to_string(i + 1) +
                          " of indirect call has type " + typeToString(argType) +
                          ", expected " + typeToString(calleeT->args[i]),
                      cv.args[i]->line, cv.args[i]->column);
            }
        }
        for (std::size_t i = n; i < cv.args.size(); ++i) checkExpr(*cv.args[i]);
        // Phase 10a: an indirect call performs the effects carried by the fn
        // value's type. Mirror recordCallEffectsFromFnType against this node.
        {
            EffectSet contrib;
            for (const auto& l : calleeT->effectLabels) contrib.add(l);
            if (calleeT->effectRowVar)
                addRowVarContribution(calleeT->effectRowVar, contrib);
            if (!contrib.labels.empty()) exprEffects_[&cv] = contrib;
        }
        return calleeT->ret;
    }

    // Phase 15: prefix unary operators. `-x` negates an i64; `!x` logically
    // negates a bool. Both are total over their input type and reproduce it.
    TypePtr checkUnary(const ast::UnaryExpr& un) {
        TypePtr operand = checkExpr(*un.operand);
        switch (un.op) {
        case ast::UnaryOp::Neg:
            // Phase 39: `-x` negates an i64 OR an f64.
            if (resolve(operand)->kind == TypeKind::Float) return makeFloat();
            if (!unify(operand, makeInt())) {
                error("unary `-` requires an i64 or f64 operand, got " +
                          typeToString(operand),
                      un.operand->line, un.operand->column);
            }
            return makeInt();
        case ast::UnaryOp::Not:
            if (!unify(operand, makeBool())) {
                error("unary `!` requires a bool operand, got " +
                          typeToString(operand),
                      un.operand->line, un.operand->column);
            }
            return makeBool();
        case ast::UnaryOp::Deref: {
            // Phase 34: `*r` reads the pointee of a `&T` / `&mut T` / Box<T>.
            TypePtr r = resolve(operand);
            if (r->kind == TypeKind::Ref) return resolve(r->refInner);
            if (r->kind == TypeKind::Box) return resolve(r->refInner);
            error("unary `*` requires a reference or Box operand, got " +
                      typeToString(operand),
                  un.operand->line, un.operand->column);
            return makeInt();
        }
        }
        return makeInt(); // unreachable
    }

    TypePtr checkIf(const ast::IfExpr& ie) {
        TypePtr cond = checkExpr(*ie.cond);
        if (!unify(cond, makeBool())) {
            error("if condition must be bool, got " + typeToString(cond),
                  ie.cond->line, ie.cond->column);
        }
        TypePtr thenT = checkExpr(*ie.thenBranch);
        TypePtr elseT = checkExpr(*ie.elseBranch);
        if (!unify(thenT, elseT)) {
            error("if branches have mismatched types: then=" +
                      typeToString(thenT) + " else=" + typeToString(elseT),
                  ie.line, ie.column);
        }
        return thenT;
    }

    // Phase 9: `while cond { body }`. cond must be bool; body is checked
    // for unit (it's a statement-position block); the whole expr is unit.
    TypePtr checkWhile(const ast::WhileExpr& we) {
        TypePtr cond = checkExpr(*we.cond);
        if (!unify(cond, makeBool())) {
            error("while condition must be bool, got " + typeToString(cond),
                  we.cond->line, we.cond->column);
        }
        loopStack_.push_back(LoopCtx{/*isValueLoop=*/false, nullptr, false});
        TypePtr bodyT = checkExpr(*we.body);
        if (!unify(bodyT, makeUnit())) {
            error("while body must be unit-typed, got " + typeToString(bodyT),
                  we.body->line, we.body->column);
        }
        loopStack_.pop_back();
        return makeUnit();
    }

    // Phase 9: `loop { body }`. Body is unit (its tail value is ignored —
    // the loop only exits via `break`). The loop expression's type is the
    // unified type of all `break <value>` expressions, or unit if every
    // break is valueless / there are no breaks (the "never" case, which we
    // model as unit for the MVP).
    TypePtr checkLoop(const ast::LoopExpr& le) {
        loopStack_.push_back(LoopCtx{/*isValueLoop=*/true, nullptr, false});
        TypePtr bodyT = checkExpr(*le.body);
        if (!unify(bodyT, makeUnit())) {
            error("loop body must be unit-typed, got " + typeToString(bodyT),
                  le.body->line, le.body->column);
        }
        LoopCtx ctx = loopStack_.back();
        loopStack_.pop_back();
        if (ctx.breakType) {
            // At least one `break <value>`. If there was also a valueless
            // break, the loop has inconsistent break types.
            if (ctx.sawValuelessBreak) {
                error("loop has both `break` with and without a value",
                      le.line, le.column);
            }
            return ctx.breakType;
        }
        if (ctx.sawValuelessBreak) {
            // Every break is valueless: the loop completes with unit.
            return makeUnit();
        }
        // No `break` at all — the loop only exits via `return` (or never).
        // Its type is "never" (bottom); model it as a fresh Var so it
        // unifies with whatever the surrounding context requires.
        return makeFreshVar();
    }

    // Phase 9 / 13a: `for <pat> in <iter> { body }`.
    //
    //   - Fast path (Phase 9): when `<iter>` is the built-in `Range` (whether
    //     a literal `a..b` or a Range value), the pattern binds the i64
    //     element and codegen lowers the loop directly with an induction var.
    //   - General path (Phase 13a): when `<iter>` is any other type that
    //     impls `Iterator` (`fn next(&mut self) -> Option<i64>`), we desugar
    //     to `{ let mut __it = <iter>; loop { match __it.next() { Some(x) =>
    //     body, None => break } } }`. We synthesize the `__it.next()`
    //     MethodCallExpr on the ForExpr so typecheck (here) records its
    //     resolution and codegen reuses the same node.
    //
    // Body is unit; the whole expression is unit either way.
    TypePtr checkFor(const ast::ForExpr& fe) {
        TypePtr iterT = checkExpr(*fe.iter);
        TypePtr ir = resolve(iterT);

        // Element type bound to the loop pattern. Ranges yield i64; an
        // Iterator yields its `next()` Option payload (i64 in the MVP).
        TypePtr elemTy = makeInt();
        bool isRange = (ir->kind == TypeKind::Struct &&
                        ir->structName == "Range");

        if (!isRange) {
            // General Iterator path. The iterable's type must impl Iterator.
            std::string typeName = concreteTypeName(ir);
            bool hasNext = false;
            if (!typeName.empty()) {
                auto tIt = methodImplLookup_.find(typeName);
                if (tIt != methodImplLookup_.end()) {
                    auto mIt = tIt->second.find("next");
                    if (mIt != tIt->second.end() &&
                        mIt->second.first == "Iterator") {
                        hasNext = true;
                    }
                }
            }
            if (!hasNext) {
                error("for-loop iterable must be a range (a..b) or a type that "
                      "impls Iterator, got " + typeToString(iterT),
                      fe.iter->line, fe.iter->column);
                // Bind pattern to i64 and check the body so we still surface
                // body errors, then bail out as a unit.
                pushScope();
                Scope bindings;
                checkPattern(*fe.pattern, makeInt(), bindings);
                for (auto& kv : bindings) scopes_.back()[kv.first] = kv.second;
                loopStack_.push_back(LoopCtx{false, nullptr, false});
                checkExpr(*fe.body);
                loopStack_.pop_back();
                popScope();
                return makeUnit();
            }
            // Set up the desugar: a fresh iterator slot + a synthetic
            // `__it.next()` call whose receiver names that slot. We register
            // the slot name as a binding (type = the iterator) so resolving
            // the receiver finds it.
            fe.iteratorDesugar = true;
            fe.iterSlotName = "__for_it_" + std::to_string(forSlotCounter_++);
            auto recv = std::make_unique<ast::IdentExpr>();
            recv->name = fe.iterSlotName;
            recv->line = fe.iter->line;
            recv->column = fe.iter->column;
            auto nextCall = std::make_shared<ast::MethodCallExpr>();
            nextCall->methodName = "next";
            nextCall->line = fe.iter->line;
            nextCall->column = fe.iter->column;
            nextCall->receiver = std::move(recv);
            fe.nextCall = nextCall;

            // Resolve `__it.next()` against the iterator type. Bind the slot
            // name in a scope first so checkMethodCall's receiver resolves.
            pushScope();
            scopes_.back()[fe.iterSlotName] = iterT;
            // Record the receiver IdentExpr's type for codegen place-emission.
            exprTypes_[fe.nextCall->receiver.get()] = iterT;
            TypePtr nextRet = checkMethodCall(*fe.nextCall);
            exprTypes_[fe.nextCall.get()] = nextRet;
            popScope();

            // next() must return Option<i64>; the element is its Some payload.
            TypePtr nr = resolve(nextRet);
            bool validIteratorOption = false;
            if (nr->kind == TypeKind::Enum && nr->enumName == "Option") {
                for (const auto& v : nr->enumVariants) {
                    if (v.name == "Some" && !v.payloadTypes.empty()) {
                        elemTy = v.payloadTypes[0];
                        validIteratorOption = true;
                        break;
                    }
                }
            }
            if (!validIteratorOption) {
                error("Iterator::next must return Option with a payload-bearing "
                      "Some variant, got " + typeToString(nextRet),
                      fe.iter->line, fe.iter->column);
            }
        }

        // Bind the loop pattern (to the element type) in a fresh scope.
        pushScope();
        Scope bindings;
        checkPattern(*fe.pattern, elemTy, bindings);
        for (auto& kv : bindings) scopes_.back()[kv.first] = kv.second;
        loopStack_.push_back(LoopCtx{/*isValueLoop=*/false, nullptr, false});
        TypePtr bodyT = checkExpr(*fe.body);
        if (!unify(bodyT, makeUnit())) {
            error("for body must be unit-typed, got " + typeToString(bodyT),
                  fe.body->line, fe.body->column);
        }
        loopStack_.pop_back();
        popScope();
        return makeUnit();
    }

    // Phase 9: `a..b` / `a..=b`. Both endpoints must be i64; the result is
    // the built-in `Range` struct.
    TypePtr checkRange(const ast::RangeExpr& re) {
        TypePtr s = checkExpr(*re.start);
        TypePtr e = checkExpr(*re.end);
        if (!unify(s, makeInt())) {
            error("range start must be i64, got " + typeToString(s),
                  re.start->line, re.start->column);
        }
        if (!unify(e, makeInt())) {
            error("range end must be i64, got " + typeToString(e),
                  re.end->line, re.end->column);
        }
        auto it = structSchemas_.find("Range");
        return it != structSchemas_.end() ? it->second.type
                                          : makeStruct("Range", {});
    }

    // Phase 13b: `&v[a..b]` — slice a Vec. The operand must be a `Vec<T>`
    // (a `&Vec<T>` is auto-peeled); `a`/`b` must be i64. The result type is
    // the slice `&[T]` (== makeSlice(T)). Bounds are not statically checked
    // (a runtime view, like vec_get); codegen trusts a <= b <= len.
    TypePtr checkSlice(const ast::SliceExpr& se) {
        TypePtr opTy = resolve(checkExpr(*se.operand));
        // Peel a borrow: `&v` and `v` both slice the same Vec.
        if (opTy->kind == TypeKind::Ref) opTy = resolve(opTy->refInner);
        TypePtr elem;
        if (opTy->kind == TypeKind::Struct && opTy->structName == "Vec" &&
            !opTy->typeArgs.empty()) {
            elem = opTy->typeArgs[0];
        } else {
            error("slice operand must be a Vec, got " + typeToString(opTy),
                  se.operand->line, se.operand->column);
            elem = makeInt();
        }
        TypePtr s = checkExpr(*se.start);
        TypePtr en = checkExpr(*se.end);
        if (!unify(s, makeInt())) {
            error("slice start must be i64, got " + typeToString(s),
                  se.start->line, se.start->column);
        }
        if (!unify(en, makeInt())) {
            error("slice end must be i64, got " + typeToString(en),
                  se.end->line, se.end->column);
        }
        return makeSlice(elem);
    }

    // Phase 22: is `t` a Copy value type allowed as an array/tuple element in
    // the MVP? i64, bool, unit, and nested arrays/tuples of those. Non-Copy
    // aggregates (Vec/String/struct/enum/Box/refs) are rejected so we never
    // need to track moves through array/tuple element copies (stated as an MVP
    // restriction — full move-tracking through aggregates is deferred).
    bool isCopyAggregateElem(const TypePtr& t) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Int:
        case TypeKind::Float: // Phase 39: f64 is Copy
        case TypeKind::Bool:
        case TypeKind::Unit:
            return true;
        case TypeKind::Array:
            return isCopyAggregateElem(r->arrayElem);
        case TypeKind::Tuple:
            for (const auto& el : r->tupleElems)
                if (!isCopyAggregateElem(el)) return false;
            return true;
        default:
            return false;
        }
    }

    // Phase 22: array literal `[a, b, c]`. All elements must share one type T;
    // the result type is `[T; N]`. An empty `[]` can't infer T -> error.
    TypePtr checkArrayLit(const ast::ArrayLitExpr& al) {
        if (al.elements.empty()) {
            error("empty array literal `[]` cannot infer its element type; "
                  "annotate with `let a: [T; 0] = [];` is not yet supported",
                  al.line, al.column);
            return makeArray(makeInt(), 0);
        }
        TypePtr elemTy = checkExpr(*al.elements[0]);
        for (std::size_t i = 1; i < al.elements.size(); ++i) {
            TypePtr et = checkExpr(*al.elements[i]);
            if (!unify(et, elemTy)) {
                error("array element " + std::to_string(i) + " has type " +
                          typeToString(et) + ", expected " +
                          typeToString(elemTy) +
                          " (all array elements must share one type)",
                      al.elements[i]->line, al.elements[i]->column);
            }
        }
        if (!isCopyAggregateElem(elemTy)) {
            error("array elements must be Copy types (i64, bool, or nested "
                  "arrays/tuples of those) in this version, got " +
                      typeToString(elemTy),
                  al.line, al.column);
        }
        return makeArray(elemTy, al.elements.size());
    }

    // Phase 22: indexing `arr[i]` reads element i of a fixed-size array. The
    // index is i64; a compile-time-constant out-of-range literal index is an
    // error (a dynamic index is unchecked in the MVP).
    TypePtr checkIndex(const ast::IndexExpr& ix) {
        TypePtr objTy = resolve(checkExpr(*ix.object));
        // Auto-deref `&[T; N]` so `(&a)[i]` works like `a[i]`.
        while (objTy->kind == TypeKind::Ref) objTy = resolve(objTy->refInner);
        TypePtr idxTy = checkExpr(*ix.index);
        if (!unify(idxTy, makeInt())) {
            error("array index must be i64, got " + typeToString(idxTy),
                  ix.index->line, ix.index->column);
        }
        if (objTy->kind != TypeKind::Array) {
            error("indexing `[i]` requires a fixed-size array, got " +
                      typeToString(objTy) +
                      " (Vec/slice use vec_get / slice_get)",
                  ix.object->line, ix.object->column);
            return makeInt();
        }
        // Compile-time bounds check for a constant literal index.
        if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(ix.index.get())) {
            if (lit->value < 0 ||
                static_cast<std::size_t>(lit->value) >= objTy->arrayLen) {
                error("array index " + std::to_string(lit->value) +
                          " out of bounds for array of length " +
                          std::to_string(objTy->arrayLen),
                      ix.index->line, ix.index->column);
            }
        }
        return objTy->arrayElem;
    }

    // Phase 22 / 36: tuple literal `(a, b, ...)`. The empty `()` is unit;
    // otherwise the type is the tuple of the element types. Phase 36 lifts the
    // Copy-only restriction: a tuple may now hold non-Copy (heap-owning)
    // elements like `(String, i64)` or `(Json, i64)`. Such a tuple is itself
    // droppable (codegen drops each droppable element) and is move-tracked as a
    // whole by the borrow checker — moving the tuple moves its elements; tuple
    // field READS of a non-Copy element are rejected by the borrow checker
    // unless the element is Copy (no partial move-out via `.0` in this MVP —
    // destructure with a tuple pattern instead). Arrays stay Copy-only (their
    // dynamic index makes element-granular move tracking unsound here).
    TypePtr checkTupleLit(const ast::TupleLitExpr& tl) {
        if (tl.elements.empty()) return makeUnit(); // 0-tuple == unit
        std::vector<TypePtr> elems;
        elems.reserve(tl.elements.size());
        for (const auto& el : tl.elements) {
            elems.push_back(checkExpr(*el));
        }
        return makeTuple(std::move(elems));
    }

    // Phase 22: tuple field access `t.0`, `t.1`.
    TypePtr checkTupleField(const ast::TupleFieldExpr& tf) {
        TypePtr objTy = resolve(checkExpr(*tf.object));
        // Auto-deref `&(A, B)` so `(&t).0` works like `t.0`.
        while (objTy->kind == TypeKind::Ref) objTy = resolve(objTy->refInner);
        if (objTy->kind != TypeKind::Tuple) {
            error("tuple field access `." + std::to_string(tf.index) +
                      "` requires a tuple, got " + typeToString(objTy),
                  tf.line, tf.column);
            return makeInt();
        }
        if (tf.index >= objTy->tupleElems.size()) {
            error("tuple index " + std::to_string(tf.index) +
                      " out of range for tuple with " +
                      std::to_string(objTy->tupleElems.size()) + " element(s)",
                  tf.line, tf.column);
            return makeInt();
        }
        return objTy->tupleElems[tf.index];
    }

    // Phase 9: `break` / `break <value>`. Validates a loop is active and,
    // for valued breaks, that the enclosing loop is a `loop` (not while/
    // for) and that the value type unifies across all breaks.
    TypePtr checkBreak(const ast::BreakExpr& be) {
        if (loopStack_.empty()) {
            error("`break` outside of a loop", be.line, be.column);
            if (be.value) checkExpr(*be.value);
            return makeFreshVar();
        }
        LoopCtx& ctx = loopStack_.back();
        if (be.value) {
            TypePtr vT = checkExpr(*be.value);
            if (!ctx.isValueLoop) {
                error("`break` with a value is only allowed inside `loop`",
                      be.line, be.column);
            } else if (!ctx.breakType) {
                ctx.breakType = vT;
            } else if (!unify(vT, ctx.breakType)) {
                error("`break` value type " + typeToString(vT) +
                          " conflicts with earlier break type " +
                          typeToString(ctx.breakType),
                      be.line, be.column);
            }
        } else {
            ctx.sawValuelessBreak = true;
        }
        // `break` diverges; give it a fresh var so it unifies with any
        // surrounding context (like a `return`).
        return makeFreshVar();
    }

    // Phase 9: `continue`. Only valid inside a loop; diverges.
    TypePtr checkContinue(const ast::ContinueExpr& ce) {
        if (loopStack_.empty()) {
            error("`continue` outside of a loop", ce.line, ce.column);
        }
        return makeFreshVar();
    }

    // Phase 9: `lhs = rhs;`. The target must be an assignable place:
    //   - a bare Ident bound by `let mut`, or
    //   - a field-access chain `place.f` whose root is assignable OR a
    //     `&mut` reference (so `&mut self`'s fields are writable).
    // Types must unify.
    void checkAssign(const ast::AssignStmt& as) {
        TypePtr targetT = checkExpr(*as.target);
        TypePtr valT = checkExpr(*as.value);
        if (!isAssignablePlace(*as.target)) {
            error("cannot assign to this expression; the target is not a "
                  "mutable place",
                  as.target->line, as.target->column);
        }
        if (!unify(targetT, valT)) {
            error("assignment type mismatch: target is " +
                      typeToString(targetT) + ", value is " +
                      typeToString(valT),
                  as.line, as.column);
        }
    }

    // Is `e` a place we may assign to? A `let mut` Ident, or a chain of field
    // accesses (`p.x`), array indices (`a[i]`, Phase 22), and tuple-field
    // accesses (`t.0`, Phase 22) rooted at a `let mut` Ident or at a `&mut`
    // reference (e.g. through a `&mut self` receiver).
    bool isAssignablePlace(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            return isMutLocal(id->name);
        }
        // Walk to the root of the place chain through field / index / tuple-
        // field projections (each just narrows into the same root storage).
        const ast::Expr* root = &e;
        while (true) {
            if (auto* fe = dynamic_cast<const ast::FieldExpr*>(root)) {
                root = fe->object.get();
            } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(root)) {
                root = ix->object.get();
            } else if (auto* tf =
                           dynamic_cast<const ast::TupleFieldExpr*>(root)) {
                root = tf->object.get();
            } else {
                break;
            }
        }
        if (root == &e) return false; // not a projection chain
        if (auto* rootId = dynamic_cast<const ast::IdentExpr*>(root)) {
            // Mutable local, OR a binding of `&mut T` (mutable ref — its
            // pointee fields/elements are writable).
            if (isMutLocal(rootId->name)) return true;
            TypePtr rt = lookupLocal(rootId->name);
            if (rt) {
                TypePtr r = resolve(rt);
                if (r->kind == TypeKind::Ref && r->refIsMut) return true;
            }
        }
        return false;
    }

    TypePtr checkBlock(const ast::BlockExpr& block) {
        pushScope();
        bool diverges = false;
        for (const auto& stmt : block.stmts) {
            checkStmt(*stmt);
            if (dynamic_cast<const ast::ReturnStmt*>(stmt.get())) {
                diverges = true;
            }
        }
        TypePtr result;
        if (block.tail) {
            result = checkExpr(*block.tail);
        } else if (diverges) {
            // Control never reaches a tail value (block ended in `return`).
            // Give it a fresh type variable so unification with a sibling
            // branch (e.g. the other arm of an `if`) succeeds — bottom
            // unifies with anything.
            result = makeFreshVar();
        } else {
            result = makeUnit();
        }
        popScope();
        return result;
    }

    TypePtr checkMatch(const ast::MatchExpr& me) {
        TypePtr scrutT = checkExpr(*me.scrutinee);
        // Phase 34: match-through-reference. When the scrutinee is `&Enum`
        // (one layer of `&`/`&mut`), match the pointee enum and bind every
        // payload as a borrow `&T` — so a recursive heap value can be
        // traversed without moving/copying it out (codegen reads through the
        // pointer; the scrutinee `&x` borrows x rather than consuming it).
        TypePtr patExpected = scrutT;
        bool refMatch = false;
        {
            TypePtr rs = resolve(scrutT);
            if (rs->kind == TypeKind::Ref) {
                TypePtr inner = resolve(rs->refInner);
                if (inner->kind == TypeKind::Enum) {
                    refMatch = true;
                    patExpected = inner;
                }
            }
        }
        if (me.arms.empty()) {
            error("match expression must have at least one arm",
                  me.line, me.column);
            return makeFreshVar();
        }
        // Snapshot error count before arm checking; if it grows during arm
        // pattern/body checks the patterns are likely malformed (wrong
        // arity, unknown ctor, duplicate binding, etc.). We skip the
        // pattern_match calls in that case to avoid feeding malformed
        // inputs through `findRedundantArms` / `compileDecisionTree`,
        // which can trip internal invariants. The match still gets a
        // proper TypeError report; codegen only consumes the DT when the
        // program is `ok()`, so a missing DT entry for an ill-typed match
        // is harmless.
        const std::size_t errsBeforeArms = errors_.size();
        TypePtr unified;
        for (const auto& arm : me.arms) {
            pushScope();
            // Type the pattern against the scrutinee type. Errors are
            // recorded inline; we still process the body so secondary
            // type errors surface.
            Scope bindings;
            checkPattern(*arm.pattern, patExpected, bindings, refMatch);
            for (auto& kv : bindings) {
                scopes_.back()[kv.first] = kv.second;
                // Phase 29: record the binding's type so codegen can drop a
                // droppable payload binding at arm-scope exit.
                matchBindingTypes_[&arm][kv.first] = kv.second;
            }
            TypePtr bodyT = checkExpr(*arm.body);
            popScope();
            if (!unified) {
                unified = bodyT;
            } else if (!unify(bodyT, unified)) {
                error("arm body type mismatch: got " + typeToString(bodyT) +
                          ", expected " + typeToString(unified),
                      arm.line, arm.column);
            }
        }
        if (!unified) unified = makeFreshVar();
        // pattern_match operates on schema variant lists, not on instance
        // typeArgs. Repackage enumSchemas_'s schema TypePtrs into the
        // legacy map<string, TypePtr> shape its API expects.
        std::unordered_map<std::string, TypePtr> enumsForPm;
        enumsForPm.reserve(enumSchemas_.size());
        for (const auto& [n, s] : enumSchemas_) enumsForPm[n] = s.type;
        if (auto w = pattern_match::checkExhaustiveness(
                patExpected, me.arms, enumsForPm, variantIndex_)) {
            error("non-exhaustive match: missing pattern `" + w->text + "`",
                  me.line, me.column);
        }
        const bool armsClean = (errors_.size() == errsBeforeArms);
        if (armsClean) {
            // Redundancy: report each arm unreachable given the arms before it.
            auto redundant = pattern_match::findRedundantArms(
                patExpected, me.arms, enumsForPm, variantIndex_);
            for (unsigned idx : redundant) {
                if (idx >= me.arms.size()) continue;
                const auto& arm = me.arms[idx];
                error("unreachable match arm: pattern already covered by an "
                      "earlier arm",
                      arm.line, arm.column);
            }
            // Build the Maranget decision tree for codegen. Always store
            // (even when non-exhaustive — the tree bottoms out at Fail
            // nodes), as long as arm patterns were well-typed.
            matchTrees_[&me] = pattern_match::compileDecisionTree(
                patExpected, me.arms, enumsForPm, variantIndex_);
        }
        return unified;
    }

    // Walk a pattern, unify against the expected type, and populate
    // `bindings` with name -> type for any VarPat encountered.
    //
    // VarPat-rewrite rule: a bare Ident in pattern position is parsed as
    // VarPat. If the name resolves to a known UNIT variant, we treat it
    // as a unit-CtorPat (matches the variant, no binding). Names that
    // collide with non-unit variants are likely a forgotten parenthesis
    // and are flagged as an arity error rather than silently binding.
    // Phase 34: `refMatch` is true when the scrutinee is `&Enum` (a
    // match-through-reference). Then a bound name binds a BORROW `&T` of the
    // matched location rather than an owned/copied `T`, so traversing a
    // recursive heap value never moves/double-frees it.
    void checkPattern(const ast::Pattern& pat, const TypePtr& expected,
                      Scope& bindings, bool refMatch = false) {
        if (dynamic_cast<const ast::LitIntPat*>(&pat)) {
            if (!unify(expected, makeInt())) {
                error("integer pattern requires i64, scrutinee is " +
                          typeToString(expected),
                      pat.line, pat.column);
            }
            return;
        }
        if (dynamic_cast<const ast::WildPat*>(&pat)) {
            return;
        }
        if (auto* vp = dynamic_cast<const ast::VarPat*>(&pat)) {
            auto vl = lookupVariant(vp->name);
            if (vl.enumInstance) {
                const auto& variant =
                    vl.enumInstance->enumVariants[vl.variantIdx];
                if (!variant.payloadTypes.empty()) {
                    error("constructor " + vp->name + " requires " +
                              std::to_string(variant.payloadTypes.size()) +
                              " argument(s) in pattern",
                          pat.line, pat.column);
                    return;
                }
                if (!unify(expected, vl.enumInstance)) {
                    error("pattern matches enum " +
                              vl.enumInstance->enumName +
                              ", scrutinee is " + typeToString(expected),
                          pat.line, pat.column);
                }
                return;
            }
            if (bindings.count(vp->name)) {
                error("duplicate binding '" + vp->name + "' in pattern",
                      pat.line, pat.column);
                return;
            }
            // Phase 34: in a match-through-reference, bind a borrow `&T`.
            bindings[vp->name] =
                refMatch ? makeRef(expected, /*isMut=*/false) : expected;
            return;
        }
        if (auto* cp = dynamic_cast<const ast::CtorPat*>(&pat)) {
            auto vl = lookupVariant(cp->ctorName);
            if (!vl.enumInstance) {
                error("unknown constructor " + cp->ctorName,
                      pat.line, pat.column);
                // Still walk subpatterns so nested errors / bindings surface
                // against fresh vars.
                for (const auto& sp : cp->subpatterns) {
                    checkPattern(*sp, makeFreshVar(), bindings, refMatch);
                }
                return;
            }
            if (!unify(expected, vl.enumInstance)) {
                error("pattern matches enum " + vl.enumInstance->enumName +
                          ", scrutinee is " + typeToString(expected),
                      pat.line, pat.column);
            }
            const auto& variant =
                vl.enumInstance->enumVariants[vl.variantIdx];
            if (cp->subpatterns.size() != variant.payloadTypes.size()) {
                error("constructor " + cp->ctorName + " expects " +
                          std::to_string(variant.payloadTypes.size()) +
                          " arg(s), got " +
                          std::to_string(cp->subpatterns.size()),
                      pat.line, pat.column);
            }
            const std::size_t n =
                std::min(cp->subpatterns.size(), variant.payloadTypes.size());
            for (std::size_t i = 0; i < n; ++i) {
                checkPattern(*cp->subpatterns[i], variant.payloadTypes[i],
                             bindings, refMatch);
            }
            // Walk any extras against fresh vars to surface their bindings/errors.
            for (std::size_t i = n; i < cp->subpatterns.size(); ++i) {
                checkPattern(*cp->subpatterns[i], makeFreshVar(), bindings,
                             refMatch);
            }
            return;
        }
        // Phase 36: a tuple destructure `(p0, p1, ...)` — the scrutinee must be
        // a tuple of matching arity; bind each element sub-pattern to its type.
        if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
            TypePtr exp = resolve(expected);
            if (exp->kind != TypeKind::Tuple) {
                std::vector<TypePtr> fresh;
                fresh.reserve(tp->elements.size());
                for (std::size_t i = 0; i < tp->elements.size(); ++i)
                    fresh.push_back(makeFreshVar());
                if (!unify(expected, makeTuple(fresh))) {
                    error("tuple pattern, but scrutinee is " +
                              typeToString(expected),
                          pat.line, pat.column);
                    for (const auto& sp : tp->elements)
                        checkPattern(*sp, makeFreshVar(), bindings, refMatch);
                    return;
                }
                exp = resolve(expected);
            }
            if (exp->tupleElems.size() != tp->elements.size()) {
                error("tuple pattern has " +
                          std::to_string(tp->elements.size()) +
                          " element(s), scrutinee tuple has " +
                          std::to_string(exp->tupleElems.size()),
                      pat.line, pat.column);
            }
            const std::size_t n =
                std::min(tp->elements.size(), exp->tupleElems.size());
            for (std::size_t i = 0; i < n; ++i)
                checkPattern(*tp->elements[i], exp->tupleElems[i], bindings,
                             refMatch);
            return;
        }
        error("unknown pattern kind", pat.line, pat.column);
    }

    void checkStmt(const ast::Stmt& s) {
        if (auto* let = dynamic_cast<const ast::LetStmt*>(&s)) {
            TypePtr valT = checkExpr(*let->value);
            // Phase 22: tuple-destructuring `let (x, y) = t;`. The RHS must be
            // a tuple of matching arity; each non-`_` name binds to the
            // corresponding element type. `mut` applies to every bound name.
            if (!let->tupleNames.empty()) {
                TypePtr r = resolve(valT);
                if (r->kind != TypeKind::Tuple) {
                    error("tuple-destructuring `let (...)` requires a tuple "
                          "value, got " + typeToString(valT),
                          let->line, let->column);
                    return;
                }
                if (r->tupleElems.size() != let->tupleNames.size()) {
                    error("tuple pattern binds " +
                              std::to_string(let->tupleNames.size()) +
                              " name(s) but the value is a tuple with " +
                              std::to_string(r->tupleElems.size()) +
                              " element(s)",
                          let->line, let->column);
                    return;
                }
                for (std::size_t i = 0; i < let->tupleNames.size(); ++i) {
                    if (let->tupleNames[i] == "_") continue;
                    scopes_.back()[let->tupleNames[i]] = r->tupleElems[i];
                    if (let->isMut) markMut(let->tupleNames[i]);
                }
                return;
            }
            // Phase 11: an explicit annotation gives the binding's type and
            // is a coercion target (e.g. `let b: Box<dyn Shape> = Box::new(
            // Sq{..})` coerces `Box<Sq>` into `Box<dyn Shape>`).
            if (let->annotation) {
                TypePtr annotTy = resolveTypeRef(*let->annotation);
                if (!coerceOrUnify(*let->value, valT, annotTy)) {
                    error("let binding '" + let->name + "' has annotated type " +
                              typeToString(annotTy) + " but value has type " +
                              typeToString(valT),
                          let->line, let->column);
                }
                scopes_.back()[let->name] = annotTy;
            } else {
                scopes_.back()[let->name] = valT;
            }
            if (let->isMut) markMut(let->name);
            markByRefClosureLocal(let->name, closureEscapesByRef(*let->value));
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignStmt*>(&s)) {
            checkAssign(*as);
            if (auto* id = dynamic_cast<const ast::IdentExpr*>(as->target.get())) {
                markByRefClosureLocal(id->name, closureEscapesByRef(*as->value));
            }
            return;
        }
        if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (ret->value) {
                TypePtr valT = checkExpr(*ret->value);
                // Phase 17a: a closure that captures BY REFERENCE holds a
                // pointer into its defining frame; returning it would dangle.
                // Reject (MVP keeps such closures non-escaping).
                if (closureEscapesByRef(*ret->value)) {
                    error("cannot return a closure that captures a variable by "
                          "reference (FnMut); its environment would point into "
                          "the dead stack frame",
                          ret->value->line, ret->value->column);
                }
                // Phase 11: `return &concrete;` from a `-> &dyn Trait` fn
                // coerces just like an argument / annotated let.
                if (currentReturnType_ &&
                    !coerceOrUnify(*ret->value, valT, currentReturnType_)) {
                    error("return value type " + typeToString(valT) +
                              " does not match function return type " +
                              typeToString(currentReturnType_),
                          ret->value->line, ret->value->column);
                }
            } else if (currentReturnType_ &&
                       resolve(currentReturnType_)->kind != TypeKind::Unit) {
                error("empty 'return' in function returning " +
                          typeToString(currentReturnType_),
                      s.line, s.column);
            }
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
            checkExpr(*es->expr);
            return;
        }
    }
};

} // namespace

// Out-of-line special members: the `matchTrees` field holds
// unique_ptr<pattern_match::DecisionTree>, which is incomplete in the
// public header. Defining these here (where the full type is visible via
// the pattern_match.hpp include above) lets `unique_ptr` instantiate its
// deleter correctly.
TypeCheckResult::TypeCheckResult() = default;
TypeCheckResult::~TypeCheckResult() = default;
TypeCheckResult::TypeCheckResult(TypeCheckResult&&) noexcept = default;
TypeCheckResult& TypeCheckResult::operator=(TypeCheckResult&&) noexcept =
    default;

TypeCheckResult typecheck(const ast::Program& program) {
    TypeChecker tc;
    return tc.check(program);
}

} // namespace kardashev
