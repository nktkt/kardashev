#include <cstdio>
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
        // v12 Phase 69: int_to_hex(n: i64) -> String ! { alloc } — lowercase
        // hexadecimal (no `0x` prefix; the two's-complement bit pattern for a
        // negative n, like Rust's `{:x}`). snprintf "%llx".
        {
            FnSchema sch;
            sch.signature = makeFunction({makeInt()}, stringTy);
            sch.declaredEffects.add("alloc");
            fnSchemas_["int_to_hex"] = std::move(sch);
        }
        // v12 Phase 69: the low-level string->number parse primitives. Each
        // writes the parsed value through an out-pointer and returns whether the
        // ENTIRE string was a valid number (no leftover characters). The
        // Option-returning `parse_int` / `parse_f64` are kardashev prelude
        // wrappers over these (see applyPrelude). Pure: they only read the
        // input and use a transient stack buffer.
        //   str_parse_i64(s: &String, out: &mut i64) -> bool
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false),
                 makeRef(makeInt(), /*isMut=*/true)},
                makeBool());
            fnSchemas_["str_parse_i64"] = std::move(sch);
        }
        //   str_parse_f64(s: &String, out: &mut f64) -> bool
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(stringTy, /*isMut=*/false),
                 makeRef(makeFloat(), /*isMut=*/true)},
                makeBool());
            fnSchemas_["str_parse_f64"] = std::move(sch);
        }
        // v12 Phase 73: f64 math (f64 -> f64), pure. sqrt / floor / ceil / abs.
        for (const char* nm :
             {"f64_sqrt", "f64_floor", "f64_ceil", "f64_abs"}) {
            FnSchema sch;
            sch.signature = makeFunction({makeFloat()}, makeFloat());
            fnSchemas_[nm] = std::move(sch);
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
        // Phase 47: vec_swap<T>(v: &mut Vec<T>, i: i64, j: i64) -> i64 — exchange
        // two element slots IN PLACE. Ownership-neutral (the two elements trade
        // storage; no clone, no drop), so it works for non-Copy T — the
        // primitive an in-place `sort<T: Ord>` needs.
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true), makeInt(), makeInt()},
                makeInt());
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_swap"] = std::move(sch);
        }
        // v12 Phase 70: Vec mutation. vec_pop / vec_remove MOVE the element out
        // (len decremented, so the Vec no longer owns that slot — no clone, no
        // double-free; the dual of vec_get which CLONES because the Vec keeps
        // its copy). An empty/out-of-range index yields a zero (vec_get's
        // contract). vec_insert may grow (alloc); the others are pure mutators.
        // vec_pop<T>(v: &mut Vec<T>) -> T
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true)}, vecFnGenericVar);
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_pop"] = std::move(sch);
        }
        // vec_remove<T>(v: &mut Vec<T>, i: i64) -> T
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true), makeInt()},
                vecFnGenericVar);
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_remove"] = std::move(sch);
        }
        // vec_insert<T>(v: &mut Vec<T>, i: i64, x: T) -> i64 ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true), makeInt(), vecFnGenericVar},
                makeInt());
            sch.declaredEffects.add("alloc");
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_insert"] = std::move(sch);
        }
        // vec_reverse<T>(v: &mut Vec<T>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/true)}, makeInt());
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_reverse"] = std::move(sch);
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

        // Phase 76 (v13): `Sender<T>` / `Receiver<T>` — typed channel endpoints.
        // Both lower to a single i64 HANDLE (a heap pointer to the shared
        // channel block), so they are Copy — a Sender can be captured BY VALUE
        // into a producer thread's closure (the cross-thread send path). They
        // are DISTINCT named types so the receiver endpoint can be marked
        // single-consumer / not-Send (Phase 77): a Receiver may not be moved
        // into a second thread.
        TypePtr senderVar = makeFreshVar();
        TypePtr senderInst = makeStruct("Sender", {});
        senderInst->typeArgs = {senderVar};
        {
            StructSchema sch;
            sch.type = senderInst;
            sch.genericVars.push_back(senderVar);
            structSchemas_["Sender"] = std::move(sch);
        }
        TypePtr receiverVar = makeFreshVar();
        TypePtr receiverInst = makeStruct("Receiver", {});
        receiverInst->typeArgs = {receiverVar};
        {
            StructSchema sch;
            sch.type = receiverInst;
            sch.genericVars.push_back(receiverVar);
            structSchemas_["Receiver"] = std::move(sch);
        }

        // Phase 78 (v13): `Rc<T>` — a NON-ATOMIC reference-counted shared owner
        // (a pointer to a heap `{ i64 strong, T value }`). rc_clone shares (the
        // count++); the value is dropped + freed when the last Rc drops. It is
        // the canonical NOT-Send type: its refcount is non-atomic, so two
        // threads racing on rc_clone/drop would corrupt it — sending or
        // capturing an Rc across a thread is a compile error (isSend(Rc)=false).
        // The legible witness for the Send rule (vs sharing safely via Mutex).
        TypePtr rcVar = makeFreshVar();
        TypePtr rcInst = makeStruct("Rc", {});
        rcInst->typeArgs = {rcVar};
        {
            StructSchema sch;
            sch.type = rcInst;
            sch.genericVars.push_back(rcVar);
            structSchemas_["Rc"] = std::move(sch);
        }
        // rc_new<T>(v: T) -> Rc<T> ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({rcVar}, rcInst);
            sch.declaredEffects.add("alloc");
            sch.genericVars.push_back(rcVar);
            fnSchemas_["rc_new"] = std::move(sch);
        }
        // rc_clone<T>(r: &Rc<T>) -> Rc<T>  (shares; bumps the strong count)
        {
            FnSchema sch;
            sch.signature =
                makeFunction({makeRef(rcInst, /*isMut=*/false)}, rcInst);
            sch.genericVars.push_back(rcVar);
            fnSchemas_["rc_clone"] = std::move(sch);
        }
        // rc_get<T>(r: &Rc<T>) -> &T   (borrow the shared value)
        {
            FnSchema sch;
            sch.signature = makeFunction({makeRef(rcInst, /*isMut=*/false)},
                                         makeRef(rcVar, /*isMut=*/false));
            sch.genericVars.push_back(rcVar);
            fnSchemas_["rc_get"] = std::move(sch);
        }
        // rc_strong_count<T>(r: &Rc<T>) -> i64
        {
            FnSchema sch;
            sch.signature =
                makeFunction({makeRef(rcInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(rcVar);
            fnSchemas_["rc_strong_count"] = std::move(sch);
        }

        // v31 Phase 171: `Arc<T>` — an ATOMICALLY reference-counted shared owner
        // (a pointer to a heap `{ i64 strong, i64 weak, T value }`), and its
        // companion `Weak<T>` (a non-owning, upgradable handle). Unlike Rc,
        // Arc's refcount is atomic, so it IS Send/Sync when T is (Send+Sync) —
        // the answer to "share owned data across threads" without lifetimes.
        // arc_new strong=1/weak=1 (all strong refs collectively hold one weak
        // ref); the value is dropped when the last strong drops, the block freed
        // when the last weak drops. Mirrors the Rc schema shape.
        TypePtr arcVar = makeFreshVar();
        TypePtr arcInst = makeStruct("Arc", {});
        arcInst->typeArgs = {arcVar};
        TypePtr weakInst = makeStruct("Weak", {});
        weakInst->typeArgs = {arcVar};
        {
            StructSchema sch;
            sch.type = arcInst;
            sch.genericVars.push_back(arcVar);
            structSchemas_["Arc"] = std::move(sch);
        }
        {
            StructSchema sch;
            sch.type = weakInst;
            sch.genericVars.push_back(arcVar);
            structSchemas_["Weak"] = std::move(sch);
        }
        auto regArc = [&](const std::string& nm, std::vector<TypePtr> params,
                          TypePtr ret, bool alloc) {
            FnSchema sch;
            sch.signature = makeFunction(std::move(params), std::move(ret));
            if (alloc) sch.declaredEffects.add("alloc");
            sch.genericVars.push_back(arcVar);
            fnSchemas_[nm] = std::move(sch);
        };
        regArc("arc_new", {arcVar}, arcInst, true);
        regArc("arc_clone", {makeRef(arcInst, false)}, arcInst, false);
        regArc("arc_get", {makeRef(arcInst, false)}, makeRef(arcVar, false),
               false);
        regArc("arc_strong_count", {makeRef(arcInst, false)}, makeInt(), false);
        regArc("arc_weak_count", {makeRef(arcInst, false)}, makeInt(), false);
        regArc("arc_downgrade", {makeRef(arcInst, false)}, weakInst, false);
        regArc("weak_clone", {makeRef(weakInst, false)}, weakInst, false);
        // weak_upgrade<T>(w: &Weak<T>) -> Option<Arc<T>> is registered AFTER the
        // enum loop (Option is not yet in enumSchemas_ at this point).
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
        // Phase 122 (v21): hashset_remove<T>(s: &mut HashSet<T>, k: T) -> bool
        // — true if the element was present (and is now removed), false if it
        // was absent. `&mut` (it mutates the table).
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/true), hsVar}, makeBool());
            sch.genericVars.push_back(hsVar);
            fnSchemas_["hashset_remove"] = std::move(sch);
        }
        // hashset_len<T>(s: &HashSet<T>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(hsVar);
            fnSchemas_["hashset_len"] = std::move(sch);
        }
        // v12 Phase 71: hashset_items<T>(s: &HashSet<T>) -> Vec<T> ! { alloc } —
        // enumerate the set (deep-cloned items, bucket order). The set is only
        // borrowed; the Vec owns its copies.
        {
            FnSchema sch;
            TypePtr vecOfElem = makeStruct("Vec", {});
            vecOfElem->typeArgs = {hsVar};
            sch.signature = makeFunction(
                {makeRef(hashSetInst, /*isMut=*/false)}, vecOfElem);
            sch.genericVars.push_back(hsVar);
            sch.declaredEffects.add("alloc");
            fnSchemas_["hashset_items"] = std::move(sch);
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
        // v32 Phase 172: `JoinHandle<T>` — a type-safe, move-only handle to a
        // spawned task. Lowers to the same i64 task index as the old bare handle
        // (codegen maps JoinHandle<T> -> i64, like Sender/Mutex), but carries
        // the result type T statically (so `join` needs no binding-context
        // inference and can't read the slot at the wrong width) and is NON-Copy
        // (a plain struct, not in isCopyType): `join` consumes it by value, so
        // double-joining the same handle (which would double-release the task
        // slot) is a compile error. Like the old i64 handle, a JoinHandle that
        // is dropped WITHOUT being joined leaks its task (no Drop glue — joining
        // is the explicit reclaim); a recursive-drop story arrives in Phase 173.
        TypePtr joinHandleVar = makeFreshVar();
        TypePtr joinHandleInst = makeStruct("JoinHandle", {});
        joinHandleInst->typeArgs = {joinHandleVar};
        {
            StructSchema sch;
            sch.type = joinHandleInst;
            sch.genericVars.push_back(joinHandleVar);
            structSchemas_["JoinHandle"] = std::move(sch);
        }
        // Helper: a fresh `JoinHandle<X>` instance for a given element var.
        auto mkJoinHandle = [&](const TypePtr& elem) -> TypePtr {
            TypePtr h = makeStruct("JoinHandle", {});
            h->typeArgs = {elem};
            return h;
        };
        // Phase 18 / v32 Phase 172 built-in, generic:
        //   `spawn<T>(f: Future<T>) -> JoinHandle<T>`.
        // Registers `f` as a concurrent task with the process-global executor
        // and returns a type-safe handle. Synchronous (no `async` effect): it
        // only enqueues; the task makes progress when the executor is driven
        // (by `block_on`/`join`). Codegen specializes per T so the handle's
        // result slot is sized for T (read back by `join<T>`).
        {
            TypePtr spawnVar = makeFreshVar();
            FnSchema sch;
            sch.signature =
                makeFunction({makeFuture(spawnVar)}, mkJoinHandle(spawnVar));
            sch.genericVars.push_back(spawnVar);
            fnSchemas_["spawn"] = std::move(sch);
        }
        // Phase 18 / v32 Phase 172 built-in, generic:
        //   `join<T>(h: JoinHandle<T>) -> T`.
        // Drives the executor until the task named by `h` completes, then yields
        // its result and releases the task. Synchronous (it runs the executor,
        // not suspends). T comes from the handle's type (no binding-context
        // guess); the handle is CONSUMED (move-only) so it can't be re-joined.
        {
            TypePtr joinVar = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction({mkJoinHandle(joinVar)}, joinVar);
            sch.genericVars.push_back(joinVar);
            fnSchemas_["join"] = std::move(sch);
        }
        // v32 Phase 173 built-in, generic:
        //   `task_cancel<T>(h: JoinHandle<T>)` — cancel a spawned task.
        // Consumes the move-only handle (so the task can't then be join()'d),
        // marks the executor task done, and releases its frame+slot (shallow —
        // a still-suspended task leaks its nested sub-frame; recursive cancel is
        // future work). Synchronous + effect-free like spawn/join (the async
        // executor is single-threaded — no thread boundary). Returns unit.
        {
            TypePtr cancelVar = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction({mkJoinHandle(cancelVar)}, makeUnit());
            sch.genericVars.push_back(cancelVar);
            fnSchemas_["task_cancel"] = std::move(sch);
        }

        // v32 Phase 172 built-in, generic: a Future COMBINATOR
        //   `map<T, U>(f: Future<T>, g: fn(T) -> U ! {e}) -> Future<U>`.
        // Builds a NEW future that, when polled, drives the inner future `f`
        // to `Ready(x)`, then applies `g(x)` exactly once and becomes
        // `Ready(g(x))`. `map` itself is synchronous (it only allocates the
        // combinator frame); the effect-row var `e` carries the continuation
        // `g`'s effects to map's call site. This is a conservative attribution:
        // composing an effectful continuation is treated as performing it (the
        // effect actually fires later, when the future is polled by
        // `block_on`/`.await`). Like `thread_spawn`, the closure is stored in a
        // heap frame and called AFTER `map` returns, so it must be `Fn` — a
        // by-reference (FnMut) capture would dangle by poll time (enforced in
        // checkCall). Codegen synthesizes a per-(T,U) leaf future (getOrEmitMap),
        // mirroring `sleep_ms`/`spawn`; there is no AST body.
        {
            TypePtr mapT = makeFreshVar();
            TypePtr mapU = makeFreshVar();
            TypePtr mapRow = makeFreshVar();
            TypePtr mapFn =
                makeFunction({mapT}, mapU, /*effectLabels=*/{}, mapRow);
            FnSchema sch;
            sch.signature =
                makeFunction({makeFuture(mapT), mapFn}, makeFuture(mapU));
            sch.genericVars.push_back(mapT);
            sch.genericVars.push_back(mapU);
            sch.declaredEffects.add("e"); // row-var name (flows g's effects)
            sch.effectRowVars.emplace_back("e", mapRow);
            fnSchemas_["future_map"] = std::move(sch);
        }

        // v32 Phase 172 built-in, generic: the monadic Future combinator
        //   `and_then<T, U>(f: Future<T>, g: fn(T) -> Future<U> ! {e})
        //      -> Future<U>`.
        // Like `map`, but the continuation `g` returns ANOTHER future, which
        // `and_then` then drives to completion (futures' `flatMap` / monadic
        // bind — the building block for sequencing async steps). Same effect
        // attribution as `map` (the row var `e` carries `g`'s effects to the
        // call site) and the same `Fn`-closure rule (the continuation is stored
        // and called at poll time, after `and_then` returns). Codegen
        // synthesizes a two-state leaf future (getOrEmitAndThen).
        {
            TypePtr atT = makeFreshVar();
            TypePtr atU = makeFreshVar();
            TypePtr atRow = makeFreshVar();
            TypePtr atFn =
                makeFunction({atT}, makeFuture(atU), /*effectLabels=*/{}, atRow);
            FnSchema sch;
            sch.signature =
                makeFunction({makeFuture(atT), atFn}, makeFuture(atU));
            sch.genericVars.push_back(atT);
            sch.genericVars.push_back(atU);
            sch.declaredEffects.add("e"); // row-var name (flows g's effects)
            sch.effectRowVars.emplace_back("e", atRow);
            fnSchemas_["future_and_then"] = std::move(sch);
        }

        // v32 Phase 172 built-in, generic: the structured "wait for all" Future
        // combinator `join2<A, B>(fa: Future<A>, fb: Future<B>)
        //   -> Future<(A, B)>`. Runs both futures concurrently and completes
        // with both results as a tuple (vs `select`'s "wait for any"). Pure (it
        // only allocates the combinator frame — no continuation, so no effect
        // row). Codegen synthesizes a per-(A,B) leaf future (getOrEmitJoin2)
        // that latches each sub-future's value as it completes.
        {
            TypePtr jA = makeFreshVar();
            TypePtr jB = makeFreshVar();
            FnSchema sch;
            sch.signature = makeFunction(
                {makeFuture(jA), makeFuture(jB)},
                makeFuture(makeTuple({jA, jB})));
            sch.genericVars.push_back(jA);
            sch.genericVars.push_back(jB);
            fnSchemas_["future_join2"] = std::move(sch);
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
            // Phase 75 (v13): `share` — the cross-thread / concurrency effect.
            // Spawning a thread crosses a thread boundary, so the spawning fn
            // must declare `! { share }`. Because `share` is a built-in effect,
            // it rides the existing effect-SUBSET rule: a trait method declared
            // pure (or without `share`) can NEVER transitively spawn through a
            // `<T: Trait>` / `&dyn Trait` dispatch — the checker attributes the
            // trait's declared effects and rejects a super-effecting impl. So
            // thread-safety becomes a CHECKED property, not a convention. (The
            // value-safety half — that only `Send` data crosses — is enforced
            // structurally at chan_send, Phase 77.)
            sch.declaredEffects.add("share");
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
        // v31 Phase 170: scoped threads. `Scope` is a phantom move-only i64
        // handle (NON-Copy, NON-Send) to a heap list of spawned thread handles;
        // its Drop joins them all (RAII join-before-scope-end). scope_new()
        // creates one; scope_spawn(&s, f) spawns f into the scope (so the
        // scope's end is guaranteed to outlive — and join — the worker).
        {
            StructSchema sch;
            sch.type = makeStruct("Scope", {}); // non-generic
            structSchemas_["Scope"] = std::move(sch);
        }
        //   scope_new() -> Scope ! { alloc }
        {
            FnSchema sch;
            sch.signature = makeFunction({}, makeStruct("Scope", {}));
            sch.declaredEffects.add("alloc");
            fnSchemas_["scope_new"] = std::move(sch);
        }
        //   scope_spawn(s: &Scope, f: fn() -> i64 ! {e}) -> i64 ! {io,share,e}
        // Mirrors thread_spawn's effect-polymorphic closure, with the closure's
        // effects flowing through the row var.
        {
            TypePtr rowVar = makeFreshVar();
            TypePtr fnParam = makeFunction({}, makeInt(),
                                           /*effectLabels=*/{}, rowVar);
            TypePtr scopeRef =
                makeRef(makeStruct("Scope", {}), /*isMut=*/false);
            FnSchema sch;
            sch.signature = makeFunction({scopeRef, fnParam}, makeInt());
            sch.declaredEffects.add("io");
            sch.declaredEffects.add("share");
            sch.declaredEffects.add("e");
            sch.effectRowVars.emplace_back("e", rowVar);
            fnSchemas_["scope_spawn"] = std::move(sch);
        }
        // Phase 19 / 123 built-in: `Mutex<T>` — a lock guarding a cell of type
        // T. It is a PHANTOM-TYPED i64 HANDLE: the value is a bare i64 (PtrToInt
        // of a heap `{ pthread_mutex_t, T value }` block) so it stays Copy and
        // can be captured BY VALUE into each thread's closure (giving every
        // thread the same lock + cell — the cross-thread sharing mechanism), but
        // the type carries T so `mutex_get`/`mutex_set` are tied to the cell
        // type. v21 Phase 123 lifts the cell from i64-only to an arbitrary T.
        // Because the handle is shareable across threads, the cell T must be
        // `Send` (enforced at the mutex_new call site, like chan_send) — so a
        // non-Send value (`Rc`, a `Receiver`, a borrow) can't be smuggled across
        // a thread boundary through the cell.
        TypePtr mutexVar = makeFreshVar();
        TypePtr mutexInst = makeStruct("Mutex", {});
        mutexInst->typeArgs = {mutexVar};
        {
            StructSchema sch;
            sch.type = mutexInst;
            sch.genericVars.push_back(mutexVar);
            structSchemas_["Mutex"] = std::move(sch);
        }
        // A fresh `Mutex<T>` instance over a fresh cell var — one per signature.
        auto mkMutex = [&](const TypePtr& cell) {
            TypePtr m = makeStruct("Mutex", {});
            m->typeArgs = {cell};
            return m;
        };
        //   mutex_new<T>(v: T) -> Mutex<T> ! { alloc }   (allocates the block)
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({c}, mkMutex(c));
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("alloc");
            fnSchemas_["mutex_new"] = std::move(sch);
        }
        //   mutex_lock<T>(m: Mutex<T>) -> i64 ! { io }
        //   mutex_unlock<T>(m: Mutex<T>) -> i64 ! { io }
        // Acquire / release the lock; `io`-effecting (cross-thread sync is an
        // observable side effect). T-agnostic in codegen (they only touch the
        // pthread_mutex_t at field 0). Return 0 (an i64 so they compose).
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkMutex(c)}, makeInt());
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_lock"] = std::move(sch);
        }
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkMutex(c)}, makeInt());
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_unlock"] = std::move(sch);
        }
        //   mutex_get<T>(m: Mutex<T>) -> T           (read the guarded cell)
        //   mutex_set<T>(m: Mutex<T>, v: T) -> i64   (write the guarded cell)
        // The caller is expected to hold the lock. `io`-effecting for the write
        // (a shared-memory mutation other threads observe); the read is left
        // pure so reading under a lock needs no extra annotation. T flows from
        // the `Mutex<T>` handle, so a `mutex_get` on a `Mutex<Point>` is a
        // `Point` (no annotation needed, and no wrong-T punning).
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkMutex(c)}, c);
            sch.genericVars.push_back(c);
            fnSchemas_["mutex_get"] = std::move(sch);
        }
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkMutex(c), c}, makeInt());
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_set"] = std::move(sch);
        }

        // === v31 Phase 168: RwLock<T> + RAII lock guards ===
        // `RwLock<T>` is a reader/writer lock — a Copy shareable i64 handle like
        // Mutex (its heap block is `{ pthread_rwlock_t, T }`); Send iff T is.
        // The three guard types (`MutexGuard<T>`, `RwLockReadGuard<T>`,
        // `RwLockWriteGuard<T>`) are MOVE-ONLY RAII tokens whose Drop releases
        // the held lock (the scoped-lock pattern, like C++ lock_guard /
        // shared_lock / unique_lock). They carry T for type clarity; data is
        // read/written through the lock's get/set while the guard holds it.
        auto registerBuiltinGeneric = [&](const char* name) {
            TypePtr v = makeFreshVar();
            TypePtr inst = makeStruct(name, {});
            inst->typeArgs = {v};
            StructSchema sch;
            sch.type = inst;
            sch.genericVars.push_back(v);
            structSchemas_[name] = std::move(sch);
        };
        registerBuiltinGeneric("RwLock");
        registerBuiltinGeneric("MutexGuard");
        registerBuiltinGeneric("RwLockReadGuard");
        registerBuiltinGeneric("RwLockWriteGuard");
        auto mkInst = [&](const char* name, const TypePtr& cell) {
            TypePtr t = makeStruct(name, {});
            t->typeArgs = {cell};
            return t;
        };
        //   mutex_guard<T>(m: Mutex<T>) -> MutexGuard<T> ! { io }  (locks)
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature =
                makeFunction({mkMutex(c)}, mkInst("MutexGuard", c));
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["mutex_guard"] = std::move(sch);
        }
        //   rwlock_new<T>(v: T) -> RwLock<T> ! { alloc }
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({c}, mkInst("RwLock", c));
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("alloc");
            fnSchemas_["rwlock_new"] = std::move(sch);
        }
        //   rwlock_read<T>(rw: RwLock<T>) -> i64 ! { io }   (acquire read lock)
        //   rwlock_write<T>(rw: RwLock<T>) -> i64 ! { io }  (acquire write lock)
        //   rwlock_unlock<T>(rw: RwLock<T>) -> i64 ! { io }
        for (const char* op : {"rwlock_read", "rwlock_write", "rwlock_unlock"}) {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkInst("RwLock", c)}, makeInt());
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_[op] = std::move(sch);
        }
        //   rwlock_get<T>(rw: RwLock<T>) -> T          (read under a read/wr lock)
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkInst("RwLock", c)}, c);
            sch.genericVars.push_back(c);
            fnSchemas_["rwlock_get"] = std::move(sch);
        }
        //   rwlock_set<T>(rw: RwLock<T>, v: T) -> i64  (write under a write lock)
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkInst("RwLock", c), c}, makeInt());
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["rwlock_set"] = std::move(sch);
        }
        //   rwlock_read_guard<T>(rw: RwLock<T>) -> RwLockReadGuard<T> ! { io }
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature =
                makeFunction({mkInst("RwLock", c)}, mkInst("RwLockReadGuard", c));
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["rwlock_read_guard"] = std::move(sch);
        }
        //   rwlock_write_guard<T>(rw: RwLock<T>) -> RwLockWriteGuard<T> ! { io }
        {
            FnSchema sch;
            TypePtr c = makeFreshVar();
            sch.signature = makeFunction({mkInst("RwLock", c)},
                                         mkInst("RwLockWriteGuard", c));
            sch.genericVars.push_back(c);
            sch.declaredEffects.add("io");
            fnSchemas_["rwlock_write_guard"] = std::move(sch);
        }

        // === v31 Phase 169: atomics + CAS + memory orderings ===
        // `AtomicI64` / `AtomicBool` are NON-generic phantom builtin structs —
        // Copy i64 handles over a naturally-aligned heap cell (process-lifetime,
        // like Mutex), and Send+Sync (the legible POSITIVE Send witness, the
        // mirror of Rc's negative one). The ops are NAME-SUFFIXED by memory
        // ordering (atomic_i64_fetch_add_seqcst, …) so the LLVM AtomicOrdering
        // is a COMPILE-TIME constant — there is no turbofish and an enum arg is
        // a runtime value. The ergonomic `enum Ordering` + `impl AtomicI64`
        // surface (prelude) matches the ordering and dispatches to these.
        for (const char* an : {"AtomicI64", "AtomicBool"}) {
            StructSchema sch;
            sch.type = makeStruct(an, {}); // non-generic — no genericVars
            structSchemas_[an] = std::move(sch);
        }
        auto atomicTy = [&](const char* n) { return makeStruct(n, {}); };
        auto regAtomic = [&](const std::string& nm, std::vector<TypePtr> params,
                             TypePtr ret, bool io, bool alloc) {
            FnSchema sch;
            sch.signature = makeFunction(std::move(params), std::move(ret));
            if (io) sch.declaredEffects.add("io");
            if (alloc) sch.declaredEffects.add("alloc");
            fnSchemas_[nm] = std::move(sch);
        };
        // constructors (allocate the cell)
        regAtomic("atomic_i64_new", {makeInt()}, atomicTy("AtomicI64"), false,
                  true);
        regAtomic("atomic_bool_new", {makeBool()}, atomicTy("AtomicBool"),
                  false, true);
        // AtomicI64: load (pure) / store / swap / fetch_* / cmpxchg.
        for (const char* o : {"relaxed", "acquire", "seqcst"})
            regAtomic(std::string("atomic_i64_load_") + o,
                      {atomicTy("AtomicI64")}, makeInt(), false, false);
        for (const char* o : {"relaxed", "release", "seqcst"})
            regAtomic(std::string("atomic_i64_store_") + o,
                      {atomicTy("AtomicI64"), makeInt()}, makeInt(), true,
                      false);
        for (const char* op : {"swap", "fetch_add", "fetch_sub", "fetch_and",
                               "fetch_or", "fetch_xor"})
            for (const char* o : {"relaxed", "acqrel", "seqcst"})
                regAtomic(std::string("atomic_i64_") + op + "_" + o,
                          {atomicTy("AtomicI64"), makeInt()}, makeInt(), true,
                          false);
        for (const char* o : {"relaxed", "acqrel", "seqcst"})
            regAtomic(std::string("atomic_i64_cmpxchg_") + o,
                      {atomicTy("AtomicI64"), makeInt(), makeInt()}, makeBool(),
                      true, false);
        // AtomicBool: load (pure) / store / swap / cmpxchg (no arithmetic).
        for (const char* o : {"relaxed", "acquire", "seqcst"})
            regAtomic(std::string("atomic_bool_load_") + o,
                      {atomicTy("AtomicBool")}, makeBool(), false, false);
        for (const char* o : {"relaxed", "release", "seqcst"})
            regAtomic(std::string("atomic_bool_store_") + o,
                      {atomicTy("AtomicBool"), makeBool()}, makeInt(), true,
                      false);
        for (const char* o : {"relaxed", "acqrel", "seqcst"})
            regAtomic(std::string("atomic_bool_swap_") + o,
                      {atomicTy("AtomicBool"), makeBool()}, makeBool(), true,
                      false);
        for (const char* o : {"relaxed", "acqrel", "seqcst"})
            regAtomic(std::string("atomic_bool_cmpxchg_") + o,
                      {atomicTy("AtomicBool"), makeBool(), makeBool()},
                      makeBool(), true, false);
        // standalone fences
        for (const char* o : {"acquire", "release", "acqrel", "seqcst"})
            regAtomic(std::string("fence_") + o, {}, makeInt(), true, false);

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

        // v26 Phase 144: register top-level type aliases BEFORE anything that
        // resolves a TypeRef (struct fields below), so an alias name resolves to
        // its target everywhere it appears.
        for (const auto& [name, target] : program.typeAliases)
            typeAliases_[name] = target;

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
        // v28 Phase 154: struct field types and enum payloads can reference one
        // another (`struct H { m: Maybe<i64> }` + `enum Maybe<T> { Yes(T), No }`).
        // Whichever is resolved first sees the other only as a SHELL (no fields /
        // no variants), and the resolved field/payload type embeds that empty
        // shell — so e.g. a generic-enum struct field used to mismatch the value.
        // Fix: resolve struct fields, then enum payloads, then RE-resolve struct
        // fields (a second round) now that the enum schemas are complete. The
        // `reResolve` flag bypasses the already-resolved guard. (enum->struct is
        // covered by round 1's struct pass running before enums; struct->enum by
        // the second struct round. One re-round suffices for the common single
        // level of mutual reference.)
        auto resolveAllStructFields = [&](bool reResolve) {
            for (const auto& sd : program.structs) {
                auto it = structSchemas_.find(sd.name);
                if (it == structSchemas_.end()) continue; // duplicate
                if (!reResolve && !it->second.type->structFields.empty())
                    continue;
                GenericEnv genEnv = buildGenericEnv(sd.genericParams,
                                                      it->second.genericVars);
                currentGenericEnv_ = &genEnv;
                setConstParamsInScope(sd.genericParams); // Phase 57
                std::vector<std::pair<std::string, TypePtr>> resolvedFields;
                resolvedFields.reserve(sd.fields.size());
                std::unordered_set<std::string> seen;
                for (const auto& f : sd.fields) {
                    if (!seen.insert(f.name).second) {
                        if (!reResolve)
                            error("duplicate field '" + f.name +
                                      "' in struct '" + sd.name + "'",
                                  sd.line, sd.column);
                        continue;
                    }
                    resolvedFields.emplace_back(f.name, resolveTypeRef(f.type));
                }
                it->second.type->structFields = std::move(resolvedFields);
                currentGenericEnv_ = nullptr;
                currentConstParams_.clear();
            }
        };
        resolveAllStructFields(/*reResolve=*/false);

        for (const auto& ed : program.enums) {
            auto it = enumSchemas_.find(ed.name);
            if (it == enumSchemas_.end()) continue; // duplicate
            if (!it->second.type->enumVariants.empty()) continue;
            GenericEnv genEnv = buildGenericEnv(ed.genericParams,
                                                  it->second.genericVars);
            currentGenericEnv_ = &genEnv;
            setConstParamsInScope(ed.genericParams); // review fix: const enums
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
            currentConstParams_.clear(); // review fix: don't leak const enums
        }
        // v28 Phase 154: second round — re-resolve struct fields now that every
        // enum schema carries its full (substituted) variants, so a generic-enum
        // struct field (`H { m: Maybe<i64> }`) materializes Yes(i64)/No and
        // unifies with the constructed value.
        resolveAllStructFields(/*reResolve=*/true);

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

            // Phase 122 (v21): hashmap_remove<K,V>(m: &mut HashMap<K,V>, k: K)
            // -> Option<V> — Some(v) if the key was present (removed, value
            // moved out), None otherwise. `&mut` (it mutates the table) and a
            // fresh Option<V> instance over the SAME V Var (pinned at the call
            // site, like hashmap_get).
            {
                TypePtr optV2;
                if (!optSchema.genericVars.empty()) {
                    std::unordered_map<int, TypePtr> subst;
                    subst[optSchema.genericVars[0]->varId] = hmValVar;
                    optV2 = instantiate(optSchema.type, subst);
                    optV2->typeArgs = {hmValVar};
                } else {
                    optV2 = optSchema.type;
                }
                FnSchema rmsch;
                rmsch.signature = makeFunction(
                    {makeRef(hashMapInst, /*isMut=*/true), hmKeyVar}, optV2);
                rmsch.genericVars.push_back(hmKeyVar);
                rmsch.genericVars.push_back(hmValVar);
                fnSchemas_["hashmap_remove"] = std::move(rmsch);
            }

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

            // Phase 76/77 (v13): the typed MPSC channel API, GENERIC over the
            // message type T (a channel MOVES a real T across threads). A
            // channel splits into a Copy Sender (multi-producer) and a single
            // Receiver. The ops carry `share` (Phase 75) — channels are the
            // cross-thread communication path, so a pure-declared interface
            // can't smuggle one in. recv returns Option (None once the channel
            // is closed AND drained). isSend(T) is enforced at the chan_send
            // call site (Phase 77).
            {
                TypePtr chanVar = makeFreshVar();
                TypePtr senderT = makeStruct("Sender", {});
                senderT->typeArgs = {chanVar};
                TypePtr receiverT = makeStruct("Receiver", {});
                receiverT->typeArgs = {chanVar};
                TypePtr optT;
                if (!optSchema.genericVars.empty()) {
                    std::unordered_map<int, TypePtr> subst;
                    subst[optSchema.genericVars[0]->varId] = chanVar;
                    optT = instantiate(optSchema.type, subst);
                    optT->typeArgs = {chanVar};
                } else {
                    optT = optSchema.type;
                }
                // channel<T>() -> (Sender<T>, Receiver<T>) ! { alloc }
                {
                    TypePtr pair = std::make_shared<Type>();
                    pair->kind = TypeKind::Tuple;
                    pair->tupleElems = {senderT, receiverT};
                    FnSchema sch;
                    sch.signature = makeFunction({}, pair);
                    sch.declaredEffects.add("alloc");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["channel"] = std::move(sch);
                }
                // Phase 81 (v13 review fix): the endpoints are refcounted,
                // move-only OWNERS now (not Copy handles), so chan_send / recv /
                // try_recv BORROW the endpoint (`&Sender` / `&Receiver`) — a
                // Sender can be sent on in a loop, and a Receiver recv'd in a
                // loop, without being moved. Multi-producer uses sender_clone;
                // an endpoint's drop decrements the refcount (close on the last
                // sender, drain + free on the last endpoint).
                // chan_send<T>(s: &Sender<T>, v: T) -> i64 ! { share }
                {
                    FnSchema sch;
                    sch.signature = makeFunction(
                        {makeRef(senderT, /*isMut=*/false), chanVar}, makeInt());
                    sch.declaredEffects.add("share");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["chan_send"] = std::move(sch);
                }
                // chan_recv<T>(r: &Receiver<T>) -> Option<T> ! { share }
                {
                    FnSchema sch;
                    sch.signature = makeFunction(
                        {makeRef(receiverT, /*isMut=*/false)}, optT);
                    sch.declaredEffects.add("share");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["chan_recv"] = std::move(sch);
                }
                // sender_clone<T>(s: &Sender<T>) -> Sender<T> ! { share } — the
                // multi-producer primitive: a second owning Sender on the same
                // channel (bumps the live-sender + endpoint counts).
                {
                    FnSchema sch;
                    sch.signature = makeFunction(
                        {makeRef(senderT, /*isMut=*/false)}, senderT);
                    sch.declaredEffects.add("share");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["sender_clone"] = std::move(sch);
                }
                // Phase 79: chan_try_recv<T>(r) -> Option<T> ! { share } — a
                // NON-BLOCKING recv: Some if an item is ready, None if the queue
                // is momentarily empty (does NOT block on the condvar). For
                // poll loops / draining the tail of a fork-join gather. Needs a
                // FRESH Option<T> (instantiating reuses a Var-bearing schema).
                {
                    TypePtr optT2;
                    if (!optSchema.genericVars.empty()) {
                        std::unordered_map<int, TypePtr> subst;
                        subst[optSchema.genericVars[0]->varId] = chanVar;
                        optT2 = instantiate(optSchema.type, subst);
                        optT2->typeArgs = {chanVar};
                    } else {
                        optT2 = optSchema.type;
                    }
                    FnSchema sch;
                    sch.signature = makeFunction(
                        {makeRef(receiverT, /*isMut=*/false)}, optT2);
                    sch.declaredEffects.add("share");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["chan_try_recv"] = std::move(sch);
                }
                // v31 Phase 170: select2/3/4<T>(r0,..,rN-1: &Receiver<T>) ->
                // SelectResult<T> ! { share } — block (poll-with-backoff) until
                // one of N HOMOGENEOUS receivers is ready, returning
                // Ready(idx, value) or Closed(idx). All receiver params share
                // chanVar, so T is homogeneous by unification. A distinct
                // fnSchema per arity (no variadic builtin path).
                if (enumSchemas_.count("SelectResult")) {
                    const EnumSchema& srSchema = enumSchemas_["SelectResult"];
                    auto mkSelectResult = [&]() -> TypePtr {
                        if (srSchema.genericVars.empty()) return srSchema.type;
                        std::unordered_map<int, TypePtr> subst;
                        subst[srSchema.genericVars[0]->varId] = chanVar;
                        TypePtr t = instantiate(srSchema.type, subst);
                        t->typeArgs = {chanVar};
                        return t;
                    };
                    for (int n : {2, 3, 4}) {
                        std::vector<TypePtr> params;
                        for (int i = 0; i < n; ++i)
                            params.push_back(
                                makeRef(receiverT, /*isMut=*/false));
                        FnSchema sch;
                        sch.signature =
                            makeFunction(std::move(params), mkSelectResult());
                        sch.declaredEffects.add("share");
                        sch.genericVars.push_back(chanVar);
                        fnSchemas_["select" + std::to_string(n)] =
                            std::move(sch);
                    }
                }
                // chan_close<T>(s: Sender<T>) -> i64 ! { share } — takes the
                // Sender BY VALUE (consumes it): the explicit "this producer is
                // done" that decrements the live-sender count (closing only when
                // it is the LAST sender), so one producer closing can no longer
                // abandon another's queued items.
                {
                    FnSchema sch;
                    sch.signature = makeFunction({senderT}, makeInt());
                    sch.declaredEffects.add("share");
                    sch.genericVars.push_back(chanVar);
                    fnSchemas_["chan_close"] = std::move(sch);
                }
            }
        }

        // v31 Phase 171: weak_upgrade<T>(w: &Weak<T>) -> Option<Arc<T>>.
        // Registered here (after the enum loop) because Option is not yet in
        // enumSchemas_ at the Arc/Weak registration site above.
        if (structSchemas_.count("Arc") && enumSchemas_.count("Option")) {
            TypePtr wuVar = makeFreshVar();
            TypePtr aInst = makeStruct("Arc", {});
            aInst->typeArgs = {wuVar};
            TypePtr wInst = makeStruct("Weak", {});
            wInst->typeArgs = {wuVar};
            const EnumSchema& optSchema = enumSchemas_["Option"];
            TypePtr optArc;
            if (!optSchema.genericVars.empty()) {
                std::unordered_map<int, TypePtr> subst;
                subst[optSchema.genericVars[0]->varId] = aInst;
                optArc = instantiate(optSchema.type, subst);
                optArc->typeArgs = {aInst};
            } else {
                optArc = optSchema.type;
            }
            FnSchema sch;
            sch.signature = makeFunction({makeRef(wInst, false)}, optArc);
            sch.declaredEffects.add("alloc");
            sch.genericVars.push_back(wuVar);
            fnSchemas_["weak_upgrade"] = std::move(sch);
        }

        // v32 Phase 172: the "wait for any" Future combinator
        //   `select<A, B>(fa: Future<A>, fb: Future<B>)
        //      -> Future<Either<A, B>>`.
        // Completes as soon as EITHER future is ready (`Left(a)` / `Right(b)`)
        // and drops the loser. Registered here (after the enum loop) because
        // the prelude enum `Either` is not yet in `enumSchemas_` at the early
        // built-in registration site (same ordering constraint as
        // `weak_upgrade`/`select2`). Pure (no continuation). Codegen
        // synthesizes a per-(A,B) leaf future (getOrEmitSelect).
        // Defensive (review #3): codegen's getOrEmitSelect hard-codes Either's
        // shape — exactly 2 type params and variants `Left`/`Right`, whose
        // payload slots it indexes by name. Only expose future_select if the
        // in-scope `Either` actually has that shape; if a user shadowed it with
        // a different-shaped `enum Either`, skip registration (future_select is
        // then simply an unknown function — a clear error — rather than
        // mis-indexing a payload at codegen time).
        if (enumSchemas_.count("Either")) {
            const EnumSchema& eitherSchema = enumSchemas_["Either"];
            bool shapeOk = eitherSchema.genericVars.size() == 2 &&
                           eitherSchema.type->enumVariants.size() == 2;
            if (shapeOk) {
                bool hasLeft = false, hasRight = false;
                for (const auto& v : eitherSchema.type->enumVariants) {
                    if (v.name == "Left") hasLeft = true;
                    else if (v.name == "Right") hasRight = true;
                }
                shapeOk = hasLeft && hasRight;
            }
            if (shapeOk) {
                TypePtr selA = makeFreshVar();
                TypePtr selB = makeFreshVar();
                std::unordered_map<int, TypePtr> subst;
                subst[eitherSchema.genericVars[0]->varId] = selA;
                subst[eitherSchema.genericVars[1]->varId] = selB;
                TypePtr eitherInst = instantiate(eitherSchema.type, subst);
                eitherInst->typeArgs = {selA, selB};
                FnSchema sch;
                sch.signature = makeFunction(
                    {makeFuture(selA), makeFuture(selB)},
                    makeFuture(eitherInst));
                sch.genericVars.push_back(selA);
                sch.genericVars.push_back(selB);
                fnSchemas_["future_select"] = std::move(sch);
            }
        }

        // v32 Phase 173: `timeout<T>(fut: Future<T>, ms: i64) -> Future<Option<T>>`
        //   — race `fut` against a `sleep_ms(ms)` timer (built internally by
        // codegen), completing `Some(v)` if fut wins or `None` on timeout. Pure
        // construction (no async effect on `timeout` itself; the wait happens
        // when the resulting future is polled). Registered after the enum loop
        // because it returns the prelude `Option`. Codegen: getOrEmitTimeout.
        if (enumSchemas_.count("Option")) {
            const EnumSchema& optSchema = enumSchemas_["Option"];
            TypePtr toVar = makeFreshVar();
            TypePtr optInst;
            if (!optSchema.genericVars.empty()) {
                std::unordered_map<int, TypePtr> subst;
                subst[optSchema.genericVars[0]->varId] = toVar;
                optInst = instantiate(optSchema.type, subst);
                optInst->typeArgs = {toVar};
            } else {
                optInst = optSchema.type;
            }
            FnSchema sch;
            sch.signature = makeFunction(
                {makeFuture(toVar), makeInt()}, makeFuture(optInst));
            sch.genericVars.push_back(toVar);
            fnSchemas_["timeout"] = std::move(sch);
        }

        // v33 Phase 181: overflow-checked + wrapping integer arithmetic. The
        // default arithmetic policy is 2's-complement WRAP (kardc compiles AOT
        // under `-fwrapv` and the JIT matches), documented in ROADMAP/CHANGELOG.
        // These give explicit control: `checked_<op>(a, b) -> Option<i64>` is
        // `None` on signed overflow (or div-by-zero / INT_MIN/-1), `Some(r)`
        // otherwise; `wrapping_<op>(a, b) -> i64` is the explicit wrapping op.
        // checked_* need the prelude `Option`, so register after the enum loop.
        if (enumSchemas_.count("Option")) {
            const EnumSchema& optSchema = enumSchemas_["Option"];
            TypePtr optI64;
            if (!optSchema.genericVars.empty()) {
                std::unordered_map<int, TypePtr> subst;
                subst[optSchema.genericVars[0]->varId] = makeInt();
                optI64 = instantiate(optSchema.type, subst);
                optI64->typeArgs = {makeInt()};
            } else {
                optI64 = optSchema.type;
            }
            for (const char* nm :
                 {"checked_add", "checked_sub", "checked_mul", "checked_div"}) {
                FnSchema sch;
                sch.signature = makeFunction({makeInt(), makeInt()}, optI64);
                fnSchemas_[nm] = std::move(sch);
            }
            for (const char* nm :
                 {"wrapping_add", "wrapping_sub", "wrapping_mul"}) {
                FnSchema sch;
                sch.signature = makeFunction({makeInt(), makeInt()}, makeInt());
                fnSchemas_[nm] = std::move(sch);
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
                // v28 Phase 155 (GATs): record the associated type's arity (the
                // number of generic params it declares), so a projection
                // `Self::Out<args>` can validate it supplies the right count.
                traitGatArity_[td.name][at.name] = at.typeParams.size();
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
                // Phase 48: a method whose first param is NOT `self` is a
                // STATIC (associated) method — e.g. `fn default() -> Self`.
                // Allowed; it is called as `Type::method(args)` (no receiver),
                // resolved against the implementing type, not a value. Only a
                // method that DOES start with a param literally named `self`
                // must spell its type `Self` (the receiver convention).
                bool hasSelf =
                    !m.params.empty() && m.params[0].name == "self";
                if (hasSelf && m.params[0].type.name != "Self") {
                    error("trait method '" + m.name + "' receiver must be "
                          "`self` of type `Self`",
                          m.line, m.column);
                }
                uniqueMethods.push_back(m);
            }
            // v31 Phase 167: Send/Sync are pure MARKER traits — no methods, no
            // vtable, no runtime cost. Reject a user adding behaviour to them.
            if ((td.name == "Send" || td.name == "Sync") &&
                !uniqueMethods.empty()) {
                error("the marker trait `" + td.name +
                          "` must have no methods",
                      td.line, td.column);
            }
            traits_[td.name] = std::move(uniqueMethods);
            traitSupertraits_[td.name] = td.supertraits; // v25 Phase 136
        }

        // v25 Phase 136: a type that impls a trait must also impl each of the
        // trait's SUPERTRAITS. Compare on the impl's spelled type name (which is
        // consistent across both the impl set and the supertrait lookup) — the
        // derive- and default-expansion passes have already populated impls.
        {
            std::unordered_set<std::string> implemented;
            for (const auto& impl : program.impls)
                // v31 Phase 167: a negative marker impl does NOT implement the
                // trait — it opts out — so it must not satisfy a supertrait
                // bound (`trait Foo: Send` over a type with `impl !Send`).
                if (!impl.traitName.empty() && !impl.isNegative)
                    implemented.insert(impl.forType.name + "/" + impl.traitName);
            for (const auto& impl : program.impls) {
                if (impl.traitName.empty()) continue;
                auto it = traitSupertraits_.find(impl.traitName);
                if (it == traitSupertraits_.end()) continue;
                for (const auto& sup : it->second) {
                    if (!implemented.count(impl.forType.name + "/" + sup)) {
                        error("impl of '" + impl.traitName + "' for '" +
                                  impl.forType.name +
                                  "' requires an impl of its supertrait '" + sup +
                                  "' for '" + impl.forType.name + "'",
                              impl.line, impl.column);
                    }
                }
            }
        }

        // v25 Phase 138: coherence — reject two impls of the same trait for the
        // same type (explicit duplicates, or two overlapping blanket impls that
        // each synthesized a concrete `impl Tr for X`). The key spells the type
        // AND its type-args plus the trait + its trait-args, so distinct
        // instantiations (`Pair<i64>` vs `Pair<bool>`, `Iterator<i64>` vs
        // `Iterator<bool>`) are NOT treated as conflicting.
        {
            auto spellArgs = [](const std::vector<ast::TypeRef>& args) {
                std::string s;
                if (!args.empty()) {
                    s += "<";
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        if (i) s += ",";
                        s += args[i].name;
                    }
                    s += ">";
                }
                return s;
            };
            std::unordered_set<std::string> seenImplPairs;
            for (const auto& impl : program.impls) {
                if (impl.traitName.empty()) continue;
                // v31 Phase 167: marker (Send/Sync) impls — positive and
                // negative — are governed by the dedicated marker-oracle pass
                // below, which owns their conflict/duplicate diagnostics.
                if (impl.traitName == "Send" || impl.traitName == "Sync")
                    continue;
                std::string key = impl.forType.name +
                                  spellArgs(impl.forType.typeArgs) + "/" +
                                  impl.traitName + spellArgs(impl.traitTypeArgs);
                if (!seenImplPairs.insert(key).second) {
                    error("conflicting implementations of trait '" +
                              impl.traitName + "' for type '" +
                              impl.forType.name + "'",
                          impl.line, impl.column);
                }
            }
        }

        // v31 Phase 167: build the Send/Sync marker oracle. A positive `impl
        // Send for T {}` forces T: Send (the legible witness for an opaque /
        // handle type whose structural answer is wrong — e.g. a future Arc); a
        // negative `impl !Send for T {}` opts T out (the principled replacement
        // for hand-coded special-cases). This is the SOLE authority on marker
        // impls — they are skipped by the supertrait/coherence checks above so
        // their conflict diagnostics live here. A type with no marker impl is
        // unspecified and gets the structural auto-derive in isSend/isSync.
        for (const auto& impl : program.impls) {
            const bool isMarker =
                impl.traitName == "Send" || impl.traitName == "Sync";
            if (impl.isNegative && !isMarker) {
                error("negative impls are only allowed for the marker traits "
                      "`Send` and `Sync`",
                      impl.line, impl.column);
                continue;
            }
            if (!isMarker) continue;
            if (!impl.methods.empty()) {
                error("a marker impl of `" + impl.traitName +
                          "` must have no methods",
                      impl.line, impl.column);
                continue;
            }
            const std::string& tyName = impl.forType.name;
            int sign = impl.isNegative ? -1 : +1;
            auto& slot = markerImpls_[tyName];
            auto existing = slot.find(impl.traitName);
            if (existing != slot.end()) {
                if (existing->second != sign)
                    error("conflicting `impl " + impl.traitName +
                              "` and `impl !" + impl.traitName + "` for type '" +
                              tyName + "'",
                          impl.line, impl.column);
                else
                    error("duplicate impl of marker trait `" + impl.traitName +
                              "` for type '" + tyName + "'",
                          impl.line, impl.column);
                continue;
            }
            slot[impl.traitName] = sign;
        }

        // Pass 1d: register impl blocks. We resolve the implementing type
        // and validate that each impl method's signature matches the
        // trait's after substituting Self -> implementing type.
        // monomorphic-only in Phase 3.3 MVP: forType must resolve to a
        // concrete (no Vars) Struct or Enum.
        for (std::size_t implIdx = 0; implIdx < program.impls.size();
             ++implIdx) {
            const auto& impl = program.impls[implIdx];
            // v31 Phase 167: a negative marker impl (`impl !Send for T {}`)
            // provides nothing — it is an opt-out recorded in markerImpls_, not
            // a real impl. Skip it here so it is neither validated against a
            // trait signature nor registered as providing a method.
            if (impl.isNegative) continue;
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
            setConstParamsInScope(impl.genericParams); // Phase 61
            TypePtr forTy = resolveTypeRef(impl.forType);
            currentConstParams_.clear();
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
            else if (rfor->kind == TypeKind::Char) typeName = "char"; // v27 P149
            else if (rfor->kind == TypeKind::Box) typeName = "Box";    // Phase 51
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
                // Phase-4-fix: record this impl's concrete trait type-args
                // (structurally, by string; a generic Var arg becomes "*",
                // a wildcard) keyed by "type/trait", so a `dyn Trait<Args>`
                // coercion can verify the impl's PARAMETERIZATION matches — not
                // just the trait name. Rejects `Producer<i64>` -> `dyn
                // Producer<String>`.
                {
                    GenericEnv targEnv = implParamEnv(impl);
                    targEnv["Self"] = forTy;
                    const GenericEnv* savedTArg = currentGenericEnv_;
                    currentGenericEnv_ = &targEnv;
                    std::vector<std::string> argStrs;
                    for (const auto& ta : impl.traitTypeArgs) {
                        TypePtr rt = resolve(resolveTypeRef(ta));
                        argStrs.push_back(rt->kind == TypeKind::Var
                                              ? std::string("*")
                                              : typeToString(rt));
                    }
                    currentGenericEnv_ = savedTArg;
                    implTraitArgStrs_[typeName + "/" + impl.traitName] =
                        std::move(argStrs);
                }
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
                    // v28 Phase 155 (GATs): a parameterized binding `type Out<T>
                    // = Pair<T, T>;` can't be pre-resolved (the RHS has free
                    // params); store it raw for per-projection substitution. A
                    // plain (Phase 21b) binding pre-resolves as before. Counts as
                    // "provided" either way (resolvedAssoc[name] = a placeholder).
                    if (!at.typeParams.empty()) {
                        GatBinding gb;
                        gb.selfTy = forTy;
                        for (const auto& tp : at.typeParams)
                            gb.paramNames.push_back(tp.name);
                        gb.rhs = at.type;
                        implGatBindings_[typeName][impl.traitName][at.name] =
                            std::move(gb);
                        resolvedAssoc[at.name] = forTy; // sentinel: "provided"
                    } else {
                        resolvedAssoc[at.name] = resolveTypeRef(at.type);
                    }
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

        // v32 Phase 176: register user-declared effects BEFORE fn signatures,
        // so an effect name is a valid concrete effect-row label on a fn that
        // performs it (`fn work() ! { Logger }`). Op param/return types are
        // resolved lazily at perform/handle sites.
        for (const auto& ed : program.effects) {
            if (userEffectNames_.count(ed.name) || isBuiltinEffect(ed.name)) {
                error("effect redefined (or shadows a built-in effect): " +
                          ed.name,
                      ed.line, ed.column);
                continue;
            }
            userEffectNames_.insert(ed.name);
            for (const auto& op : ed.ops) {
                if (effectOpDecls_[ed.name].count(op.name)) {
                    error("effect operation redefined: " + ed.name +
                              "::" + op.name,
                          op.line, op.column);
                    continue;
                }
                effectOpDecls_[ed.name][op.name] = &op;
            }
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
            std::vector<std::string> genConstNames; // Phase 59, parallel to genVars
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
                // Phase 61: a const param is NOT a type Var in genEnv — it
                // resolves symbolically via currentConstParams_.
                if (!gp.isConst) genEnv[gp.name] = v;
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
                    genConstNames.push_back(gp.isConst ? gp.name
                                                       : std::string{});
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

            // Phase 59: const-generic params in scope so `[i64; N]` in the
            // signature is a SYMBOLIC length (not a const-eval failure).
            setConstParamsInScope(fn.genericParams);
            std::vector<TypePtr> argTypes;
            argTypes.reserve(fn.params.size());
            for (const auto& p : fn.params) {
                argTypes.push_back(resolveTypeRef(p.type));
            }
            TypePtr ret = resolveTypeRef(fn.returnType);
            currentConstParams_.clear();

            currentGenericEnv_ = nullptr;
            currentEffectRowVarNames_ = nullptr;
            currentVarBound_.clear();
            currentVarAllBounds_.clear();

            FnSchema schema;
            schema.signature = makeFunction(std::move(argTypes), ret);
            schema.genericVars = std::move(genVars);
            schema.constParamNames = std::move(genConstNames); // Phase 59
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
                    // Phase 61: a const param keeps a genVars SLOT (positional
                    // monomorphization) but is NOT a type Var in genEnv — it
                    // resolves to a symbolic const via currentConstParams_.
                    if (!gp.isConst) genEnv[gp.name] = v;
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
                    genExtraBounds.push_back(gp.extraBounds);
                    genBoundParam.push_back(&gp);
                }
                for (const auto& gp : fn.genericParams) {
                    // Review fix: a METHOD-level const param isn't monomorphized
                    // (no constParamNames on the impl-method schema), so it'd
                    // later leak an internal mangled name in codegen. Reject it
                    // early with a real diagnostic; declare it on the impl block.
                    if (gp.isConst)
                        error("const generic parameter '" + gp.name +
                                  "' on impl method '" + fn.name +
                                  "' is not supported; declare it on the `impl` "
                                  "block (`impl<.., const " + gp.name +
                                  ": i64> ..`) instead",
                              gp.line, gp.column);
                    TypePtr v = makeFreshVar();
                    if (!gp.isConst) genEnv[gp.name] = v; // Phase 61
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
                // Phase 61: the impl's const params in scope so the forType
                // `RingBuffer<T, CAP>` resolves CAP as a symbolic const arg.
                setConstParamsInScope(impl.genericParams, fn.genericParams);
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
                // Phase 61: impl + method const params in scope so `[T; CAP]`
                // and `RingBuffer<T, CAP>` resolve CAP symbolically.
                setConstParamsInScope(impl.genericParams, fn.genericParams);
                std::vector<TypePtr> argTypes;
                for (const auto& p : fn.params) {
                    argTypes.push_back(resolveTypeRef(p.type));
                }
                TypePtr ret = resolveTypeRef(fn.returnType);
                currentConstParams_.clear();
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

        // Pass 1d2 (Phase 60, v10): the EFFECT-SUBSET RULE — a trait impl
        // method's effects must be a SUBSET of the trait method's declared
        // effects. This is the effect system's last soundness floor: a `dyn
        // Trait` / generic-bound call attributes the TRAIT method's declared
        // effects (there is no concrete impl to mangle), so if an impl could
        // secretly do `io`/`alloc`/`panic` the trait didn't declare, a
        // pure-looking dispatch would silently perform them. Since `checkEffects`
        // already proves each impl body's effects ⊆ its declared effects, the
        // trait's declared set then bounds every impl body — attribution is
        // sound by construction. (Effect-row VARIABLES are polymorphic
        // placeholders, not concrete obligations, so only built-in labels are
        // compared.)
        for (const auto& impl : program.impls) {
            if (impl.isInherent()) continue; // inherent impls answer to no trait
            // Review fix: `Drop` is NOT exempt. It is a user-declarable trait,
            // so `&dyn Drop` dispatch (and a `<T: Drop>` bound) attributes the
            // trait method's declared effects — the earlier name-based exemption
            // let a pure-declared `dyn Drop::drop()` launder io/alloc/panic. The
            // subset rule now applies to every trait uniformly; a `Drop` impl
            // that performs io must declare it on the trait method.
            auto trIt = traits_.find(impl.traitName);
            if (trIt == traits_.end()) continue; // unknown trait (reported)
            for (const auto& fn : impl.methods) {
                const ast::MethodSig* tm = nullptr;
                for (const auto& m : trIt->second)
                    if (m.name == fn.name) { tm = &m; break; }
                if (!tm) continue; // not-in-trait already reported
                std::unordered_set<std::string> allowed;
                for (const auto& l : tm->effects.labels)
                    if (isBuiltinEffect(l)) allowed.insert(l);
                auto mit = implMethodMangled_.find(&fn);
                if (mit == implMethodMangled_.end()) continue;
                auto sit = fnSchemas_.find(mit->second);
                if (sit == fnSchemas_.end()) continue;
                for (const auto& l : sit->second.declaredEffects.labels) {
                    if (!isBuiltinEffect(l)) continue; // skip row vars
                    if (!allowed.count(l)) {
                        std::string allowedStr;
                        for (const auto& a : allowed)
                            allowedStr += (allowedStr.empty() ? "" : ", ") + a;
                        error("impl of trait '" + impl.traitName + "' for '" +
                                  impl.forType.name + "': method '" + fn.name +
                                  "' declares effect `" + l +
                                  "` that the trait method does not permit "
                                  "(trait allows: { " +
                                  (allowedStr.empty() ? "" : allowedStr + " ") +
                                  "}). A trait impl's effects must be a SUBSET "
                                  "of the trait's, so dyn/generic dispatch stays "
                                  "sound — declare `" + l +
                                  "` on the trait method, or remove it here",
                              fn.line, fn.column);
                    }
                }
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
            setConstParamsInScope(impl.genericParams); // Phase 61
            TypePtr selfTy = resolveTypeRef(impl.forType);
            currentConstParams_.clear();
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
        result.staticCallMangled = std::move(staticCallMangled_);
        result.staticCallGeneric = std::move(staticCallGeneric_);
        result.methodResolutions = std::move(methodResolutions_);
        result.binOpMethod = std::move(binOpMethod_); // v34 Phase 184
        result.dynCoercions = std::move(dynCoercions_);
        result.dynVtablesNeeded = std::move(dynVtablesNeeded_);
        result.assocProjections = std::move(assocProjections_);
        result.implAssocTypes = std::move(implAssocTypes_);
        result.implGatBindings = std::move(implGatBindings_); // v28 Phase 155
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
        // v28 Phase 152: aggregate const values. `isAgg` marks an array / tuple
        // / struct (`elems` in order; `fieldNames` parallel for a struct) or an
        // enum (`enumTag` >= 0, `elems` = the variant's payload). A scalar use
        // keeps isAgg=false. Aggregates exist so a `const` of these types can be
        // built + PROJECTED (`A[i]`, `p.field`) at compile time; codegen emits a
        // runtime use of the const by re-emitting its initializer.
        bool isAgg = false;
        int enumTag = -1;
        std::vector<ConstValue> elems;
        std::vector<std::string> fieldNames;
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
    // v34 Phase 185: thrown by a const `return e;` to unwind to the const fn
    // boundary, where `evalConstCall` catches it and yields the value. Using an
    // exception (rather than a bool flag) lets `return` short-circuit out of
    // arbitrarily nested `if` / `while` blocks inside a const fn — a flag only
    // unwinds the immediately enclosing block.
    struct ConstReturn {
        ConstValue value;
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
    // v25 Phase 136: trait -> its supertrait names.
    std::unordered_map<std::string, std::vector<std::string>> traitSupertraits_;
    // v31 Phase 167: explicit Send/Sync marker membership, keyed
    // [typeName]["Send"|"Sync"] -> +1 (an `impl Send for T {}` forces true,
    // for opaque/handle types) or -1 (an `impl !Send for T {}` opts out). A
    // type with NO entry (the common case) falls through to the structural
    // auto-derive in isSend/isSync. This is the single source of truth the
    // marker oracle consults; populated once after coherence (Pass 1c/1d).
    std::unordered_map<std::string, std::unordered_map<std::string, int>>
        markerImpls_;
    // v26 Phase 144: top-level type aliases (name -> aliased TypeRef).
    std::unordered_map<std::string, ast::TypeRef> typeAliases_;
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
    // v28 Phase 155 (GATs): a trait's associated-type ARITY —
    // traitGatArity_[trait][assocName] = number of generic params it declares
    // (`type Out<T>;` -> 1). 0 (or absent) = a plain Phase-21b associated type.
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::size_t>>
        traitGatArity_;
    // v28 Phase 155 (GATs): a parameterized impl binding `type Out<T> = Pair<T,
    // T>;` stored RAW (the RHS has free params), so a projection `Self::Out<i64>`
    // can substitute the supplied args into it on demand. Uses the result-header
    // GatBinding so it moves into the result for codegen verbatim.
    using GatBinding = TypeCheckResult::GatBinding;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string,
                           std::unordered_map<std::string, GatBinding>>>
        implGatBindings_;
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
    // Phase 48: qualified static call `Type::method()` -> mangled impl method.
    std::unordered_map<const ast::CallExpr*, std::string> staticCallMangled_;
    // Phase-4-fix: "type/trait" -> the impl's trait type-args as structural
    // strings ("*" = a generic Var, matches anything). Used to verify a
    // `dyn Trait<Args>` coercion's parameterization.
    std::unordered_map<std::string, std::vector<std::string>> implTraitArgStrs_;
    // Phase 52: generic static call `T::method()` -> (trait, method, varId).
    std::unordered_map<const ast::CallExpr*, TypeCheckResult::StaticGenericCall>
        staticCallGeneric_;
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
    // v34 Phase 184: an operator-overloaded binary op (`a + b` on a user type) ->
    // the mangled impl-method fn name codegen should call instead of LLVM arith.
    std::unordered_map<const ast::BinaryExpr*, std::string> binOpMethod_;
    // v32 Phase 176: user-declared effects. `userEffectNames_` makes an effect
    // name a valid (concrete) effect-row label; `effectOpDecls_` maps
    // effect -> op -> the AST op signature (param/return TypeRefs resolved
    // lazily at perform/handle sites).
    std::unordered_set<std::string> userEffectNames_;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, const ast::EffectOp*>>
        effectOpDecls_;
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
    // Phase 57 (v10): names of CONST-generic params in scope for the decl
    // currently being resolved, so `resolveTypeRef` treats `[T; N]` as a
    // symbolic-length array (recorded, not const-evaluated). Set/cleared in
    // lockstep with currentGenericEnv_.
    std::unordered_set<std::string> currentConstParams_;
    // v28 Phase 153: each in-scope const-generic param's declared type name
    // (i64/bool/char), so a value-use of the param has the right type + width.
    std::unordered_map<std::string, std::string> currentConstParamTypes_;
    // v28 Phase 153/154: the expected type of the expression currently being
    // checked, when known from an annotation/coercion target. A struct literal
    // consults it to adopt const-generic args it can't infer from a field
    // (`let s: Sel<true> = Sel { .. }`). null = no expectation.
    TypePtr currentExpectedType_ = nullptr;
    // v33 Phase 177: nesting depth of enclosing `unsafe { … }` blocks (> 0 means
    // raw-pointer derefs / int↔ptr casts are permitted at the current site).
    int unsafeDepth_ = 0;
    // Phase 61: when checking a call argument that is a closure, the callee's
    // expected fn-typed parameter — so `checkClosure` can infer an unannotated
    // `|x|`'s param type before checking the body. Consumed (cleared) by
    // checkClosure so it never leaks into nested expressions.
    TypePtr expectedArgType_;
    void setConstParamsInScope(const std::vector<ast::TypeParam>& ps) {
        currentConstParams_.clear();
        currentConstParamTypes_.clear();
        for (const auto& p : ps)
            if (p.isConst) {
                currentConstParams_.insert(p.name);
                currentConstParamTypes_[p.name] = p.constTypeName;
            }
    }
    // Phase 61: an impl method sees BOTH the impl's const params and its own
    // (`impl<.., const CAP> .. for RingBuffer<T, CAP>` + a `fn f<const M>`).
    void setConstParamsInScope(const std::vector<ast::TypeParam>& implPs,
                               const std::vector<ast::TypeParam>& fnPs) {
        currentConstParams_.clear();
        currentConstParamTypes_.clear();
        for (const auto& p : implPs) {
            if (p.isConst) {
                currentConstParams_.insert(p.name);
                currentConstParamTypes_[p.name] = p.constTypeName;
            }
        }
        for (const auto& p : fnPs)
            if (p.isConst) {
                currentConstParams_.insert(p.name);
                currentConstParamTypes_[p.name] = p.constTypeName;
            }
    }
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
               l == "async" || l == "unwind" || l == "share";
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
        // v32 Phase 176: a user-declared effect name is a CONCRETE label, never
        // an (implicit) effect-row variable — even if it appears in a fn-type
        // row. Keep it out of the row-var set.
        for (const auto& en : userEffectNames_) rowVars.erase(en);
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
            // v34 Phase 184: an operator-overloaded op runs its impl method, so
            // contribute that method's recorded effects.
            if (auto it = exprEffects_.find(bin); it != exprEffects_.end())
                out.unionWith(it->second);
            return;
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            collectEffects(*un->operand, out);
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CastExpr*>(&e)) {
            collectEffects(*ce->operand, out); // Phase 65: cast is pure glue
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
            // Phase-7-fix: prefer a per-site effect set when one was recorded
            // (a `dyn Trait` dispatch records the trait method's declared
            // effects — see checkDynMethodCall — since there is no concrete impl
            // schema to mangle; previously such a call contributed ZERO effects,
            // letting a pure-declared fn call an io/alloc trait method through a
            // trait object).
            if (auto eit = exprEffects_.find(mc); eit != exprEffects_.end()) {
                out.unionWith(eit->second);
                return;
            }
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
        // v32 Phase 176: performing an effect op contributes the effect E (plus
        // the arg effects and the op's own declared effects).
        if (auto* pe = dynamic_cast<const ast::PerformExpr*>(&e)) {
            for (const auto& a : pe->args) collectEffects(*a, out);
            out.add(pe->effectName);
            auto eff = effectOpDecls_.find(pe->effectName);
            if (eff != effectOpDecls_.end()) {
                auto op = eff->second.find(pe->opName);
                if (op != eff->second.end() && op->second)
                    for (const auto& l : op->second->effects.labels)
                        out.add(l);
            }
            return;
        }
        // v32 Phase 176: `handle { body } with E { … }` DISCHARGES E — body's
        // effects flow through MINUS E, plus each handler arm's body effects
        // (the arm runs when the op is performed during the handle).
        if (auto* he = dynamic_cast<const ast::HandleExpr*>(&e)) {
            EffectSet bodyEff;
            collectEffects(*he->body, bodyEff);
            for (const auto& l : bodyEff.labels)
                if (l != he->effectName) out.add(l);
            for (const auto& arm : he->arms)
                if (arm.handler && arm.handler->body)
                    collectEffects(*arm.handler->body, out);
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
            // v32 Phase 176: a user-declared effect name (`effect Logger {..}`)
            // is a valid CONCRETE effect-row label — a fn that `perform`s it
            // declares `! { Logger }`; a `handle … with Logger` discharges it.
            if (userEffectNames_.count(l)) {
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
                          "' (built-ins: alloc, io, panic, async, unwind, "
                          "share; or "
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
        // Phase 61: const params are NOT type Vars — they resolve symbolically
        // via currentConstParams_ (set by the caller around forType / body).
        for (const auto& gp : impl.genericParams)
            if (!gp.isConst) env[gp.name] = makeFreshVar();
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
            // Phase 61: const params hold a genericVars SLOT but are not type
            // Vars in genEnv (they resolve symbolically via currentConstParams_).
            if (gvi < schema.genericVars.size()) {
                if (!gp.isConst) genEnv[gp.name] = schema.genericVars[gvi];
                gvi++;
            }
        }
        for (const auto& gp : fn.genericParams) {
            if (gvi < schema.genericVars.size()) {
                if (!gp.isConst) genEnv[gp.name] = schema.genericVars[gvi];
                gvi++;
            }
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
        // Phase 61: impl + method const params in scope for the whole body.
        setConstParamsInScope(impl.genericParams, fn.genericParams);
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
        currentConstParams_.clear(); // Phase 59
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
        s.constParamNames.reserve(genericParams.size());
        // One fresh Var per declared generic param, positionally; duplicate
        // names get one Var each (and we report the duplicate below) so the
        // genericVars vector stays index-aligned with `genericParams`.
        // Phase 58: const params occupy a position too (their Var is never
        // referenced by field types — `[T; N]` carries N as `arrayLenParam`,
        // a name); `constParamNames[i]` records the name so monomorphization
        // can bind the supplied value.
        for (std::size_t i = 0; i < genericParams.size(); ++i) {
            s.genericVars.push_back(makeFreshVar());
            s.constParamNames.push_back(
                genericParams[i].isConst ? genericParams[i].name : std::string{});
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
    // Phase 58: structurally match a generic struct's symbolic field type
    // against a concrete value type to discover each symbolic array length
    // `[T; N]`. Records name->length in `out` (first binding wins; a
    // conflicting second use surfaces as a field-unify mismatch). Recurses
    // through arrays, refs/boxes, tuples and nested struct/enum fields — so
    // `data: [[i64; C]; R]` recovers both R (outer) and C (inner).
    void solveConstLengths(
        const TypePtr& schemaTy, const TypePtr& valTy,
        std::unordered_map<std::string, std::size_t>& out,
        std::unordered_map<std::string, std::string>& outSym) {
        TypePtr s = resolve(schemaTy);
        TypePtr v = resolve(valTy);
        if (s->kind != v->kind) return;
        switch (s->kind) {
        case TypeKind::Array:
            if (!s->arrayLenParam.empty()) {
                // Phase 61: a SYMBOLIC value length (`[T; CAP]` from a generic
                // field) binds the struct's param to that symbolic name; a
                // concrete length binds to its value.
                if (!v->arrayLenParam.empty())
                    outSym.emplace(s->arrayLenParam, v->arrayLenParam);
                else
                    out.emplace(s->arrayLenParam, v->arrayLen);
            }
            solveConstLengths(s->arrayElem, v->arrayElem, out, outSym);
            break;
        case TypeKind::Ref:
        case TypeKind::Box:
            solveConstLengths(s->refInner, v->refInner, out, outSym);
            break;
        case TypeKind::Tuple:
            for (std::size_t i = 0; i < s->tupleElems.size() &&
                                    i < v->tupleElems.size(); ++i)
                solveConstLengths(s->tupleElems[i], v->tupleElems[i], out,
                                  outSym);
            break;
        case TypeKind::Struct:
            for (std::size_t i = 0; i < s->structFields.size() &&
                                    i < v->structFields.size(); ++i)
                solveConstLengths(s->structFields[i].second,
                                  v->structFields[i].second, out, outSym);
            for (std::size_t i = 0; i < s->typeArgs.size() &&
                                    i < v->typeArgs.size(); ++i)
                solveConstLengths(s->typeArgs[i], v->typeArgs[i], out, outSym);
            break;
        case TypeKind::Enum:
            for (std::size_t i = 0; i < s->typeArgs.size() &&
                                    i < v->typeArgs.size(); ++i)
                solveConstLengths(s->typeArgs[i], v->typeArgs[i], out, outSym);
            break;
        default:
            break;
        }
    }

    // Shared field validation for a struct literal once its instance type is
    // fixed: unify each supplied field against the declared type, and report
    // unknown / duplicate / missing fields. `pre` (Phase 58) carries the
    // already-checked value types (so the const-generic path doesn't re-check
    // exprs); pass nullptr to check exprs inline.
    void validateStructLitFields(
        const ast::StructLitExpr& sl, const TypePtr& instType,
        const std::unordered_map<std::string, TypePtr>* pre) {
        std::unordered_map<std::string, TypePtr> declared;
        declared.reserve(instType->structFields.size());
        for (const auto& df : instType->structFields)
            declared.emplace(df.first, df.second);
        std::unordered_set<std::string> initialised;
        for (const auto& f : sl.fields) {
            TypePtr valT;
            if (pre) {
                auto pit = pre->find(f.first);
                valT = pit != pre->end() ? pit->second : checkExpr(*f.second);
            } else {
                valT = checkExpr(*f.second);
            }
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
            // v28 Phase 154: route through coerceOrUnify (the coercion entry
            // point) rather than a raw unify, so a struct-field value gets the
            // same bidirectional inference / coercions a fn arg does — an
            // unannotated `None` / empty container infers its type param from
            // the field, an int literal narrows, a `Box<Concrete>` coerces to a
            // `Box<dyn Trait>` field, a `&mut` reborrows to a `&` field.
            if (!coerceOrUnify(*f.second, valT, declIt->second)) {
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
    }

    // Phase 58: a const-param slot must be given a const value (`Mat<3>`),
    // and a type-param slot a type (`Vec<i64>`). Report a mix-up at the use
    // site. `kind` is "struct" / "enum" for the message.
    void checkConstSlots(const std::string& kind, const std::string& name,
                         const std::vector<std::string>& constParamNames,
                         const std::vector<TypePtr>& argTypes,
                         std::size_t line, std::size_t col) {
        for (std::size_t i = 0;
             i < argTypes.size() && i < constParamNames.size(); ++i) {
            bool isConstSlot = !constParamNames[i].empty();
            bool gotConst = resolve(argTypes[i])->isConstValue;
            if (isConstSlot && !gotConst) {
                error(kind + " '" + name + "' parameter '" +
                          constParamNames[i] +
                          "' is a `const` parameter; expected a const value, "
                          "got type '" + typeToString(argTypes[i]) + "'",
                      line, col);
            } else if (!isConstSlot && gotConst) {
                error(kind + " '" + name + "' expects a type at argument " +
                          std::to_string(i + 1) + ", got const value '" +
                          typeToString(argTypes[i]) + "'",
                      line, col);
            }
        }
    }

    TypePtr instantiateStructWithArgs(const StructSchema& schema,
                                       std::vector<TypePtr> typeArgs) {
        if (schema.genericVars.empty()) return schema.type;
        return instantiateGeneric(schema.type, schema.genericVars,
                                  schema.constParamNames, std::move(typeArgs),
                                  /*isStruct=*/true);
    }

    TypePtr instantiateEnumWithArgs(const EnumSchema& schema,
                                     std::vector<TypePtr> typeArgs) {
        if (schema.genericVars.empty()) return schema.type;
        return instantiateGeneric(schema.type, schema.genericVars,
                                  schema.constParamNames, std::move(typeArgs),
                                  /*isStruct=*/false);
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
            // v28 Phase 155: a GAT projection on a BOUNDED GENERIC param
            // (`C::Out<i64>`) would need the args threaded through
            // monomorphization — deferred. The concrete-`Self` GAT case (an impl
            // method) is supported above.
            if (!tr.assocTypeArgs.empty()) {
                error("a generic associated-type projection on a bounded "
                      "generic param ('" + tr.name + "::" + tr.assocName +
                      "<…>') is not yet supported; use it on a concrete type "
                      "(e.g. `Self::" + tr.assocName + "<…>` in an impl method)",
                      tr.line, tr.column);
                return makeInt();
            }
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
        else if (rb->kind == TypeKind::Char) typeName = "char"; // v27 P149
        else {
            error("associated type projection on unsupported base type " +
                      typeToString(base),
                  tr.line, tr.column);
            return makeInt();
        }
        // v28 Phase 155 (GATs): a parameterized projection `Self::Out<i64>` on a
        // concrete base — substitute the supplied args into the impl's raw
        // `type Out<T> = ...` binding and resolve it.
        if (!tr.assocTypeArgs.empty()) {
            auto gtyIt = implGatBindings_.find(typeName);
            if (gtyIt != implGatBindings_.end()) {
                for (const auto& [traitName, table] : gtyIt->second) {
                    auto bit = table.find(tr.assocName);
                    if (bit == table.end()) continue;
                    const GatBinding& gb = bit->second;
                    if (gb.paramNames.size() != tr.assocTypeArgs.size()) {
                        error("generic associated type '" + tr.assocName +
                                  "' of '" + typeName + "' expects " +
                                  std::to_string(gb.paramNames.size()) +
                                  " type argument(s), got " +
                                  std::to_string(tr.assocTypeArgs.size()),
                              tr.line, tr.column);
                        return makeInt();
                    }
                    // Resolve the supplied args in the CURRENT env, then bind
                    // Self + the GAT's params and resolve the binding RHS.
                    GenericEnv env;
                    env["Self"] = gb.selfTy;
                    for (std::size_t i = 0; i < gb.paramNames.size(); ++i)
                        env[gb.paramNames[i]] =
                            resolveTypeRef(tr.assocTypeArgs[i]);
                    const GenericEnv* saved = currentGenericEnv_;
                    currentGenericEnv_ = &env;
                    TypePtr result = resolveTypeRef(gb.rhs);
                    currentGenericEnv_ = saved;
                    return result;
                }
            }
            error("type '" + typeName + "' has no generic associated type '" +
                      tr.assocName + "'",
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
            case TypeKind::Int:   // any width i8..i64 / u8..u64 (C char..long)
            case TypeKind::Float: // v33 Phase 178: f64 -> C double, f32 -> float
            case TypeKind::Bool:
            case TypeKind::Ref: // any &T / &mut T / &[T] / *const T / *mut T -> C pointer
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
                  "' (allowed: i8..i64 / u8..u64, f32 / f64, bool, "
                  "&T / &mut T / &[T] / *const T / *mut T (C pointer), "
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
        // Phase 58 (v10): a const-generic VALUE in type-arg position — the `3`
        // in `Mat<3>`. It is not a type; produce a const-value Type that the
        // struct/enum/fn instantiation matches against its `const N` slot.
        if (tr.isConstArg) {
            if (tr.constArgValue < 0) {
                error("const generic argument must be non-negative, got " +
                          std::to_string(tr.constArgValue),
                      tr.line, tr.column);
                return makeConstValue(0);
            }
            return makeConstValue(tr.constArgValue);
        }
        // v26 Phase 144: a plain name (no type-args / assoc / fn) that names a
        // type alias resolves to its target; `&Meters` keeps the `&`. The alias
        // chain is followed iteratively with a cycle guard.
        if (!tr.isFn && tr.assocName.empty() && tr.typeArgs.empty() &&
            typeAliases_.count(tr.name)) {
            ast::TypeRef cur = tr;
            std::unordered_set<std::string> seen;
            while (!cur.isFn && cur.assocName.empty() && cur.typeArgs.empty() &&
                   typeAliases_.count(cur.name)) {
                if (!seen.insert(cur.name).second) {
                    error("cyclic type alias '" + cur.name + "'", tr.line,
                          tr.column);
                    return makeInt();
                }
                bool wasRef = cur.isRef, wasMut = cur.refIsMut;
                cur = typeAliases_[cur.name];
                if (wasRef) {
                    cur.isRef = true;
                    cur.refIsMut = wasMut;
                }
            }
            return resolveTypeRef(cur);
        }
        // Phase 61: a bare const-generic param used as a type argument — the
        // `CAP` in `RingBuffer<T, CAP>` or `C`/`R` in `Matrix<C, R>`. It is a
        // SYMBOLIC const value (resolved per monomorphization), not a type.
        if (!tr.isRef && !tr.isDyn && !tr.isSlice && !tr.isArray &&
            !tr.isTuple && !tr.isFn && tr.assocName.empty() &&
            tr.typeArgs.empty() && currentConstParams_.count(tr.name)) {
            return makeConstSymbol(tr.name);
        }
        // v33 Phase 177: a raw pointer `*const T` / `*mut T`. The raw-ptr flags
        // live on the pointee's TypeRef node; resolve the pointee with them
        // cleared, then wrap in a raw-pointer Type.
        if (tr.isRawPtr) {
            ast::TypeRef inner = tr;
            inner.isRawPtr = false;
            inner.rawPtrMut = false;
            return makeRawPtr(resolveTypeRef(inner), tr.rawPtrMut);
        }
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
            // Phase 57: a SYMBOLIC length `[T; N]` where N is a const-generic
            // param in scope. Don't const-eval (N has no value until the type
            // is instantiated) — record the name so Phase 58 substitutes it.
            if (tr.arrayLenExpr) {
                if (auto* id =
                        dynamic_cast<const ast::IdentExpr*>(tr.arrayLenExpr.get());
                    id && currentConstParams_.count(id->name)) {
                    TypePtr arr = makeArray(elem, 0);
                    arr->arrayLenParam = id->name;
                    if (tr.isRef) return makeRef(arr, tr.refIsMut);
                    return arr;
                }
            }
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
            // Phase 49: a parameterized trait object `dyn Trait<Args>` (e.g.
            // `dyn Producer<i64>`). The trait's concrete args are carried on the
            // Dyn type (in `typeArgs`); checkDynMethodCall binds the trait's
            // params to them so a method returning the trait param resolves to
            // the concrete type. The vtable thunk forwards to the impl method's
            // already-concrete signature, so codegen needs nothing extra.
            auto pit = traitGenericParams_.find(tr.name);
            std::size_t want =
                pit != traitGenericParams_.end() ? pit->second.size() : 0;
            if (tr.typeArgs.size() != want) {
                error("`dyn " + tr.name + "` expects " +
                          std::to_string(want) + " trait type arg(s), got " +
                          std::to_string(tr.typeArgs.size()),
                      tr.line, tr.column);
                return makeInt();
            }
            checkObjectSafe(tr.name, tr.line, tr.column);
            TypePtr d = makeDyn(tr.name);
            for (const auto& a : tr.typeArgs)
                d->typeArgs.push_back(resolveTypeRef(a));
            return d;
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
            // Phase 145: carry a `Fn(..)`/`FnMut(..)`/`FnOnce(..)` bound onto
            // the Function type so coerceOrUnify can check a passed closure's
            // kind satisfies it (a bare `fn(..)` leaves closureBound at -1).
            fnTy->closureBound = tr.closureBound;
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
        // v11: the SIZED machine integers — signed i8/i16/i32 (Phase 63) and
        // unsigned u8/u16/u32/u64 (Phase 66); i64 is the signed default above.
        // Each is a distinct non-coercive type (unify checks width AND sign);
        // `as` bridges between them, and codegen picks udiv/urem/lshr/icmp-u
        // for the unsigned ones.
        {
            static const struct { const char* nm; int w; bool sg; } kInts[] = {
                {"i8", 8, true},  {"i16", 16, true},  {"i32", 32, true},
                {"u8", 8, false}, {"u16", 16, false}, {"u32", 32, false},
                {"u64", 64, false}};
            for (const auto& it : kInts) {
                if (tr.name == it.nm) {
                    if (!tr.typeArgs.empty())
                        error(std::string(it.nm) + " takes no type arguments",
                              tr.line, tr.column);
                    return makeIntW(it.w, it.sg);
                }
            }
        }
        if (tr.name == "f64") {
            if (!tr.typeArgs.empty())
                error("f64 takes no type arguments", tr.line, tr.column);
            return makeFloat();
        }
        if (tr.name == "f32") { // Phase 67: single-precision float
            if (!tr.typeArgs.empty())
                error("f32 takes no type arguments", tr.line, tr.column);
            return makeFloatW(32);
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
        if (tr.name == "char") { // v27 Phase 147
            if (!tr.typeArgs.empty())
                error("char takes no type arguments", tr.line, tr.column);
            return makeChar();
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
            checkConstSlots("struct", tr.name, sit->second.constParamNames,
                            argTypes, tr.line, tr.column);
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
            checkConstSlots("enum", tr.name, eit->second.constParamNames,
                            argTypes, tr.line, tr.column);
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
            // Phase 61: const params resolve symbolically (currentConstParams_),
            // not as type Vars in genEnv.
            if (!fn.genericParams[i].isConst)
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

        // Phase 59: const-generic params are in scope for the whole body — so
        // `[i64; N]` param types are symbolic and a bare `N` is a const value.
        setConstParamsInScope(fn.genericParams);
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
        currentConstParams_.clear(); // Phase 59
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
        ConstValue v;
        try {
            v = evalConstExpr(*declIt->second->value, /*env=*/nullptr,
                              /*depth=*/0);
        } catch (const ConstReturn& cr) {
            // Defensive: a const item whose initializer is a block with a
            // top-level `return e;` yields that value.
            v = cr.value;
        }
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
            // v28 Phase 152: a bare unit enum variant (`None`) in a const expr.
            auto vl = lookupVariant(id->name);
            if (vl.enumInstance) {
                ConstValue agg;
                agg.isAgg = true;
                agg.enumTag = static_cast<int>(vl.variantIdx);
                return agg;
            }
            // Not a local / variant — must be another const.
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
                return {false, wrapConstToType(-v.i, e)};
            }
            // Not (bool -> bool).
            if (!v.isBool)
                constFail("unary `!` requires bool in a const expr", e);
            return {true, v.i ? 0 : 1};
        }
        if (auto* ce = dynamic_cast<const ast::CastExpr*>(&e)) {
            // Phase 65: a numeric `as` cast inside a const expr. Only int->int
            // is const-foldable (the const evaluator is integer-valued); a
            // cast to/from f64 is rejected honestly. The value wraps into the
            // target width with two's-complement semantics, matching codegen.
            ConstValue v = evalConstExpr(*ce->operand, env, depth);
            if (v.isBool)
                constFail("`as` cast requires a numeric operand in a const "
                          "expr",
                          e);
            TypePtr dst = resolve(resolveTypeRef(ce->targetType));
            if (dst->kind != TypeKind::Int || dst->isConstValue)
                constFail("`as` to a non-integer type is not allowed in a "
                          "const expr",
                          e);
            long long val = v.i;
            if (dst->intWidth < 64) {
                unsigned long long mask = (1ULL << dst->intWidth) - 1;
                unsigned long long u =
                    static_cast<unsigned long long>(val) & mask;
                if (dst->intSigned && (u & (1ULL << (dst->intWidth - 1))))
                    u |= ~mask; // sign-extend the narrow signed value
                val = static_cast<long long>(u);
            }
            return {false, val};
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
        // v28 Phase 152: aggregate const values — array / tuple / struct
        // literals, enum constructors, and projection out of them.
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            ConstValue agg;
            agg.isAgg = true;
            if (al->repeatCount) {
                // `[value; N]` — N copies of the repeated value.
                if (al->elements.empty())
                    constFail("malformed array-repeat in const expr", e);
                ConstValue v = evalConstExpr(*al->elements[0], env, depth);
                std::int64_t n = 0;
                if (!evalConstI64(*al->repeatCount, n) || n < 0)
                    constFail("array-repeat length must be a non-negative const "
                              "in a const expr",
                              e);
                agg.elems.assign(static_cast<std::size_t>(n), v);
            } else {
                for (const auto& el : al->elements)
                    agg.elems.push_back(evalConstExpr(*el, env, depth));
            }
            return agg;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            ConstValue agg;
            agg.isAgg = true;
            for (const auto& el : tl->elements)
                agg.elems.push_back(evalConstExpr(*el, env, depth));
            return agg;
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            ConstValue agg;
            agg.isAgg = true;
            for (const auto& [fname, fexpr] : sl->fields) {
                agg.fieldNames.push_back(fname);
                agg.elems.push_back(evalConstExpr(*fexpr, env, depth));
            }
            return agg;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            ConstValue obj = evalConstExpr(*ix->object, env, depth);
            if (!obj.isAgg || obj.enumTag >= 0)
                constFail("indexing requires an array/tuple const value", e);
            ConstValue idx = evalConstExpr(*ix->index, env, depth);
            if (idx.isBool || idx.isAgg)
                constFail("array index must be an integer in a const expr", e);
            if (idx.i < 0 ||
                static_cast<std::size_t>(idx.i) >= obj.elems.size())
                constFail("const array index " + std::to_string(idx.i) +
                              " out of bounds (len " +
                              std::to_string(obj.elems.size()) + ")",
                          e);
            return obj.elems[static_cast<std::size_t>(idx.i)];
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            ConstValue obj = evalConstExpr(*fe->object, env, depth);
            if (!obj.isAgg || obj.enumTag >= 0)
                constFail("field access requires a struct const value", e);
            for (std::size_t k = 0; k < obj.fieldNames.size(); ++k)
                if (obj.fieldNames[k] == fe->fieldName) return obj.elems[k];
            constFail("unknown field `" + fe->fieldName + "` in const struct value",
                      e);
        }
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e)) {
            ConstValue obj = evalConstExpr(*tf->object, env, depth);
            if (!obj.isAgg || obj.enumTag >= 0)
                constFail("tuple-field access requires a tuple const value", e);
            if (tf->index >= obj.elems.size())
                constFail("const tuple index out of bounds", e);
            return obj.elems[tf->index];
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // v28 Phase 152: an enum constructor with a payload in a const expr
            // (e.g. `Some(5)`). A unit variant arrives as a bare IdentExpr and is
            // handled by evalConstByName -> lookupVariant below.
            auto vl = lookupVariant(call->callee);
            if (vl.enumInstance) {
                ConstValue agg;
                agg.isAgg = true;
                agg.enumTag = static_cast<int>(vl.variantIdx);
                for (const auto& a : call->args)
                    agg.elems.push_back(evalConstExpr(*a, env, depth));
                return agg;
            }
            return evalConstCall(*call, env, depth);
        }
        constFail("expression is not allowed in a const context (only int/"
                  "bool literals, arithmetic/comparison/unary operators, "
                  "`if`/`else`, `let`, calls to `const fn`s, and "
                  "array/tuple/struct/enum aggregates are const-evaluable)",
                  e);
    }

    // v11 fix: wrap a const-folded integer value to the width/signedness of
    // `e`'s recorded type, with two's-complement semantics that MATCH runtime
    // codegen. A narrow UNSIGNED result becomes a POSITIVE i64 in range, and a
    // narrow SIGNED result is sign-extended — so every subsequent i64 op
    // (compare, `/`, `>>`, ...) on the wrapped value gives the type-correct
    // answer (an unsigned `>>` becomes a logical shift for free). i64/u64 (or
    // an untyped expr) pass through unchanged. This is what makes a sized/
    // unsigned `const` fold identically to the same expression at run time.
    long long wrapConstToType(long long val, const ast::Expr& e) {
        auto it = exprTypes_.find(&e);
        if (it == exprTypes_.end()) return val;
        TypePtr t = resolve(it->second);
        if (t->kind != TypeKind::Int || t->isConstValue || t->intWidth >= 64)
            return val;
        int w = t->intWidth;
        unsigned long long mask = (1ULL << w) - 1;
        unsigned long long u = static_cast<unsigned long long>(val) & mask;
        if (t->intSigned && (u & (1ULL << (w - 1))))
            u |= ~mask; // sign-extend a narrow signed value
        return static_cast<long long>(u);
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
        case Op::Or: // Phase 124: bool || bool (both operands treated as 0/1)
            return {true, (l.i != 0 || r.i != 0) ? 1 : 0};
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
            // Wrap to the result's width so a narrow const folds like runtime.
            return {false, wrapConstToType(out, bin)};
        }
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr: {
            // Phase 66 + v11 fix: integer bitwise ops fold at const time. Each
            // operand is already WRAPPED to its own type's width (an unsigned
            // narrow value is a positive i64, a signed one is sign-extended), so
            // `l.i >> r.i` is a LOGICAL shift for an unsigned operand and an
            // ARITHMETIC shift for a signed one — matching runtime — and the
            // result is re-wrapped to the op's width below.
            if (l.isBool || r.isBool)
                constFail("bitwise op requires integer operands in a const "
                          "expr",
                          bin);
            std::int64_t out = 0;
            if (bin.op == Op::BitAnd) out = l.i & r.i;
            else if (bin.op == Op::BitOr) out = l.i | r.i;
            else if (bin.op == Op::BitXor) out = l.i ^ r.i;
            else if (bin.op == Op::Shl) {
                if (r.i < 0 || r.i >= 64)
                    constFail("shift amount out of range (0..63) in const expr",
                              bin);
                out = static_cast<std::int64_t>(static_cast<std::uint64_t>(l.i)
                                                << r.i);
            } else { // Shr (arithmetic, for the signed const evaluator)
                if (r.i < 0 || r.i >= 64)
                    constFail("shift amount out of range (0..63) in const expr",
                              bin);
                out = l.i >> r.i;
            }
            return {false, wrapConstToType(out, bin)};
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
        evalConstStmts(blk.stmts, local, depth);
        // A tail-less block is unit-valued (e.g. an `if c { … } else { … }`
        // used as a statement, where both arms end in a `;`-statement). We
        // return a unit sentinel rather than erroring: such a block only ever
        // reaches here in statement position (its value is dropped), and the
        // type checker independently guarantees that a const fn / const item
        // declared with a scalar type actually yields one — so a unit value
        // can never silently leak out as the const's result.
        if (!blk.tail)
            return ConstValue{};  // unit
        return evalConstExpr(*blk.tail, &local, depth);
    }

    // v34 Phase 185: run a sequence of const statements against the MUTABLE env
    // `local`, in order. Beyond plain `let`, this const-evaluates VARIABLE
    // ASSIGNMENT (`x = e;`, simple ident target) and `while` LOOPS — together
    // these let a `const fn` use the imperative `let mut … ; while … { … }`
    // style (e.g. an iterative factorial / fibonacci) at compile time. A
    // `return e;` throws `ConstReturn` to unwind to the const fn boundary, so
    // it short-circuits out of any nesting (e.g. an `if` inside the loop).
    // Non-termination is caught by the global step budget (`kConstEvalMaxSteps`,
    // checked in `constTick`): every condition + body re-evaluation ticks it,
    // so a runaway loop fails cleanly rather than hanging the compiler. The loop
    // body shares `local` (no fresh scope copy) so its mutations persist across
    // iterations and out to the enclosing block — the imprecision (a body `let`
    // leaks past the loop) is harmless for const-eval, which only computes a
    // value.
    void evalConstStmts(const std::vector<ast::StmtPtr>& stmts,
                        ConstEnv& local, int depth) {
        for (const auto& s : stmts) {
            if (auto* let = dynamic_cast<const ast::LetStmt*>(s.get())) {
                if (!let->tupleNames.empty())
                    throw ConstEvalError{"tuple-destructuring `let` is not "
                                         "const-evaluable",
                                         let->line, let->column};
                if (!let->value)
                    throw ConstEvalError{"`let` without an initializer is not "
                                         "const-evaluable",
                                         let->line, let->column};
                local[let->name] = evalConstExpr(*let->value, &local, depth);
            } else if (auto* r =
                           dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                if (!r->value)
                    throw ConstEvalError{"bare `return;` is not "
                                         "const-evaluable (a const fn must "
                                         "yield a value)",
                                         r->line, r->column};
                // Unwind to the const fn boundary (caught in evalConstCall).
                throw ConstReturn{evalConstExpr(*r->value, &local, depth)};
            } else if (auto* as =
                           dynamic_cast<const ast::AssignStmt*>(s.get())) {
                auto* id = dynamic_cast<const ast::IdentExpr*>(as->target.get());
                if (!id)
                    throw ConstEvalError{"only simple-variable assignment is "
                                         "const-evaluable (no field / index "
                                         "assignment in a const context)",
                                         as->line, as->column};
                if (local.find(id->name) == local.end())
                    throw ConstEvalError{"assignment to `" + id->name +
                                             "`, which is not a local in scope, "
                                             "is not const-evaluable",
                                         as->line, as->column};
                local[id->name] = evalConstExpr(*as->value, &local, depth);
            } else if (auto* es =
                           dynamic_cast<const ast::ExprStmt*>(s.get())) {
                if (auto* we =
                        dynamic_cast<const ast::WhileExpr*>(es->expr.get())) {
                    while (true) {
                        ConstValue c =
                            evalConstExpr(*we->cond, &local, depth);
                        if (!c.isBool)
                            throw ConstEvalError{"`while` condition must be a "
                                                 "bool in a const context",
                                                 we->line, we->column};
                        if (c.i == 0)
                            break;
                        if (auto* body = dynamic_cast<const ast::BlockExpr*>(
                                we->body.get())) {
                            // A `return` inside the loop throws ConstReturn,
                            // which unwinds straight past this loop.
                            evalConstStmts(body->stmts, local, depth);
                            if (body->tail)
                                evalConstExpr(*body->tail, &local, depth);
                        }
                    }
                } else {
                    // Evaluate for its (absence of) effects; value dropped.
                    // Also surfaces an error for a non-const-evaluable stmt.
                    evalConstExpr(*es->expr, &local, depth);
                }
            } else {
                throw ConstEvalError{"statement is not const-evaluable",
                                     s->line, s->column};
            }
        }
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
        // The fn body is the boundary at which a `return e;` (thrown as
        // ConstReturn from any nesting depth) yields the fn's value.
        try {
            return evalConstBlock(*fn.body, &callee, depth + 1);
        } catch (const ConstReturn& cr) {
            return cr.value;
        }
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
        // v28 Phase 152: a const may be a scalar (i64/bool) OR an aggregate
        // (array / tuple / struct / enum) whose leaves are const-evaluable. The
        // evaluator enforces leaf-level evaluability; the type gate just admits
        // the aggregate shapes here. (char / f64 scalar consts stay a documented
        // follow-on — the integer evaluator + the const-use codegen width handle
        // i64/bool today.)
        bool okType = rDeclared->kind == TypeKind::Int ||
                      rDeclared->kind == TypeKind::Bool ||
                      rDeclared->kind == TypeKind::Array ||
                      rDeclared->kind == TypeKind::Tuple ||
                      rDeclared->kind == TypeKind::Struct ||
                      rDeclared->kind == TypeKind::Enum;
        if (!okType) {
            error("a `const` must be a scalar (i64/bool) or an aggregate "
                  "(array/tuple/struct/enum) of const-evaluable values, got " +
                      typeToString(declared),
                  cd.type.line, cd.type.column);
        }
        // Type-check the initializer expression (records exprTypes for codegen
        // + surfaces ordinary type errors), then coerce to the declared type.
        if (cd.value) {
            TypePtr initTy = checkExpr(*cd.value);
            // v11 fix: route through coerceOrUnify (not a raw unify) so a plain
            // narrow/unsigned literal initializer narrows just like a `let`
            // (`const C: i32 = 100`, `const O: u64 = 0xcbf...`) — and so an
            // out-of-range literal is range-checked here too.
            if (okType && !coerceOrUnify(*cd.value, initTy, declared)) {
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
            // v28 Phase 152: an AGGREGATE const value (array/tuple/struct/enum)
            // isn't a scalar immediate — codegen re-emits the initializer at a
            // runtime use, so only a SCALAR result flows through constExprValues_
            // (the codegen-facing folded-literal table). The aggregate stays in
            // constValues_ for compile-time PROJECTION by other const exprs.
            if (!v.isAgg) {
                // v11 fix: wrap the final value to the DECLARED width too, so a
                // bare-literal initializer (`const C: i8 = 200i8`) is stored at
                // the right width — codegen emits it narrow.
                if (!v.isBool) v.i = wrapConstToType(v.i, *cd.value);
                constExprValues_[cd.value.get()] = v;
            }
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
        if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(&e)) {
            // Phase 64: a SUFFIXED literal (`5i32`) has a fixed concrete width
            // and signedness — it does not narrow, it IS that type. A range
            // check at the suffixed width still applies (see below).
            if (lit->suffixWidth != 0) {
                // Phase 64/66: a suffixed literal IS its concrete type (signed
                // i8..i64 or unsigned u8..u64), range-checked at that width.
                if (!intLitFitsWidth(lit->value, lit->suffixWidth,
                                     lit->suffixSigned)) {
                    error("integer literal " + std::to_string(lit->value) +
                              " out of range for " +
                              intTypeName(lit->suffixWidth, lit->suffixSigned),
                          e.line, e.column);
                }
                return makeIntW(lit->suffixWidth, lit->suffixSigned);
            }
            // v11: an unsuffixed integer literal is i64 by default; a coercion
            // site (let annotation / fn arg / binary operand / return) may
            // NARROW it to a concrete int width via narrowIntLiteral(), which
            // re-records its exprType — see coerceOrUnify.
            return makeInt();
        }
        if (auto* fl = dynamic_cast<const ast::FloatLitExpr*>(&e)) {
            // Phase 67: a suffixed float literal (`1.5f32`) IS that width; an
            // unsuffixed one is f64 by default and narrows to f32 in context
            // (narrowFloatLiteral, at coercion sites — see coerceOrUnify).
            if (fl->suffixWidth != 0) return makeFloatW(fl->suffixWidth);
            return makeFloat();
        }
        if (dynamic_cast<const ast::BoolLitExpr*>(&e)) {
            return makeBool();
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            return checkUnary(*un);
        }
        if (auto* ce = dynamic_cast<const ast::CastExpr*>(&e)) {
            return checkCast(*ce);
        }
        if (dynamic_cast<const ast::StringLitExpr*>(&e)) {
            auto it = structSchemas_.find("String");
            return it != structSchemas_.end() ? it->second.type
                                              : makeStruct("String", {});
        }
        if (dynamic_cast<const ast::CharLitExpr*>(&e)) {
            return makeChar(); // v27 Phase 147
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
                    // v28 Phase 152: only a SCALAR const use is a folded
                    // immediate; an aggregate const use is re-emitted from its
                    // initializer by codegen (so don't record a bogus scalar).
                    if (!v.isAgg) constExprValues_[id] = v;
                } catch (const ConstEvalError& ce) {
                    // The error is reported once at the const's own definition
                    // site (checkConstItem); avoid a duplicate here, but still
                    // give the use site a sensible type.
                }
                return declared;
            }
            // Phase 59: a bare const-generic param used as a VALUE (`N` in
            // `dot<const N>`'s body) — type i64, monomorphized to the concrete
            // value per instance (codegen emits a literal). A local of the same
            // name shadows it (handled by lookupLocal above).
            if (currentConstParams_.count(id->name)) {
                // v28 Phase 153: the value's type is the param's declared type.
                auto tit = currentConstParamTypes_.find(id->name);
                if (tit != currentConstParamTypes_.end()) {
                    if (tit->second == "bool") return makeBool();
                    if (tit->second == "char") return makeChar();
                }
                return makeInt();
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
        if (auto* pe = dynamic_cast<const ast::PerformExpr*>(&e)) {
            return checkPerform(*pe);
        }
        if (auto* he = dynamic_cast<const ast::HandleExpr*>(&e)) {
            return checkHandle(*he);
        }
        if (auto* ue = dynamic_cast<const ast::UnsafeExpr*>(&e)) {
            // v33 Phase 177: `unsafe { … }` permits unchecked ops in the body;
            // its value is the body's value.
            unsafeDepth_++;
            TypePtr t = ue->body ? checkExpr(*ue->body) : makeUnit();
            unsafeDepth_--;
            return t;
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
    // v11: narrow an unsuffixed integer LITERAL `srcExpr` to a concrete int
    // `target` (a literal is i64 by default; in an int context it adopts the
    // target's width). Records the literal's exprType so codegen emits the
    // right width; range-checks narrow widths. No-op (false) for non-literals.
    // Phase 63/64: does `value` fit a signed/unsigned integer of `width` bits?
    // A 64-bit width always fits (the literal is a `long long`).
    static bool intLitFitsWidth(long long value, int width, bool isSigned) {
        if (width >= 64) return true;
        long long lo = isSigned ? -(1LL << (width - 1)) : 0;
        long long hi = isSigned ? (1LL << (width - 1)) - 1
                                : (1LL << width) - 1;
        return value >= lo && value <= hi;
    }

    bool narrowIntLiteral(const ast::Expr& srcExpr, const TypePtr& target) {
        TypePtr t = resolve(target);
        if (t->kind != TypeKind::Int || t->isConstValue) return false;
        if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(&srcExpr)) {
            if (!intLitFitsWidth(lit->value, t->intWidth, t->intSigned))
                error("integer literal " + std::to_string(lit->value) +
                          " out of range for " + typeToString(t),
                      srcExpr.line, srcExpr.column);
            exprTypes_[&srcExpr] = t;
            return true;
        }
        // Phase 67: a NEGATED literal `-N` narrows too, so a narrow signed type
        // can hold its minimum (`let x: i8 = -128`). The NEGATED value is the
        // one range-checked (and `let x: u8 = -1` is correctly rejected). The
        // inner literal is recorded at the target width as well, so codegen
        // emits both at that width (the negate wraps in two's complement).
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&srcExpr)) {
            if (un->op == ast::UnaryOp::Neg) {
                if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(
                        un->operand.get())) {
                    long long neg = -lit->value;
                    if (!intLitFitsWidth(neg, t->intWidth, t->intSigned))
                        error("integer literal " + std::to_string(neg) +
                                  " out of range for " + typeToString(t),
                              srcExpr.line, srcExpr.column);
                    exprTypes_[un] = t;
                    exprTypes_[un->operand.get()] = t;
                    return true;
                }
            }
        }
        return false;
    }

    // Phase 67: narrow an unsuffixed FLOAT literal `srcExpr` to a concrete
    // float `target` (a float literal is f64 by default; in a context that
    // wants f32 it adopts f32). Records its exprType so codegen emits the right
    // width. No-op (false) for non-literals or a non-float target.
    bool narrowFloatLiteral(const ast::Expr& srcExpr, const TypePtr& target) {
        auto* lit = dynamic_cast<const ast::FloatLitExpr*>(&srcExpr);
        if (!lit || lit->suffixWidth != 0) return false;
        TypePtr t = resolve(target);
        if (t->kind != TypeKind::Float) return false;
        exprTypes_[&srcExpr] = t;
        return true;
    }

    // Phase 145: human names for the closure-kind ranks.
    static const char* closureKindName(int rank) {
        switch (rank) {
            case 0: return "Fn";
            case 1: return "FnMut";
            default: return "FnOnce";
        }
    }

    bool coerceOrUnify(const ast::Expr& srcExpr, const TypePtr& actual,
                       const TypePtr& expected) {
        TypePtr e = resolve(expected);
        TypePtr a = resolve(actual);
        // Phase 145: when the expected slot is a `Fn(..)`/`FnMut(..)`/
        // `FnOnce(..)` bound (closureBound >= 0), check the supplied callable's
        // kind satisfies it. A closure EXPRESSION carries its classified kind
        // on the node; any other callable (a top-level fn name, a fn-typed
        // variable) is treated as `Fn` — the most permissive, satisfying every
        // bound — since a plain fn pointer captures nothing. (Enforcement is
        // exact for a closure passed directly; a closure routed through an
        // intermediate `let` binding loses its kind and reads as `Fn`.)
        if (e->kind == TypeKind::Function && e->closureBound >= 0) {
            int actualRank = 0;
            if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&srcExpr))
                actualRank = static_cast<int>(cl->kind);
            if (actualRank > e->closureBound) {
                error(std::string("this closure is `") +
                          closureKindName(actualRank) +
                          "` (it " +
                          (actualRank == 2
                               ? "moves a captured value out"
                               : "mutates a captured binding") +
                          "), but a `" + closureKindName(e->closureBound) +
                          "` is required here; an `" +
                          closureKindName(e->closureBound) +
                          "` callable may not " +
                          (e->closureBound == 0
                               ? "mutate or consume its captures"
                               : "consume its captures"),
                      srcExpr.line, srcExpr.column);
                // fall through to unify so the signature is still checked
            }
        }
        // v11: an integer literal narrows to the expected concrete int width
        // (`let x: i32 = 5`, `f(5)` with an i32 param). Only when `actual` is
        // a plain i64 (the literal's default) — never override a real type.
        if (e->kind == TypeKind::Int && !e->isConstValue &&
            a->kind == TypeKind::Int && !a->isConstValue && a->intWidth == 64 &&
            a->intSigned && narrowIntLiteral(srcExpr, e))
            return true;
        // Phase 67: an f64-default float literal narrows to an expected f32.
        if (e->kind == TypeKind::Float && e->floatWidth == 32 &&
            a->kind == TypeKind::Float && a->floatWidth == 64 &&
            narrowFloatLiteral(srcExpr, e))
            return true;
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
                    // Phase-4-fix: for a PARAMETERIZED `dyn Trait<Args>`, the
                    // concrete type's impl of the trait must carry MATCHING
                    // trait args — `&IntGen` (impl Producer<i64>) must NOT
                    // coerce to `&dyn Producer<String>`. A "*" (generic) impl
                    // arg matches anything. On mismatch, fall through so unify
                    // fails and the caller reports a type error.
                    bool argsOk = true;
                    if (!typeName.empty() && !inner->typeArgs.empty()) {
                        auto tit = implTraitArgStrs_.find(
                            typeName + "/" + inner->dynTraitName);
                        if (tit != implTraitArgStrs_.end()) {
                            if (tit->second.size() != inner->typeArgs.size()) {
                                argsOk = false;
                            } else {
                                for (std::size_t i = 0;
                                     i < inner->typeArgs.size(); ++i) {
                                    if (tit->second[i] == "*") continue;
                                    if (tit->second[i] !=
                                        typeToString(resolve(inner->typeArgs[i]))) {
                                        argsOk = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (argsOk && !typeName.empty() &&
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
        // Phase 47: a `&mut T` reborrows as a shared `&T` — passing a mutable
        // reference where an immutable one is expected is sound (shared access
        // is the weaker capability), and bit-identical at codegen (both are
        // plain pointers). Only mut->shared; never the reverse. This lets a
        // `fn f(v: &mut Vec<T>)` call `vec_len(v)` / `vec_get_ref(v, i)` (whose
        // params are `&Vec<T>`) without an explicit reborrow spelling.
        if (expIsRef && !e->refIsMut && a->kind == TypeKind::Ref &&
            a->refIsMut && unify(a->refInner, e->refInner)) {
            return true;
        }
        // v32 Phase 175: effect SUBTYPING (subsumption). A fn value that
        // performs FEWER effects is usable where one with MORE effects is
        // expected — calling it can only do at most what the expected signature
        // permits — so a pure `fn()->R` coerces to a `fn()->R ! {io}` parameter.
        // Unify the arg + return types as usual, then accept if the ACTUAL's
        // effect labels are a SUBSET of the EXPECTED's (the extra effects the
        // expected allows simply aren't performed). The reverse — an actual that
        // does MORE than expected — falls through to the exact unify (rejected).
        // We fire ONLY when the actual row is CLOSED and the expected has
        // strictly more labels (or an open tail): an open ACTUAL tail, and the
        // exact-match case, keep the symmetric unify so effect-row threading
        // (e.g. the `! {e}` row var of vec_map / future_map) is unchanged.
        if (e->kind == TypeKind::Function && a->kind == TypeKind::Function &&
            e->closureBound < 0 && a->args.size() == e->args.size()) {
            TypePtr aRv = a->effectRowVar ? resolve(a->effectRowVar) : nullptr;
            TypePtr eRv = e->effectRowVar ? resolve(e->effectRowVar) : nullptr;
            bool aOpen = aRv && aRv->kind == TypeKind::Var &&
                         !aRv->effectRowSolved;
            bool eOpen = eRv && eRv->kind == TypeKind::Var &&
                         !eRv->effectRowSolved;
            if (!aOpen) {
                std::vector<std::string> al = resolveEffectRow(a);
                std::vector<std::string> el = resolveEffectRow(e);
                bool actualSubset = true;
                for (const auto& l : al)
                    if (std::find(el.begin(), el.end(), l) == el.end()) {
                        actualSubset = false;
                        break;
                    }
                if (actualSubset && (el.size() > al.size() || eOpen)) {
                    bool ok = true;
                    for (std::size_t i = 0; i < a->args.size() && ok; ++i)
                        if (!unify(a->args[i], e->args[i])) ok = false;
                    if (ok && !unify(a->ret, e->ret)) ok = false;
                    if (ok) return true;
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
        // Peel ANY number of pointer layers to find a `dyn` underneath:
        // `&dyn`, `Box<dyn>`, AND `&Box<dyn>` (the shape returned by
        // vec_get_ref over a `Vec<Box<dyn Trait>>`). Codegen's dyn dispatch
        // already loads the fat pointer through an arbitrary pointer receiver.
        {
            TypePtr p = r;
            while (p->kind == TypeKind::Ref || p->kind == TypeKind::Box)
                p = resolve(p->refInner);
            if (p->kind == TypeKind::Dyn)
                return checkDynMethodCall(mc, p->dynTraitName, recvT);
        }

        // Phase 2.4b auto-deref: if the receiver is `&T` (or `&mut T`),
        // dispatch as though the receiver were the underlying `T`. Phase 11: a
        // `Box<Concrete>` derefs the same way (method call through the box).
        // Phase 51: but STOP peeling at a `Box` that ITSELF has an impl of the
        // called method (e.g. `impl<T: Clone> Clone for Box<T>`), so
        // `box.clone()` reaches the Box impl rather than the inner T's. A `&` is
        // always peeled (no `impl for &T`).
        auto boxHasMethod = [&](const std::string& m) {
            auto it = methodImplLookup_.find("Box");
            return it != methodImplLookup_.end() && it->second.count(m) > 0;
        };
        while (r->kind == TypeKind::Ref ||
               (r->kind == TypeKind::Box && !boxHasMethod(mc.methodName)))
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
        else if (r->kind == TypeKind::Char) typeName = "char"; // v27 P149
        else if (r->kind == TypeKind::Box) typeName = "Box";    // Phase 51
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
        // Phase 51: a Box keeps its element in `refInner`, not `typeArgs`, so
        // surface it as the single receiver type arg for monomorphizing the
        // generic `impl<T> .. for Box<T>` method.
        res.receiverTypeArgs =
            r->kind == TypeKind::Box ? std::vector<TypePtr>{r->refInner}
                                     : r->typeArgs;
        res.selfKind = instSig->args.empty()
                           ? ResolvedMethod::SelfKind::ByValue
                           : selfKindFromSlot(instSig->args[0]);
        methodResolutions_[&mc] = std::move(res);
        // Review fix (B2): a bare `b.clone()` on a const-generic struct returns
        // a type still mentioning the struct's SYMBOLIC const params (`Buf<T,
        // CAP>`); pin them to the receiver's CONCRETE const args so the result
        // mangles to `Buf__..._c2` (else a symbolic CAP mangles to c0 and the
        // call result type-confuses). The explicit-annotation path already
        // lands on the concrete type; this makes the inferred path agree.
        TypePtr retTy = instSig->ret;
        {
            TypePtr rr = resolve(recvT);
            while (rr->kind == TypeKind::Ref) rr = resolve(rr->refInner);
            const std::vector<std::string>* cpn = nullptr;
            if (rr->kind == TypeKind::Struct) {
                if (auto s = structSchemas_.find(rr->structName);
                    s != structSchemas_.end())
                    cpn = &s->second.constParamNames;
            } else if (rr->kind == TypeKind::Enum) {
                if (auto e = enumSchemas_.find(rr->enumName);
                    e != enumSchemas_.end())
                    cpn = &e->second.constParamNames;
            }
            if (cpn) {
                std::unordered_map<std::string, std::size_t> cs;
                for (std::size_t i = 0;
                     i < cpn->size() && i < rr->typeArgs.size(); ++i) {
                    if ((*cpn)[i].empty()) continue;
                    TypePtr a = resolve(rr->typeArgs[i]);
                    if (a->isConstValue && a->constValueName.empty() &&
                        a->constValue >= 0)
                        cs[(*cpn)[i]] = static_cast<std::size_t>(a->constValue);
                }
                if (!cs.empty()) retTy = substituteConstLengths(retTy, cs);
            }
        }
        return retTy;
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
        // Review fix (Phase 60): a BOUNDED-GENERIC method call (`<T: Trait>` +
        // `t.method()`) has no concrete impl to mangle, so attribute the TRAIT
        // method's declared effects here — exactly like checkDynMethodCall —
        // else a generic caller of an io/alloc trait method contributes ZERO
        // effects (the subset-rule's whole soundness floor). The Concrete case
        // already unions the real impl effects via methodResolutions_ in
        // collectEffects, so only do this for the bounded-generic case.
        if (!concrete) {
            exprEffects_[&mc] =
                buildEffectSet(sig.effects, {}, mc.methodName, nullptr);
        }
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
        // Phase 49: bind the trait's generic params to the trait object's
        // concrete args (`dyn Producer<i64>` => T -> i64), so a method whose
        // signature names a trait param resolves to the concrete type. The
        // args live on the Dyn type (its `typeArgs`); peel the receiver's
        // pointer layers to reach it.
        {
            TypePtr p = resolve(dynRecvTy);
            while (p->kind == TypeKind::Ref || p->kind == TypeKind::Box)
                p = resolve(p->refInner);
            auto pit = traitGenericParams_.find(traitName);
            if (p->kind == TypeKind::Dyn && pit != traitGenericParams_.end()) {
                for (std::size_t i = 0;
                     i < pit->second.size() && i < p->typeArgs.size(); ++i)
                    selfEnv[pit->second[i]] = p->typeArgs[i];
            }
        }
        const GenericEnv* savedEnv = currentGenericEnv_;
        currentGenericEnv_ = &selfEnv;
        std::vector<TypePtr> paramTypes;
        for (const auto& p : sig->params) {
            paramTypes.push_back(resolveTypeRef(p.type));
        }
        TypePtr retTy = resolveTypeRef(sig->returnType);
        currentGenericEnv_ = savedEnv;

        // Phase-7-fix: a dynamic call has no concrete impl schema to mangle, so
        // attribute the TRAIT method's DECLARED effects (the object's contract)
        // to this site — collectEffects reads exprEffects_ for method calls.
        // (Conservative upper bound: an impl may not exceed its trait's
        // declared effects without that being the documented relaxation.)
        exprEffects_[&mc] = buildEffectSet(sig->effects, {}, mc.methodName,
                                           nullptr);

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
        const StructSchema& schema = it->second;
        bool hasConst = false;
        for (const auto& nm : schema.constParamNames)
            if (!nm.empty()) { hasConst = true; break; }

        // Phase 58: a const-generic struct literal infers each `const N`
        // parameter from the dimensions of the field that carries `[..; N]`
        // (so `Mat { data: [1,2,3] }` is a `Mat<3>` with no annotation), then
        // checks the fields against the now-concrete instance. Type params
        // (mixed `Foo<T, const N>`) keep the fresh-Var-and-unify treatment.
        if (hasConst) {
            std::unordered_map<std::string, TypePtr> valTypes;
            valTypes.reserve(sl.fields.size());
            for (const auto& f : sl.fields)
                valTypes[f.first] = checkExpr(*f.second);

            std::unordered_map<std::string, std::size_t> constSubst;
            std::unordered_map<std::string, std::string> constSym;
            for (const auto& [fname, fty] : schema.type->structFields) {
                auto vit = valTypes.find(fname);
                if (vit != valTypes.end())
                    solveConstLengths(fty, vit->second, constSubst, constSym);
            }
            // v28 Phase 153/154: adopt const-generic args from the expected
            // type (an annotation) for params not inferable from a field.
            if (currentExpectedType_) {
                TypePtr et = resolve(currentExpectedType_);
                if (et->kind == TypeKind::Struct &&
                    et->structName == sl.structName &&
                    et->typeArgs.size() >= schema.constParamNames.size()) {
                    for (std::size_t i = 0; i < schema.constParamNames.size();
                         ++i) {
                        const std::string& pn = schema.constParamNames[i];
                        if (pn.empty() || constSubst.count(pn) ||
                            constSym.count(pn))
                            continue;
                        TypePtr a = resolve(et->typeArgs[i]);
                        if (a->kind == TypeKind::Int && a->isConstValue)
                            constSubst[pn] =
                                static_cast<std::size_t>(a->constValue);
                    }
                }
            }
            for (const auto& nm : schema.constParamNames) {
                if (!nm.empty() && !constSubst.count(nm) && !constSym.count(nm)) {
                    error("cannot infer const parameter '" + nm +
                              "' of struct '" + sl.structName +
                              "' from the literal; a field typed `[..; " + nm +
                              "]` must be given a fixed-size array value",
                          sl.line, sl.column);
                    constSubst.emplace(nm, 0); // continue with a placeholder
                }
            }

            // Phase 61: a const param inferred SYMBOLICALLY (the field value's
            // array length is itself a const param in scope) yields a symbolic
            // const arg; a concrete one yields its value.
            std::vector<TypePtr> args;
            args.reserve(schema.genericVars.size());
            for (std::size_t i = 0; i < schema.genericVars.size(); ++i) {
                bool isConstSlot = i < schema.constParamNames.size() &&
                                   !schema.constParamNames[i].empty();
                if (isConstSlot) {
                    const std::string& pn = schema.constParamNames[i];
                    if (auto sy = constSym.find(pn); sy != constSym.end())
                        args.push_back(makeConstSymbol(sy->second));
                    else
                        args.push_back(makeConstValue(
                            static_cast<long long>(constSubst[pn])));
                } else {
                    args.push_back(makeFreshVar());
                }
            }
            TypePtr inst = instantiateGeneric(schema.type, schema.genericVars,
                                              schema.constParamNames,
                                              std::move(args), /*isStruct=*/true);
            // Re-unify field values against the materialized fields to solve the
            // fresh type-param Vars (instantiateGeneric used those Vars).
            validateStructLitFields(sl, inst, &valTypes);
            return inst;
        }

        // For generic structs, build a fresh instantiation so field-type
        // unification with each literal expr leaves the instance's
        // typeArgs in a fully-solved state.
        TypePtr instType = freshInstantiateStruct(schema);
        validateStructLitFields(sl, instType, nullptr);
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
        // Phase 33/124: `&&` and `||` are the boolean binary ops — both
        // operands and the result are bool (short-circuit; codegen only
        // evaluates rhs when lhs doesn't already settle the result).
        if (bin.op == ast::BinOp::And || bin.op == ast::BinOp::Or) {
            const char* sp = (bin.op == ast::BinOp::And) ? "&&" : "||";
            if (!unify(lhs, makeBool())) {
                error(std::string("logical `") + sp + "` expects bool on lhs, got " +
                          typeToString(lhs),
                      bin.lhs->line, bin.lhs->column);
            }
            if (!unify(rhs, makeBool())) {
                error(std::string("logical `") + sp + "` expects bool on rhs, got " +
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
        // Phase 66: integer bitwise operators (& | ^ << >>). Integer-only (any
        // width/signedness); like arithmetic they return the operand int type,
        // but they are NOT defined for f64.
        const bool isBitwise = (bin.op == ast::BinOp::BitAnd) ||
                               (bin.op == ast::BinOp::BitOr) ||
                               (bin.op == ast::BinOp::BitXor) ||
                               (bin.op == ast::BinOp::Shl) ||
                               (bin.op == ast::BinOp::Shr);
        const char* what =
            isComparison ? "comparison" : isBitwise ? "bitwise" : "arithmetic";
        // v27 Phase 147: `char` supports equality + ordering (by codepoint),
        // returning bool — but NO arithmetic / bitwise (like Rust). Both sides
        // must be `char`.
        if (resolve(lhs)->kind == TypeKind::Char ||
            resolve(rhs)->kind == TypeKind::Char) {
            if (!isComparison) {
                error(std::string(what) +
                          " operators are not defined for `char` (cast to an "
                          "integer with `as` first)",
                      bin.line, bin.column);
                return makeChar();
            }
            if (!unify(lhs, rhs) || resolve(lhs)->kind != TypeKind::Char) {
                error("`char` comparison requires both operands to be `char`, "
                      "got " + typeToString(lhs) + " and " + typeToString(rhs),
                      bin.line, bin.column);
            }
            return makeBool();
        }
        // Phase 39/67: float arithmetic / comparison. If a side is a float,
        // both sides must be the SAME float width (f32 or f64) — there is NO
        // implicit i64<->float or f32<->f64 coercion (use `as`). `%` and the
        // bitwise ops are integer-only. An unsuffixed f64-default float literal
        // narrows to the other operand's width (`x + 1.0` with `x: f32`).
        if (resolve(lhs)->kind == TypeKind::Float ||
            resolve(rhs)->kind == TypeKind::Float) {
            if (bin.op == ast::BinOp::Mod) {
                error("`%` (modulo) is not defined for floats", bin.line,
                      bin.column);
            }
            if (isBitwise) {
                error("bitwise operators (& | ^ << >>) are not defined for "
                      "floats",
                      bin.line, bin.column);
            }
            TypePtr rl = resolve(lhs), rr = resolve(rhs);
            // Narrow an f64-default float literal to the other side's f32.
            if (rl->kind == TypeKind::Float && rl->floatWidth == 32 &&
                rr->kind == TypeKind::Float && rr->floatWidth == 64 &&
                narrowFloatLiteral(*bin.rhs, rl))
                rhs = rl;
            else if (rr->kind == TypeKind::Float && rr->floatWidth == 32 &&
                     rl->kind == TypeKind::Float && rl->floatWidth == 64 &&
                     narrowFloatLiteral(*bin.lhs, rr))
                lhs = rr;
            if (!unify(lhs, rhs)) {
                error(std::string(what) +
                          " op requires both operands to be the same float "
                          "type, got " + typeToString(lhs) + " and " +
                          typeToString(rhs),
                      bin.line, bin.column);
                return isComparison ? makeBool() : resolve(lhs);
            }
            return isComparison ? makeBool() : resolve(lhs);
        }
        // v34 Phase 184: operator overloading. For an arithmetic op `+ - * /`
        // whose LHS is a user STRUCT/ENUM, desugar to the matching operator
        // trait's method (`Add::add` / `Sub::sub` / `Mul::mul` / `Div::div`)
        // resolved for that type. Homogeneous: the RHS must be the same type;
        // the result is that type. Records the impl method's mangled name so
        // codegen emits the call instead of LLVM arithmetic.
        {
            TypePtr rl = resolve(lhs);
            const char* opMethod = nullptr;
            const char* opTrait = nullptr;
            switch (bin.op) {
                case ast::BinOp::Add: opTrait = "Add"; opMethod = "add"; break;
                case ast::BinOp::Sub: opTrait = "Sub"; opMethod = "sub"; break;
                case ast::BinOp::Mul: opTrait = "Mul"; opMethod = "mul"; break;
                case ast::BinOp::Div: opTrait = "Div"; opMethod = "div"; break;
                default: break;
            }
            if (opMethod &&
                (rl->kind == TypeKind::Struct || rl->kind == TypeKind::Enum)) {
                std::string typeName = rl->kind == TypeKind::Struct
                                           ? rl->structName
                                           : rl->enumName;
                auto tIt = methodImplLookup_.find(typeName);
                if (tIt != methodImplLookup_.end()) {
                    auto mIt = tIt->second.find(opMethod);
                    if (mIt != tIt->second.end() &&
                        mIt->second.first == opTrait && mIt->second.second) {
                        const ast::FnDecl* mfn = mIt->second.second;
                        // RHS must be the same (Self) type.
                        if (!coerceOrUnify(*bin.rhs, rhs, lhs))
                            error(std::string("operator `") +
                                      (opMethod[0] == 'a' ? "+" :
                                       opMethod[0] == 's' ? "-" :
                                       opMethod[0] == 'm' ? "*" : "/") +
                                      "` on `" + typeName +
                                      "` requires the right operand to be `" +
                                      typeName + "`, got " + typeToString(rhs),
                                  bin.rhs->line, bin.rhs->column);
                        auto mangIt = implMethodMangled_.find(mfn);
                        if (mangIt != implMethodMangled_.end())
                            binOpMethod_[&bin] = mangIt->second;
                        // Attribute the operator method's declared effects.
                        auto sIt = fnSchemas_.find(
                            mangIt != implMethodMangled_.end() ? mangIt->second
                                                               : std::string());
                        if (sIt != fnSchemas_.end())
                            exprEffects_[&bin] = sIt->second.declaredEffects;
                        return lhs; // result is Self
                    }
                }
                error(std::string("operator is not defined for `") + typeName +
                          "` — implement `" + opTrait + "` for it (`impl " +
                          opTrait + " for " + typeName + " { fn " + opMethod +
                          "(self, rhs: Self) -> Self { … } }`)",
                      bin.line, bin.column);
                return lhs;
            }
        }
        // v11: integer arithmetic/comparison over the sized-int tower. Both
        // operands must be the SAME int type; an i64 LITERAL operand narrows to
        // the other side's concrete width (`x + 1` with `x: i32` -> 1 is i32).
        // Two default operands stay i64. No mixed-width arithmetic.
        {
            TypePtr rl = resolve(lhs), rr = resolve(rhs);
            bool lLit = dynamic_cast<const ast::IntLitExpr*>(bin.lhs.get());
            bool rLit = dynamic_cast<const ast::IntLitExpr*>(bin.rhs.get());
            auto concreteNonDefault = [](const TypePtr& t) {
                return t->kind == TypeKind::Int && !t->isConstValue &&
                       !(t->intWidth == 64 && t->intSigned);
            };
            if (concreteNonDefault(rl) && rLit && narrowIntLiteral(*bin.rhs, rl))
                rhs = rl;
            else if (concreteNonDefault(rr) && lLit &&
                     narrowIntLiteral(*bin.lhs, rr))
                lhs = rr;
        }
        if (!unify(lhs, rhs)) {
            error(std::string(what) +
                      " op requires both operands to be the same integer "
                      "type, got " + typeToString(lhs) + " and " +
                      typeToString(rhs),
                  bin.line, bin.column);
            return isComparison ? makeBool() : makeInt();
        }
        TypePtr rl = resolve(lhs);
        if (rl->kind == TypeKind::Var) {
            // Both sides were vars (two literals, or generic params): pin i64.
            unify(lhs, makeInt());
            rl = resolve(lhs);
        }
        if (rl->kind != TypeKind::Int || rl->isConstValue) {
            error(std::string(what) + " op expects integer operands, got " +
                      typeToString(lhs),
                  bin.lhs->line, bin.lhs->column);
            return isComparison ? makeBool() : makeInt();
        }
        return isComparison ? makeBool() : rl;
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
    // v14 (channel footgun): does `name` appear as a BARE (by-value) use
    // anywhere in `e` — i.e. used in some way OTHER than `&name`? For a non-Copy
    // `Sender`, a bare use is a MOVE, and a move is the ONLY way the Sender can
    // leave a closure's heap env (which never drops its captures). So a captured
    // Sender with NO bare use is captured-and-kept: it is never dropped, the
    // channel never closes, and a `recv`-until-`None` consumer hangs. We reject
    // that at compile time. Errs toward ALLOWING (returns true for any node type
    // it doesn't model precisely), so it can never reject a sound program — it
    // only catches the clear footgun (every use is `&name`).
    bool hasBareIdentUse(const ast::Expr& e, const std::string& name) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e))
            return id->name == name;
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            // `&name` is a by-reference use, not a move. But `&(expr...)` over a
            // compound operand may still contain a bare use deeper inside.
            if (dynamic_cast<const ast::IdentExpr*>(re->operand.get()))
                return false; // `&name` (by-ref) or `&other` — no bare use
            return hasBareIdentUse(*re->operand, name);
        }
        auto any = [&](const ast::Expr* p) {
            return p && hasBareIdentUse(*p, name);
        };
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e))
            return any(b->lhs.get()) || any(b->rhs.get());
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
            return any(u->operand.get());
        if (auto* c = dynamic_cast<const ast::CastExpr*>(&e))
            return any(c->operand.get());
        if (auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : c->args) if (any(a.get())) return true;
            return false;
        }
        if (auto* c = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            if (any(c->callee.get())) return true;
            for (const auto& a : c->args) if (any(a.get())) return true;
            return false;
        }
        if (auto* m = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            if (any(m->receiver.get())) return true;
            for (const auto& a : m->args) if (any(a.get())) return true;
            return false;
        }
        if (auto* i = dynamic_cast<const ast::IfExpr*>(&e))
            return any(i->cond.get()) || any(i->thenBranch.get()) ||
                   any(i->elseBranch.get());
        if (auto* bl = dynamic_cast<const ast::BlockExpr*>(&e)) {
            for (const auto& s : bl->stmts) {
                if (auto* l = dynamic_cast<const ast::LetStmt*>(s.get())) {
                    if (any(l->value.get())) return true;
                } else if (auto* r =
                               dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                    if (r->value && any(r->value.get())) return true;
                } else if (auto* a =
                               dynamic_cast<const ast::AssignStmt*>(s.get())) {
                    if (any(a->value.get())) return true;
                } else if (auto* x =
                               dynamic_cast<const ast::ExprStmt*>(s.get())) {
                    if (any(x->expr.get())) return true;
                }
            }
            return bl->tail && any(bl->tail.get());
        }
        if (auto* m = dynamic_cast<const ast::MatchExpr*>(&e)) {
            if (any(m->scrutinee.get())) return true;
            for (const auto& arm : m->arms)
                if (any(arm.body.get())) return true;
            return false;
        }
        if (auto* w = dynamic_cast<const ast::WhileExpr*>(&e))
            return any(w->cond.get()) || any(w->body.get());
        if (auto* l = dynamic_cast<const ast::LoopExpr*>(&e))
            return any(l->body.get());
        if (auto* f = dynamic_cast<const ast::ForExpr*>(&e))
            return any(f->iter.get()) || any(f->body.get());
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields)
                if (any(v.get())) return true;
            return false;
        }
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            for (const auto& el : al->elements) if (any(el.get())) return true;
            return false;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            for (const auto& el : tl->elements) if (any(el.get())) return true;
            return false;
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e))
            return be->value && any(be->value.get());
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e))
            return any(te->operand.get());
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e))
            return any(ae->operand.get());
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e))
            return any(ix->object.get()) || any(ix->index.get());
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e))
            return any(fe->object.get());
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e))
            return any(tf->object.get());
        if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e))
            return any(rg->start.get()) || any(rg->end.get());
        if (auto* sl = dynamic_cast<const ast::SliceExpr*>(&e))
            return any(sl->operand.get()) || any(sl->start.get()) ||
                   any(sl->end.get());
        if (auto* nc = dynamic_cast<const ast::ClosureExpr*>(&e))
            return any(nc->body.get()); // a nested closure may move it out
        // Leaves (IntLit / StringLit / Continue / …) reference no binding, so
        // they contribute no bare use. Every COMPOUND node that can hold a
        // `Sender` ident is modeled above; default false. (If a sound move-out
        // ever hid in an unmodeled node this would over-reject — the full v13
        // channel suite guards against that.)
        return false;
    }

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
        if (auto* ce = dynamic_cast<const ast::CastExpr*>(&e)) {
            collectFreeVars(*ce->operand, bound, order, seen);
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
        if (auto* ce = dynamic_cast<const ast::CastExpr*>(&e))
            return bodyMutatesCapture(*ce->operand, name, bound);
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

    // v31 Phase 167: the first BY-VALUE capture of a closure whose type is not
    // `Send` (the value is moved into the spawned thread, so it must be Send).
    // Today every by-value-capturable type (scalars, the Mutex / Sender
    // handles) is already Send, so this is the explicit Send FLOOR at the spawn
    // boundary rather than a new restriction — it becomes load-bearing when
    // richer captures land (scoped threads). By-ref captures are handled
    // separately (closureByRefCaptureName) and rejected outright.
    std::string closureNonSendCaptureName(const ast::Expr& e) {
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            for (const auto& cap : cl->captures)
                if (!cap.byRef && cap.type && !isSend(cap.type))
                    return cap.name;
        }
        return "";
    }

    // v32 Phase 176: `perform E::op(args)` — type-check against the declared op
    // signature, contribute effect `E`, and yield the op's return type.
    TypePtr checkPerform(const ast::PerformExpr& pe) {
        auto eff = effectOpDecls_.find(pe.effectName);
        if (eff == effectOpDecls_.end()) {
            error("unknown effect `" + pe.effectName +
                      "` (declare it with `effect " + pe.effectName + " { … }`)",
                  pe.line, pe.column);
            for (const auto& a : pe.args) checkExpr(*a);
            return makeInt();
        }
        auto opIt = eff->second.find(pe.opName);
        if (opIt == eff->second.end() || !opIt->second) {
            error("effect `" + pe.effectName + "` has no operation `" +
                      pe.opName + "`",
                  pe.line, pe.column);
            for (const auto& a : pe.args) checkExpr(*a);
            return makeInt();
        }
        const ast::EffectOp& op = *opIt->second;
        std::vector<TypePtr> paramTypes;
        paramTypes.reserve(op.params.size());
        for (const auto& p : op.params)
            paramTypes.push_back(resolveTypeRef(p.type));
        TypePtr ret = resolveTypeRef(op.returnType);
        if (pe.args.size() != paramTypes.size())
            error("`perform " + pe.effectName + "::" + pe.opName +
                      "` expects " + std::to_string(paramTypes.size()) +
                      " arg(s), got " + std::to_string(pe.args.size()),
                  pe.line, pe.column);
        std::size_t n = std::min(pe.args.size(), paramTypes.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr at = checkExpr(*pe.args[i]);
            if (!coerceOrUnify(*pe.args[i], at, paramTypes[i]))
                error("argument " + std::to_string(i + 1) + " to `perform " +
                          pe.effectName + "::" + pe.opName + "` has type " +
                          typeToString(at) + ", expected " +
                          typeToString(paramTypes[i]),
                      pe.args[i]->line, pe.args[i]->column);
        }
        // Record this site's effect contribution (E + the op's own effects).
        EffectSet contrib;
        contrib.add(pe.effectName);
        for (const auto& l : op.effects.labels) contrib.add(l);
        exprEffects_[&pe] = std::move(contrib);
        return ret;
    }

    // v32 Phase 176: `handle { body } with E { op(p) => hbody, … }`. Each arm is
    // a closure desugared at parse time; check it against the op's signature
    // (params + return). The handle's value is the body's value. (Effect E is
    // discharged from the body in collectEffects, not here.)
    TypePtr checkHandle(const ast::HandleExpr& he) {
        auto eff = effectOpDecls_.find(he.effectName);
        if (eff == effectOpDecls_.end()) {
            error("unknown effect `" + he.effectName +
                      "` in `handle … with`",
                  he.line, he.column);
        }
        for (const auto& arm : he.arms) {
            if (!arm.handler) continue;
            const ast::EffectOp* op = nullptr;
            if (eff != effectOpDecls_.end()) {
                auto opIt = eff->second.find(arm.opName);
                if (opIt != eff->second.end()) op = opIt->second;
            }
            if (!op) {
                if (eff != effectOpDecls_.end())
                    error("effect `" + he.effectName + "` has no operation `" +
                              arm.opName + "`",
                          arm.line, arm.column);
                checkExpr(*arm.handler); // still check the body
                continue;
            }
            std::vector<TypePtr> paramTypes;
            paramTypes.reserve(op->params.size());
            for (const auto& p : op->params)
                paramTypes.push_back(resolveTypeRef(p.type));
            TypePtr ret = resolveTypeRef(op->returnType);
            if (arm.handler->params.size() != paramTypes.size())
                error("handler for `" + he.effectName + "::" + arm.opName +
                          "` takes " +
                          std::to_string(arm.handler->params.size()) +
                          " param(s), but the operation declares " +
                          std::to_string(paramTypes.size()),
                      arm.line, arm.column);
            // Drive closure-param inference from the op's signature, then
            // require the handler body's type to match the op's return type.
            expectedArgType_ = makeFunction(paramTypes, ret);
            TypePtr clTy = resolve(checkExpr(*arm.handler));
            if (clTy->kind == TypeKind::Function &&
                !unify(clTy->ret, ret))
                error("handler for `" + he.effectName + "::" + arm.opName +
                          "` returns " + typeToString(clTy->ret) +
                          ", but the operation returns " + typeToString(ret),
                      arm.line, arm.column);
        }
        // The handle expression evaluates to its body's value.
        return checkExpr(*he.body);
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
        // Phase 145: classify the closure as Fn/FnMut/FnOnce. Start at Fn (0);
        // a by-ref (mutated) capture lifts it to FnMut (1); a consumed/moved-out
        // non-Copy capture (a captured `Sender`, which must be moved into the
        // spawned worker) lifts it to FnOnce (2). The kind is the max over all
        // captures.
        int kindRank = 0;
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
                            rt->kind == TypeKind::Char || // v27 Phase 147
                            rt->kind == TypeKind::Unit;
            // Phase 123: a `Mutex<T>` is a true Copy i64 handle (the lock+cell
            // block is process-lifetime, never freed), so capturing it by value
            // just copies the handle — both the closure and the enclosing
            // binding share the one lock + cell (exactly the cross-thread
            // sharing mechanism, like the pre-123 raw-i64 Mutex handle). No
            // clone, no drop. Its cell T is Send-gated at mutex_new, so the
            // captured value can't smuggle a non-Send across the boundary.
            if (rt->kind == TypeKind::Struct && rt->structName == "Mutex")
                copyable = true;
            // v31 Phase 168/169: a `RwLock<T>` and the atomic handles
            // (`AtomicI64`/`AtomicBool`) are Copy i64 handles over a
            // process-lifetime block, exactly like Mutex — capturing by value
            // copies the handle so N threads share the one lock / atomic cell.
            // (RwLock<T>'s cell is Send-gated at rwlock_new; atomics are Send.)
            if (rt->kind == TypeKind::Struct &&
                (rt->structName == "RwLock" || rt->structName == "AtomicI64" ||
                 rt->structName == "AtomicBool"))
                copyable = true;
            // Phase 81 (v13 review fix): a channel `Sender<T>` is Send, so it
            // may be captured into a producer thread's closure. It is no longer
            // Copy — capturing it CLONES it (codegen emits sender_clone when
            // building the env), so each producer thread gets its own refcounted
            // handle and the enclosing binding keeps its own. The captured clone
            // must be moved into the spawned worker by value (so the worker's
            // by-value `Sender` param drops it on that thread). A `Receiver<T>`
            // is NOT capturable — single-consumer / not-Send — and falls through
            // to the rejection below (a Receiver can't cross a thread boundary).
            if (rt->kind == TypeKind::Struct && rt->structName == "Sender") {
                copyable = true;
                // Phase 86 (v14): close the capture-and-keep footgun. The clone
                // this capture makes is owned by the closure's heap env, which
                // never drops its captures — so the ONLY way it gets dropped
                // (and the channel eventually closes) is being MOVED out of the
                // closure, i.e. a bare by-value use of `name` somewhere in the
                // body (e.g. `worker(.., tx)` or `chan_close(tx)`). If every use
                // is `&tx` (send-only, never moved/closed), the Sender leaks and
                // a `recv`-until-`None` consumer hangs forever. Reject it.
                if (!hasBareIdentUse(*cl.body, name)) {
                    error("closure captures the `Sender` `" + name +
                              "` but never moves it out (every use is `&" + name +
                              "`), so it is never dropped — the channel can't "
                              "close and a `chan_recv`-until-`None` consumer "
                              "hangs. Move it into the spawned worker by value "
                              "(e.g. `worker(.., " + name +
                              ")`) or `chan_close(" + name + ")` when done.",
                          cl.line, cl.column);
                }
            }
            // v31 Phase 171: an `Arc<T>`/`Weak<T>` captured into a thread closure
            // is CLONED (arc_clone/weak_clone — atomic count bump) so each
            // thread gets its own counted handle. The clone lives in the closure
            // env (which never drops captures), so it MUST be moved out into the
            // worker by value to be dropped (count released) on that thread —
            // else the refcount leaks. Same move-out rule as Sender.
            if (rt->kind == TypeKind::Struct &&
                (rt->structName == "Arc" || rt->structName == "Weak")) {
                copyable = true;
                if (!hasBareIdentUse(*cl.body, name)) {
                    error("closure captures the `" + rt->structName + "` `" +
                              name + "` but never moves it out (every use is `&" +
                              name + "`), so its atomic refcount is never "
                              "released. Move it into the spawned worker by value "
                              "(e.g. `worker(.., " + name + ")`).",
                          cl.line, cl.column);
                }
            }
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
            } else if (rt->kind == TypeKind::Struct &&
                       rt->structName == "Receiver") {
                // Phase 76 (v13): a channel Receiver is the SINGLE-CONSUMER
                // endpoint — it is not Send, so it can't be moved into another
                // thread (only the Sender crosses). This is what keeps the MPSC
                // contract sound; recv on the owning thread.
                error("cannot move a channel `Receiver` into a thread — it is "
                      "the single-consumer endpoint and is not Send; keep the "
                      "Receiver on the owning thread and move the Sender across "
                      "instead",
                      cl.line, cl.column);
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
            // Phase 145: fold this capture into the closure-kind rank. A
            // mutated capture ⇒ FnMut; a captured `Sender` (moved out into the
            // worker, hence consumed after one call) ⇒ FnOnce.
            if (rt->kind == TypeKind::Struct &&
                (rt->structName == "Sender" || rt->structName == "Arc" ||
                 rt->structName == "Weak"))
                kindRank = std::max(kindRank, 2);
            else if (mutated)
                kindRank = std::max(kindRank, 1);

            ast::ClosureCapture cap;
            cap.name = name;
            cap.type = t;
            // v32 Phase 176: a handler-arm closure captures EVERY free var by
            // reference (see ClosureExpr::forceCaptureByRef) so all arms share
            // the live handle-scope state (e.g. a State effect's cell).
            cap.byRef = mutated || cl.forceCaptureByRef;
            cl.captures.push_back(std::move(cap));
            captureTypes.emplace_back(name, t);
            captureMut.push_back(enclosingMut);
        }
        cl.kind = static_cast<ast::ClosureKind>(kindRank);

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
        // Phase 61: closure-param INFERENCE. Consume the expected fn-typed
        // parameter (set by the call-arg check) and unify each unannotated
        // param's fresh Var with the expected param type BEFORE checking the
        // body — so `vec_map(v, |x| ..)` infers `x: &i64` instead of letting
        // the body pin it to `i64` (which then fails to match `fn(&i64)`).
        {
            TypePtr expected = expectedArgType_;
            expectedArgType_ = nullptr; // consume; don't leak into the body
            if (expected) {
                TypePtr e = resolve(expected);
                if (e->kind == TypeKind::Function &&
                    e->args.size() == paramTypes.size()) {
                    for (std::size_t i = 0; i < paramTypes.size(); ++i)
                        if (!cl.params[i].hasAnnotation)
                            unify(paramTypes[i], e->args[i]);
                }
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
        // Phase 48: a qualified static call `Type::method(args)` — an
        // associated (no-self) trait method such as `P::default()`. Resolve it
        // against the named type's impl rather than a value receiver. Only
        // fires when `pathQualifier` names a type that actually has a method
        // `callee`; otherwise this is an ordinary (flat-merged) module path and
        // falls through to the bare-name lookup below.
        if (!call.pathQualifier.empty()) {
            auto tIt = methodImplLookup_.find(call.pathQualifier);
            if (tIt != methodImplLookup_.end()) {
                auto mIt = tIt->second.find(call.callee);
                if (mIt != tIt->second.end() && mIt->second.second) {
                    auto mangIt = implMethodMangled_.find(mIt->second.second);
                    if (mangIt != implMethodMangled_.end()) {
                        auto sIt = fnSchemas_.find(mangIt->second);
                        if (sIt != fnSchemas_.end()) {
                            const FnSchema& sch = sIt->second;
                            // Concrete impl: no generic vars to bind (generic
                            // static methods are deferred). Instantiate with an
                            // empty subst so any stray Var is freshened.
                            std::unordered_map<int, TypePtr> subst;
                            TypePtr instSig = instantiate(sch.signature, subst);
                            if (instSig->args.size() != call.args.size()) {
                                error("static method '" + call.pathQualifier +
                                          "::" + call.callee + "' expects " +
                                          std::to_string(instSig->args.size()) +
                                          " arg(s), got " +
                                          std::to_string(call.args.size()),
                                      call.line, call.column);
                            }
                            const std::size_t n = std::min(
                                instSig->args.size(), call.args.size());
                            for (std::size_t i = 0; i < n; ++i) {
                                TypePtr at = checkExpr(*call.args[i]);
                                if (!coerceOrUnify(*call.args[i], at,
                                                   instSig->args[i])) {
                                    error("argument " + std::to_string(i + 1) +
                                              " to '" + call.pathQualifier +
                                              "::" + call.callee +
                                              "' has type " + typeToString(at) +
                                              ", expected " +
                                              typeToString(instSig->args[i]),
                                          call.args[i]->line,
                                          call.args[i]->column);
                                }
                            }
                            // Attribute the method's effects to this call site.
                            exprEffects_[&call] = sch.declaredEffects;
                            staticCallMangled_[&call] = mangIt->second;
                            return resolve(instSig->ret);
                        }
                    }
                }
            }
            // Phase 52: a GENERIC static call `T::method()` — T a bounded type
            // param whose bound trait declares a static (no-self) method. The
            // concrete impl is chosen at monomorphization; here we resolve the
            // return type (Self -> T) and record (trait, method, varId).
            if (tIt == methodImplLookup_.end() && currentGenericEnv_) {
                auto git = currentGenericEnv_->find(call.pathQualifier);
                if (git != currentGenericEnv_->end()) {
                    TypePtr tv = resolve(git->second);
                    auto bit = tv->kind == TypeKind::Var
                                   ? currentVarAllBounds_.find(tv->varId)
                                   : currentVarAllBounds_.end();
                    if (tv->kind == TypeKind::Var &&
                        bit != currentVarAllBounds_.end()) {
                        for (const auto& traitName : bit->second) {
                            auto trIt = traits_.find(traitName);
                            if (trIt == traits_.end()) continue;
                            const ast::MethodSig* sig = nullptr;
                            for (const auto& m : trIt->second)
                                if (m.name == call.callee &&
                                    (m.params.empty() ||
                                     m.params[0].name != "self")) {
                                    sig = &m;
                                    break;
                                }
                            if (!sig) continue;
                            // Resolve param/return with Self -> T (keep the
                            // surrounding generic env so other names resolve).
                            GenericEnv selfEnv = *currentGenericEnv_;
                            selfEnv["Self"] = tv;
                            const GenericEnv* saved = currentGenericEnv_;
                            currentGenericEnv_ = &selfEnv;
                            std::vector<TypePtr> paramTypes;
                            for (const auto& p : sig->params)
                                paramTypes.push_back(resolveTypeRef(p.type));
                            TypePtr retTy = resolveTypeRef(sig->returnType);
                            currentGenericEnv_ = saved;
                            if (paramTypes.size() != call.args.size()) {
                                error("static method '" + call.pathQualifier +
                                          "::" + call.callee + "' expects " +
                                          std::to_string(paramTypes.size()) +
                                          " arg(s), got " +
                                          std::to_string(call.args.size()),
                                      call.line, call.column);
                            }
                            const std::size_t n = std::min(paramTypes.size(),
                                                           call.args.size());
                            for (std::size_t i = 0; i < n; ++i) {
                                TypePtr at = checkExpr(*call.args[i]);
                                if (!coerceOrUnify(*call.args[i], at,
                                                   paramTypes[i]))
                                    error("argument " + std::to_string(i + 1) +
                                              " to '" + call.pathQualifier +
                                              "::" + call.callee + "'",
                                          call.args[i]->line,
                                          call.args[i]->column);
                            }
                            exprEffects_[&call] = buildEffectSet(
                                sig->effects, {}, call.callee, nullptr);
                            staticCallGeneric_[&call] = {traitName, call.callee,
                                                         tv->varId};
                            return resolve(retTy);
                        }
                    }
                }
            }
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

            // Phase 59: a const-generic fn (`dot<const N>(a: [i64; N], ...)`)
            // infers each `const N` from the argument array dimensions. Uses
            // that disagree are a compile-time DIMENSION MISMATCH; the inferred
            // lengths are substituted into the signature (so ordinary arg
            // unification then validates them) and recorded as const-value
            // type-args so codegen monomorphizes per value.
            bool fnHasConst = false;
            for (const auto& nm : schema.constParamNames)
                if (!nm.empty()) { fnHasConst = true; break; }
            std::vector<TypePtr> preArgTypes;
            if (fnHasConst) {
                preArgTypes.reserve(call.args.size());
                for (const auto& a : call.args)
                    preArgTypes.push_back(checkExpr(*a));
                // Review fix (B5/M5): a callee const param can be bound from an
                // arg either to a CONCRETE length or to a CALLER-symbolic length
                // (forwarding `a: [i64; M]` into `dot<const N>(a: [i64; N])`).
                // Collect both; a param bound to two different concretes, two
                // different symbols, OR a concrete AND a symbol (unprovable
                // equal) is a dimension-mismatch error.
                std::unordered_map<std::string, std::size_t> constSubst;
                std::unordered_map<std::string, std::string> constSymSubst;
                auto dimErr = [&](const std::string& nm, const std::string& a,
                                  const std::string& b) {
                    error("const generic parameter '" + nm + "' of '" +
                              call.callee + "' is used with conflicting sizes " +
                              a + " and " + b + " (dimension mismatch)",
                          call.line, call.column);
                };
                for (std::size_t i = 0; i < n; ++i) {
                    std::unordered_map<std::string, std::size_t> local;
                    std::unordered_map<std::string, std::string> localSym;
                    solveConstLengths(instSig->args[i], preArgTypes[i], local,
                                      localSym);
                    for (const auto& [nm, val] : local) {
                        if (auto s = constSymSubst.find(nm);
                            s != constSymSubst.end())
                            dimErr(nm, s->second, std::to_string(val));
                        else if (auto it = constSubst.find(nm);
                                 it != constSubst.end()) {
                            if (it->second != val)
                                dimErr(nm, std::to_string(it->second),
                                       std::to_string(val));
                        } else
                            constSubst[nm] = val;
                    }
                    for (const auto& [nm, sym] : localSym) {
                        if (auto it = constSubst.find(nm);
                            it != constSubst.end())
                            dimErr(nm, std::to_string(it->second), sym);
                        else if (auto s = constSymSubst.find(nm);
                                 s != constSymSubst.end()) {
                            if (s->second != sym) dimErr(nm, s->second, sym);
                        } else
                            constSymSubst[nm] = sym;
                    }
                }
                for (std::size_t i = 0; i < schema.genericVars.size(); ++i) {
                    if (i < schema.constParamNames.size() &&
                        !schema.constParamNames[i].empty() &&
                        !constSubst.count(schema.constParamNames[i]) &&
                        !constSymSubst.count(schema.constParamNames[i])) {
                        error("cannot infer const generic parameter '" +
                                  schema.constParamNames[i] + "' of '" +
                                  call.callee +
                                  "' from the arguments (it must appear in an "
                                  "argument's array type)",
                              call.line, call.column);
                        constSubst[schema.constParamNames[i]] = 0;
                    }
                }
                instSig = substituteConstLengths(instSig, constSubst);
                if (!constSymSubst.empty())
                    instSig = renameConstLengths(instSig, constSymSubst);
                for (std::size_t i = 0; i < schema.genericVars.size() &&
                                        i < typeArgs.size(); ++i) {
                    if (i < schema.constParamNames.size() &&
                        !schema.constParamNames[i].empty()) {
                        const std::string& pn = schema.constParamNames[i];
                        if (auto s = constSymSubst.find(pn);
                            s != constSymSubst.end())
                            typeArgs[i] = makeConstSymbol(s->second);
                        else
                            typeArgs[i] = makeConstValue(
                                static_cast<long long>(constSubst[pn]));
                    }
                }
            }

            for (std::size_t i = 0; i < n; ++i) {
                TypePtr argType;
                if (fnHasConst) {
                    argType = preArgTypes[i];
                } else {
                    // Phase 61: closure-param inference — propagate the callee's
                    // expected fn-typed parameter into the closure so an
                    // unannotated `|x|` infers x's type (`vec_map(v, |x| ..)`
                    // needs no `|x: &i64|`). Earlier args (the Vec) have already
                    // solved the element type, so `instSig->args[i]` is concrete.
                    expectedArgType_ = resolve(instSig->args[i]);
                    argType = checkExpr(*call.args[i]);
                    expectedArgType_ = nullptr;
                }
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
                if (!fnHasConst) checkExpr(*call.args[i]);
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
                else if (concrete->kind == TypeKind::Char) typeName = "char"; // v27 P149
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
            // v31 Phase 170: scope_spawn carries the same Send-capture floor as
            // thread_spawn — its closure is arg[1] (arg[0] is `&Scope`).
            if ((call.callee == "thread_spawn" || call.callee == "scope_spawn") &&
                call.args.size() >
                    (call.callee == "scope_spawn" ? 1u : 0u)) {
                size_t ci = call.callee == "scope_spawn" ? 1 : 0;
                const char* which =
                    call.callee == "scope_spawn" ? "scope_spawn" : "thread_spawn";
                std::string offending =
                    closureByRefCaptureName(*call.args[ci]);
                if (!offending.empty()) {
                    error(std::string("cannot send a by-reference capture across "
                          "a thread boundary: closure passed to `") + which +
                              "` captures `" + offending +
                              "` by reference (FnMut), which would alias the "
                              "spawning frame's stack across threads (data race "
                              "+ use-after-free). Capture it by value (move a "
                              "Copy value, or share via a Mutex handle) instead",
                          call.args[ci]->line, call.args[ci]->column);
                }
                // v31 Phase 167: the Send FLOOR, made explicit at the spawn
                // boundary — every by-value capture is moved into the thread,
                // so its type must be `Send`. Goes through the marker oracle,
                // so an `impl !Send for T {}` makes capturing a T into a thread
                // a hard error (and an `impl Send for Opaque {}` permits it).
                std::string nonSend = closureNonSendCaptureName(*call.args[ci]);
                if (!nonSend.empty()) {
                    error(std::string("cannot move a non-`Send` value across a "
                          "thread boundary: closure passed to `") + which +
                              "` captures `" + nonSend +
                              "` by value, but its type is not `Send`. Only "
                              "`Send` data may cross a thread boundary",
                          call.args[ci]->line, call.args[ci]->column);
                }
            }
            // Phase 77 (v13): the value sent on a channel crosses a thread
            // boundary, so its type must be `Send` (the value-safety half of
            // the concurrency story). This rejects sending a `&T` borrow, a
            // Receiver, an Rc (Phase 78), or a struct that contains one — the
            // structural Send rule's teeth.
            if (call.callee == "chan_send" && call.args.size() == 2) {
                auto vit = exprTypes_.find(call.args[1].get());
                TypePtr vt =
                    vit != exprTypes_.end() ? resolve(vit->second) : nullptr;
                if (vt && vt->kind != TypeKind::Var && !isSend(vt)) {
                    error("cannot send a value of type " + typeToString(vt) +
                              " on a channel: it is not `Send` (it can't safely "
                              "cross a thread boundary). Send an owned value "
                              "(move it) — not a borrow / Receiver / Rc",
                          call.args[1]->line, call.args[1]->column);
                }
            }
            // Phase 123: the Mutex CELL type (the only entry point is mutex_new)
            // must be `Send` and must not be a shared handle. The i64 Mutex
            // handle is Copy + shareable across threads, so storing a non-Send
            // value would smuggle it across a thread boundary, bypassing the
            // chan_send / capture Send floors (the data-race hole on
            // Mutex<Rc<_>>). And Rc/Sender/Receiver are refcounted handles whose
            // deep clone is not a plain copy — a Mutex guards owned DATA; share
            // those handles via a channel, not a Mutex cell. Gating mutex_new
            // alone suffices: get/set/lock/unlock can only act on a Mutex<T>
            // that was already constructed here.
            if (call.callee == "mutex_new" && !typeArgs.empty()) {
                TypePtr cell = resolve(typeArgs[0]);
                size_t ln = call.args.empty() ? call.line : call.args[0]->line;
                size_t col =
                    call.args.empty() ? call.column : call.args[0]->column;
                bool handleTy = cell->kind == TypeKind::Struct &&
                                (cell->structName == "Rc" ||
                                 cell->structName == "Sender" ||
                                 cell->structName == "Receiver");
                if (handleTy) {
                    error("cannot store a `" + cell->structName +
                              "` in a Mutex: it is a shared handle, not owned "
                              "data — share it across threads via a channel "
                              "instead of a Mutex cell",
                          ln, col);
                } else if (cell->kind != TypeKind::Var && !isSend(cell)) {
                    error("cannot store a value of type " + typeToString(cell) +
                              " in a Mutex: it is not `Send`, and the Mutex "
                              "handle is shareable across threads (a non-Send "
                              "cell could be smuggled across a thread boundary). "
                              "Store an owned Send value — not a borrow / "
                              "Receiver / Rc",
                          ln, col);
                }
            }
            // v31 Phase 168: the RwLock cell is gated exactly like the Mutex
            // cell — a `RwLock<T>` is a shareable handle, so a non-Send / shared-
            // handle cell would be smuggled across a thread boundary.
            if (call.callee == "rwlock_new" && !typeArgs.empty()) {
                TypePtr cell = resolve(typeArgs[0]);
                size_t ln = call.args.empty() ? call.line : call.args[0]->line;
                size_t col =
                    call.args.empty() ? call.column : call.args[0]->column;
                bool handleTy = cell->kind == TypeKind::Struct &&
                                (cell->structName == "Rc" ||
                                 cell->structName == "Sender" ||
                                 cell->structName == "Receiver");
                if (handleTy) {
                    error("cannot store a `" + cell->structName +
                              "` in a RwLock: it is a shared handle, not owned "
                              "data — share it across threads via a channel "
                              "instead of a RwLock cell",
                          ln, col);
                } else if (cell->kind != TypeKind::Var && !isSend(cell)) {
                    error("cannot store a value of type " + typeToString(cell) +
                              " in a RwLock: it is not `Send`, and the RwLock "
                              "handle is shareable across threads. Store an owned "
                              "Send value — not a borrow / Receiver / Rc",
                          ln, col);
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
            // v32 Phase 172: a Future combinator's inner result type T becomes
            // the closure's parameter type, and `unit` lowers to `void`, which
            // cannot be a fn parameter. Reject a unit inner type (combine a
            // non-unit future). T is typeArgs[0].
            if ((call.callee == "future_map" || call.callee == "future_and_then") &&
                !typeArgs.empty() &&
                resolve(typeArgs[0])->kind == TypeKind::Unit) {
                error("`" + call.callee +
                          "`'s inner future result type cannot be unit (its "
                          "value is handed to the closure)",
                      call.line, call.column);
            }
            // v32 Phase 172: `join2` latches each sub-future's value into the
            // result tuple, so neither result type may be unit (`void` has no
            // storable value). A and B are typeArgs[0] / typeArgs[1].
            if (call.callee == "future_join2" && typeArgs.size() >= 2 &&
                (resolve(typeArgs[0])->kind == TypeKind::Unit ||
                 resolve(typeArgs[1])->kind == TypeKind::Unit)) {
                error("`future_join2` future result types cannot be unit (each value "
                      "is latched into the result tuple)",
                      call.line, call.column);
            }
            // v32 Phase 172: `select` carries the winning future's value in an
            // `Either` payload, so neither result type may be unit (`void` has
            // no storable payload). A and B are typeArgs[0] / typeArgs[1].
            if (call.callee == "future_select" && typeArgs.size() >= 2 &&
                (resolve(typeArgs[0])->kind == TypeKind::Unit ||
                 resolve(typeArgs[1])->kind == TypeKind::Unit)) {
                error("`future_select` future result types cannot be unit (the "
                      "winner's value is carried in an `Either` payload)",
                      call.line, call.column);
            }
            // v32 Phase 172: the closure handed to a Future combinator (`map` /
            // `and_then`) is stored in the heap combinator frame and called
            // LATER, when the future is polled — after this call has returned. A
            // by-reference (FnMut) capture aliases the caller's stack frame,
            // which is dead by poll time (use-after-free). Require a by-value
            // (`Fn`) closure, exactly like the thread_spawn Send floor. The
            // closure is arg[1] (arg[0] is the inner Future).
            if ((call.callee == "future_map" || call.callee == "future_and_then") &&
                call.args.size() > 1) {
                std::string offending = closureByRefCaptureName(*call.args[1]);
                if (!offending.empty()) {
                    error("closure passed to `" + call.callee + "` captures `" +
                              offending +
                              "` by reference (FnMut), but a Future "
                              "combinator's closure is stored and called later "
                              "(when the future is polled), after this call "
                              "returns — a by-ref capture would dangle. Capture "
                              "it by value (`Fn`) instead",
                          call.args[1]->line, call.args[1]->column);
                }
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
    // Phase 65: `operand as Type` — an explicit numeric cast. Both the source
    // and the target must be a primitive numeric type (an int of any
    // width/signedness, or f64); the result type is the target. This is the
    // only bridge across the non-coercive lattice — every other int-width or
    // int/float crossing is a type error. Casting from/to a non-numeric type
    // (a struct, bool, String, reference, ...) is rejected.
    TypePtr checkCast(const ast::CastExpr& ce) {
        TypePtr src = resolve(checkExpr(*ce.operand));
        TypePtr dst = resolve(resolveTypeRef(ce.targetType));
        auto isNumeric = [](const TypePtr& t) {
            return (t->kind == TypeKind::Int && !t->isConstValue) ||
                   t->kind == TypeKind::Float;
        };
        // v27 Phase 147: `char as <int>` reads the scalar's codepoint, and
        // `<int> as char` builds a char from a codepoint (the integer is
        // assumed in range — `char_from_u32` is the validating constructor).
        // No `char as f64` / `f64 as char`. char-to-char is identity.
        if (src->kind == TypeKind::Char || dst->kind == TypeKind::Char) {
            bool ok = (src->kind == TypeKind::Char && dst->kind == TypeKind::Char) ||
                      (src->kind == TypeKind::Char &&
                       dst->kind == TypeKind::Int && !dst->isConstValue) ||
                      (dst->kind == TypeKind::Char &&
                       src->kind == TypeKind::Int && !src->isConstValue);
            if (!ok) {
                error("`as` cast for `char` is only to/from an integer type, "
                      "got " + typeToString(src) + " as " + typeToString(dst),
                      ce.line, ce.column);
            }
            return dst;
        }
        // v33 Phase 177: raw-pointer `as` casts. The cast itself is SAFE (only a
        // raw *dereference* needs `unsafe`): create a raw pointer from a
        // reference (`&x as *const T`), reinterpret between raw pointers
        // (`*const T as *mut U`), and convert a raw pointer <-> an integer
        // address (`p as i64` / `addr as *mut T`).
        bool srcRaw = src->kind == TypeKind::Ref && src->isRawPtr;
        bool dstRaw = dst->kind == TypeKind::Ref && dst->isRawPtr;
        if (srcRaw || dstRaw) {
            bool srcRef = src->kind == TypeKind::Ref && !src->isRawPtr;
            bool srcInt = src->kind == TypeKind::Int && !src->isConstValue;
            bool dstInt = dst->kind == TypeKind::Int && !dst->isConstValue;
            bool ok = (srcRef && dstRaw) || (srcRaw && dstRaw) ||
                      (srcRaw && dstInt) || (srcInt && dstRaw);
            if (!ok)
                error("invalid raw-pointer `as` cast: " + typeToString(src) +
                          " as " + typeToString(dst) +
                          " (allowed: &T->*T, *T<->*U, *T<->int)",
                      ce.line, ce.column);
            return dst;
        }
        if (!isNumeric(src) || !isNumeric(dst)) {
            error("`as` cast is only allowed between numeric types (integers "
                  "and f64), got " +
                      typeToString(src) + " as " + typeToString(dst),
                  ce.line, ce.column);
            // Recover with the target if it is at least numeric, else i64.
            return isNumeric(dst) ? dst : makeInt();
        }
        return dst;
    }

    TypePtr checkUnary(const ast::UnaryExpr& un) {
        TypePtr operand = checkExpr(*un.operand);
        switch (un.op) {
        case ast::UnaryOp::Neg:
            // Phase 39/67: `-x` negates an i64 OR a float of either width.
            if (resolve(operand)->kind == TypeKind::Float)
                return resolve(operand);
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
        case ast::UnaryOp::BitNot: {
            // Phase 66: `~x` is the bitwise complement of an integer of any
            // width/signedness; the result is that same int type.
            TypePtr r = resolve(operand);
            if (r->kind != TypeKind::Int || r->isConstValue) {
                error("unary `~` requires an integer operand, got " +
                          typeToString(operand),
                      un.operand->line, un.operand->column);
                return makeInt();
            }
            return r;
        }
        case ast::UnaryOp::Deref: {
            // Phase 34: `*r` reads the pointee of a `&T` / `&mut T` / Box<T>.
            TypePtr r = resolve(operand);
            if (r->kind == TypeKind::Ref) {
                // v33 Phase 177: dereferencing a RAW pointer is unchecked — it
                // requires an `unsafe` block (a `&T`/`&mut T` deref does not).
                if (r->isRawPtr && unsafeDepth_ == 0)
                    error("dereferencing a raw pointer (`*const`/`*mut`) "
                          "requires an `unsafe` block",
                          un.operand->line, un.operand->column);
                return resolve(r->refInner);
            }
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
    // Phase 77 (v13): the structural `Send` predicate — the VALUE-SAFETY half
    // of the concurrency story (the `share` effect is the control half). A
    // value may cross a thread boundary (move into a thread closure / send on a
    // channel) only if its type is Send. Send is decided STRUCTURALLY on the
    // solved type at the boundary call site (sound regardless of where the
    // value later runs): scalars + String + the Mutex / Sender handles are
    // Send; owning aggregates are Send iff every part is; a `&T` / `&mut T`
    // borrow, the single-consumer Receiver, a non-atomic Rc (Phase 78), and a
    // closure / fn value (may capture by reference) are NOT Send. The MPSC
    // contract — only the Sender crosses, never the Receiver — and the
    // no-dangling-borrow guarantee both fall out of this.
    // v31 Phase 167: the marker oracle. +1 if an `impl Send/Sync for T {}`
    // grants membership, -1 if an `impl !Send/!Sync for T {}` opts out, 0 if
    // unspecified (the type gets the structural auto-derive). Consulted at the
    // TOP of the Struct/Enum arms of isSend/isSync so an explicit impl wins.
    int markerStatus(const std::string& tyName, const char* marker) const {
        auto it = markerImpls_.find(tyName);
        if (it == markerImpls_.end()) return 0;
        auto jt = it->second.find(marker);
        return jt == it->second.end() ? 0 : jt->second;
    }
    bool isSend(const TypePtr& t) {
        std::unordered_set<std::string> seen;
        return isSendImpl(t, seen);
    }
    bool isSendImpl(const TypePtr& t, std::unordered_set<std::string>& seen) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Bool:
        case TypeKind::Char: // v27 Copy scalar — Send (was a latent gap: fell
                             // through to default:false, wrongly rejecting
                             // chan_send(char) / Mutex<char>).
        case TypeKind::Unit:
            return true;
        case TypeKind::Box:
            return isSendImpl(r->refInner, seen);
        case TypeKind::Array:
            return isSendImpl(r->arrayElem, seen);
        case TypeKind::Tuple:
            for (const auto& el : r->tupleElems)
                if (!isSendImpl(el, seen)) return false;
            return true;
        case TypeKind::Struct: {
            // v31 Phase 167: an explicit `impl Send`/`impl !Send` overrides the
            // structural rule (the principled hook for opaque/handle types).
            if (int ms = markerStatus(r->structName, "Send")) return ms > 0;
            // String owns its bytes (Send); the channel Sender is Send. The
            // Receiver (single-consumer) and an Rc (non-atomic refcount) are NOT.
            if (r->structName == "String" || r->structName == "Sender")
                return true;
            // v31 Phase 169: an atomic handle IS the safe-sharing primitive —
            // Send (and Sync). The legible POSITIVE witness opposite Rc.
            if (r->structName == "AtomicI64" || r->structName == "AtomicBool")
                return true;
            // v31 Phase 171: `Arc<T>`/`Weak<T>` are atomically refcounted, so
            // (unlike Rc) they ARE Send when T is — specifically T: Send + Sync
            // (the Rust bound: a shared owner can both move T to another thread
            // and be aliased from several threads).
            if (r->structName == "Arc" || r->structName == "Weak") {
                for (const auto& a : r->typeArgs)
                    if (!isSendImpl(a, seen) || !isSync(a)) return false;
                return true;
            }
            if (r->structName == "Receiver" || r->structName == "Rc")
                return false;
            // v31 Phase 168: a lock GUARD is bound to the locking thread — never
            // Send (it must be released by the thread that took the lock).
            // v31 Phase 170: a `Scope` is bound to its defining thread (it joins
            // on drop there) — never Send.
            if (r->structName == "MutexGuard" ||
                r->structName == "RwLockReadGuard" ||
                r->structName == "RwLockWriteGuard" ||
                r->structName == "Scope")
                return false;
            // Containers + the Mutex/RwLock handle carry their element(s)/cell in
            // typeArgs, not structFields. A `Mutex<T>`/`RwLock<T>` is Send iff
            // its cell T is (the cell is also Send-gated at construction, so this
            // is belt-and-braces — sharing a Mutex<NonSend> across threads is
            // rejected).
            if (r->structName == "Vec" || r->structName == "HashMap" ||
                r->structName == "HashSet" || r->structName == "Mutex" ||
                r->structName == "RwLock") {
                for (const auto& a : r->typeArgs)
                    if (!isSendImpl(a, seen)) return false;
                return true;
            }
            // A user struct is Send iff every field is.
            if (!seen.insert(r->structName).second) return true; // recursive
            for (const auto& f : r->structFields)
                if (!isSendImpl(f.second, seen)) return false;
            return true;
        }
        case TypeKind::Enum: {
            if (int ms = markerStatus(r->enumName, "Send")) return ms > 0;
            if (!seen.insert(r->enumName).second) return true;
            for (const auto& v : r->enumVariants)
                for (const auto& p : v.payloadTypes)
                    if (!isSendImpl(p, seen)) return false;
            return true;
        }
        // A borrow can't cross a thread boundary in this MVP (its referent's
        // lifetime can't be proven to outlive the thread); a closure may
        // capture by reference; an unsolved Var / dyn is conservatively unsafe.
        case TypeKind::Ref:
        case TypeKind::Function:
        case TypeKind::Dyn:
        case TypeKind::Var:
        default:
            return false;
        }
    }

    // v31 Phase 167: `Sync` — a type is Sync iff it is safe to SHARE a `&T`
    // across threads. Decided structurally, mirroring isSend, with the
    // interior-mutability inversion for Mutex. Sync has no enforcement site in
    // this MVP (a `&T` cannot yet cross a thread — by-ref captures are rejected
    // outright at thread_spawn), so it carries no teeth until scoped threads
    // (a later v31 phase) let a borrow cross a thread scope; it is defined now
    // so that machinery can consult it. The marker oracle (`impl Sync` / `impl
    // !Sync`) overrides the structural answer, exactly like Send.
    bool isSync(const TypePtr& t) {
        std::unordered_set<std::string> seen;
        return isSyncImpl(t, seen);
    }
    bool isSyncImpl(const TypePtr& t, std::unordered_set<std::string>& seen) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Bool:
        case TypeKind::Char:
        case TypeKind::Unit:
            return true;
        case TypeKind::Box:
            return isSyncImpl(r->refInner, seen);
        // `&T` / `&mut T` is itself Sync iff the pointee is Sync (sharing the
        // reference is sharing the data).
        case TypeKind::Ref:
            return isSyncImpl(r->refInner, seen);
        case TypeKind::Array:
            return isSyncImpl(r->arrayElem, seen);
        case TypeKind::Tuple:
            for (const auto& el : r->tupleElems)
                if (!isSyncImpl(el, seen)) return false;
            return true;
        case TypeKind::Struct: {
            if (int ms = markerStatus(r->structName, "Sync")) return ms > 0;
            // String is immutable-once-shared (Sync). The Sender only appends
            // under the channel's own mutex, so concurrent &Sender use is race
            // free (Sync). The single-consumer Receiver and the non-atomic Rc
            // are NOT Sync.
            if (r->structName == "String" || r->structName == "Sender")
                return true;
            // v31 Phase 169: atomic handles are Sync (concurrent &Atomic access
            // is race-free — that is their purpose).
            if (r->structName == "AtomicI64" || r->structName == "AtomicBool")
                return true;
            // v31 Phase 171: Arc<T>/Weak<T> are Sync iff T is Send + Sync.
            if (r->structName == "Arc" || r->structName == "Weak") {
                for (const auto& a : r->typeArgs)
                    if (!isSendImpl(a, seen) || !isSyncImpl(a, seen))
                        return false;
                return true;
            }
            if (r->structName == "Receiver" || r->structName == "Rc")
                return false;
            // v31 Phase 168: a lock guard is thread-bound — not Sync.
            if (r->structName == "MutexGuard" ||
                r->structName == "RwLockReadGuard" ||
                r->structName == "RwLockWriteGuard")
                return false;
            // A Mutex<T> / RwLock<T> is Sync iff its cell T is SEND — the
            // interior-mutability inversion: the lock serialises access, so a
            // Send-but-not-Sync T becomes safely shareable behind the lock.
            if (r->structName == "Mutex" || r->structName == "RwLock") {
                for (const auto& a : r->typeArgs)
                    if (!isSendImpl(a, seen)) return false;
                return true;
            }
            if (r->structName == "Vec" || r->structName == "HashMap" ||
                r->structName == "HashSet") {
                for (const auto& a : r->typeArgs)
                    if (!isSyncImpl(a, seen)) return false;
                return true;
            }
            // A user struct is Sync iff every field is.
            if (!seen.insert(r->structName).second) return true; // recursive
            for (const auto& f : r->structFields)
                if (!isSyncImpl(f.second, seen)) return false;
            return true;
        }
        case TypeKind::Enum: {
            if (int ms = markerStatus(r->enumName, "Sync")) return ms > 0;
            if (!seen.insert(r->enumName).second) return true;
            for (const auto& v : r->enumVariants)
                for (const auto& p : v.payloadTypes)
                    if (!isSyncImpl(p, seen)) return false;
            return true;
        }
        // A closure (may hold by-ref captures), a dyn object, and an unsolved
        // Var are conservatively NOT Sync.
        case TypeKind::Function:
        case TypeKind::Dyn:
        case TypeKind::Var:
        default:
            return false;
        }
    }

    bool isCopyAggregateElem(const TypePtr& t) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Int:
        case TypeKind::Float: // Phase 39: f64 is Copy
        case TypeKind::Bool:
        case TypeKind::Char:  // v27 Phase 147: char is a Copy scalar
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
        // Phase 62: array-REPEAT `[value; count]`. The element type is the
        // value's type; the length is `count` — a const-generic param (a
        // symbolic length) or a compile-time const (a concrete length).
        if (al.repeatCount) {
            TypePtr elemTy = checkExpr(*al.elements[0]);
            // The value is evaluated once and broadcast to every slot, so a
            // non-Copy (owning) element would alias one heap value N times (a
            // later N-fold free). Restrict repeat to Copy elements.
            if (!isCopyAggregateElem(elemTy)) {
                error("array-repeat `[value; N]` requires a Copy element "
                      "(i64/bool/f64 or nested arrays of those), got " +
                          typeToString(elemTy) +
                          "; build a non-Copy array with an explicit element "
                          "list instead",
                      al.line, al.column);
            }
            if (auto* id =
                    dynamic_cast<const ast::IdentExpr*>(al.repeatCount.get());
                id && !lookupLocal(id->name) &&
                currentConstParams_.count(id->name)) {
                // Review fix: a LOCAL of the same name shadows the const param,
                // so only treat the length as a symbolic const param when no
                // local binds the name (else fall to const-eval below).
                TypePtr arr = makeArray(elemTy, 0);
                arr->arrayLenParam = id->name; // symbolic, per Phase 58/59
                return arr;
            }
            std::int64_t n = 0;
            if (!evalConstI64(*al.repeatCount, n) || n < 0) {
                error("array-repeat length must be a non-negative compile-time "
                      "constant or a const-generic param",
                      al.repeatCount->line, al.repeatCount->column);
                n = 0;
            }
            return makeArray(elemTy, static_cast<std::size_t>(n));
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
        // Phase 61 (v10): non-Copy element types (String, structs, Vec, Box)
        // are now allowed — codegen clones the array element-wise and drops it
        // element-wise (mirroring non-Copy tuples). A non-Copy-element array is
        // itself non-Copy (isCopyType recurses on the element), so the borrow
        // checker move-tracks it correctly.
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
        // Compile-time bounds check for a constant literal index. Phase 59:
        // skip it for a SYMBOLIC length `[T; N]` (N a const-generic param) —
        // the length isn't known until the fn is monomorphized, so the check
        // happens per instance / at runtime, not against the placeholder 0.
        if (objTy->arrayLenParam.empty()) {
            if (auto* lit =
                    dynamic_cast<const ast::IntLitExpr*>(ix.index.get())) {
                if (lit->value < 0 ||
                    static_cast<std::size_t>(lit->value) >= objTy->arrayLen) {
                    error("array index " + std::to_string(lit->value) +
                              " out of bounds for array of length " +
                              std::to_string(objTy->arrayLen),
                          ix.index->line, ix.index->column);
                }
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
        // v27 Phase 147: a char-literal pattern requires a `char` scrutinee.
        if (dynamic_cast<const ast::LitCharPat*>(&pat)) {
            if (!unify(expected, makeChar())) {
                error("char pattern requires a `char` scrutinee, got " +
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
            // v28 Phase 153/154: a scalar annotation is an expected-type hint
            // for the value (lets a struct literal adopt the annotation's
            // const-generic args). Restored right after the value is checked.
            TypePtr savedExpected = currentExpectedType_;
            if (let->annotation && let->tupleNames.empty())
                currentExpectedType_ = resolveTypeRef(*let->annotation);
            TypePtr valT = checkExpr(*let->value);
            currentExpectedType_ = savedExpected;
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
                // Phase 57: an optional `: (T, ...)` annotation pins each
                // element type (and is a coercion target per element), so a
                // multi-value generic call whose element types can't be
                // inferred is spell-able: `let (a, b): (T, T) = f()`.
                std::vector<TypePtr> elemTys = r->tupleElems;
                if (let->annotation) {
                    TypePtr annot = resolve(resolveTypeRef(*let->annotation));
                    if (annot->kind != TypeKind::Tuple ||
                        annot->tupleElems.size() != let->tupleNames.size()) {
                        error("tuple-`let` annotation must be a tuple type with "
                                  + std::to_string(let->tupleNames.size()) +
                                  " element(s), got " + typeToString(annot),
                              let->line, let->column);
                        return;
                    }
                    for (std::size_t i = 0; i < let->tupleNames.size(); ++i) {
                        if (!coerceOrUnify(*let->value, r->tupleElems[i],
                                           annot->tupleElems[i])) {
                            error("tuple-`let` element " + std::to_string(i + 1) +
                                      " has type " +
                                      typeToString(r->tupleElems[i]) +
                                      " but the annotation says " +
                                      typeToString(annot->tupleElems[i]),
                                  let->line, let->column);
                        }
                    }
                    elemTys = annot->tupleElems;
                }
                for (std::size_t i = 0; i < let->tupleNames.size(); ++i) {
                    if (let->tupleNames[i] == "_") continue;
                    scopes_.back()[let->tupleNames[i]] = elemTys[i];
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
