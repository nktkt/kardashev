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

        // Phase 6.1 built-in: `Future` — opaque handle returned by every
        // async fn. Codegen lays it out as { i64 state, i64 result }
        // (MVP only handles `async fn () -> i64`; richer wrapped types
        // wait for generic Future<T>). The struct stays nameable but
        // opaque to user dot-access.
        {
            StructSchema sch;
            sch.type = makeStruct("Future", {});
            structSchemas_["Future"] = std::move(sch);
        }

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
        // vec_len<T>(v: &Vec<T>) -> i64
        {
            FnSchema sch;
            sch.signature = makeFunction(
                {makeRef(vecFnInst, /*isMut=*/false)}, makeInt());
            sch.genericVars.push_back(vecFnGenericVar);
            fnSchemas_["vec_len"] = std::move(sch);
        }

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

        // Pass 1c: register trait declarations. Each trait gets a global
        // entry with its method signatures, used later to validate impl
        // blocks and to type-check method calls through bounded generic
        // params.
        for (const auto& td : program.traits) {
            if (traits_.count(td.name)) {
                error("trait redefined: " + td.name, td.line, td.column);
                continue;
            }
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
            auto traitIt = traits_.find(impl.traitName);
            if (traitIt == traits_.end()) {
                error("impl references unknown trait '" + impl.traitName + "'",
                      impl.line, impl.column);
                continue;
            }
            TypePtr forTy = resolveTypeRef(impl.forType);
            TypePtr rfor = resolve(forTy);
            std::string typeName;
            if (rfor->kind == TypeKind::Struct) typeName = rfor->structName;
            else if (rfor->kind == TypeKind::Enum) typeName = rfor->enumName;
            else if (rfor->kind == TypeKind::Int) typeName = "i64";
            else if (rfor->kind == TypeKind::Bool) typeName = "bool";
            else {
                error("impl for unsupported type " + typeToString(forTy),
                      impl.forType.line, impl.forType.column);
                continue;
            }
            ImplRegistration reg;
            reg.traitName = impl.traitName;
            reg.typeName = typeName;
            // Validate methods + collect.
            std::unordered_set<std::string> seenMethod;
            for (const auto& fn : impl.methods) {
                if (!seenMethod.insert(fn.name).second) {
                    error("duplicate method '" + fn.name + "' in impl",
                          fn.line, fn.column);
                    continue;
                }
                const ast::MethodSig* traitMethod = nullptr;
                for (const auto& m : traitIt->second) {
                    if (m.name == fn.name) { traitMethod = &m; break; }
                }
                if (!traitMethod) {
                    error("method '" + fn.name +
                              "' is not declared in trait '" + impl.traitName +
                              "'",
                          fn.line, fn.column);
                    continue;
                }
                reg.methods[fn.name] = &fn;
            }
            // Verify the impl covers every trait method (no missing impls).
            for (const auto& m : traitIt->second) {
                if (!reg.methods.count(m.name)) {
                    error("impl of '" + impl.traitName + "' for '" +
                              typeName +
                              "' is missing method '" + m.name + "'",
                          impl.line, impl.column);
                }
            }
            implMethodByType_[typeName][impl.traitName] = std::move(reg);
            // Also build a quick (typeName, methodName) -> impl-method lookup
            // for the method-call resolver.
            for (const auto& [mname, mfn] :
                 implMethodByType_[typeName][impl.traitName].methods) {
                methodImplLookup_[typeName][mname] =
                    {impl.traitName, mfn};
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
            std::vector<std::pair<std::string, TypePtr>> effectRowVars;
            genVars.reserve(fn.genericParams.size());
            genBounds.reserve(fn.genericParams.size());
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
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
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

            std::vector<TypePtr> argTypes;
            argTypes.reserve(fn.params.size());
            for (const auto& p : fn.params) {
                argTypes.push_back(resolveTypeRef(p.type));
            }
            TypePtr ret = resolveTypeRef(fn.returnType);

            currentGenericEnv_ = nullptr;
            currentEffectRowVarNames_ = nullptr;

            FnSchema schema;
            schema.signature = makeFunction(std::move(argTypes), ret);
            schema.genericVars = std::move(genVars);
            schema.genericBounds = std::move(genBounds);
            // Note: effect-row vars are deliberately NOT added to
            // genericVars — that list drives codegen monomorphization, and
            // effects are compile-time only (zero runtime cost). Row vars
            // are substituted separately at call sites via `effectRowVars`.
            schema.effectRowVars = std::move(effectRowVars);
            schema.declaredEffects = buildEffectSet(fn.effects,
                                                       fn.genericParams,
                                                       fn.name, &rowVarNames);
            // Phase 6.1: `async fn` wraps the declared return type in
            // the built-in `Future` struct. The body still emits the
            // inner type (which we stash on the schema for codegen);
            // callers see Future and must `.await` to unwrap.
            if (fn.isAsync) {
                schema.declaredEffects.add("async");
                schema.isAsync = true;
                schema.asyncInnerType = schema.signature->ret;
                auto futIt = structSchemas_.find("Future");
                if (futIt != structSchemas_.end()) {
                    schema.signature->ret = futIt->second.type;
                }
            }
            schema.isPub = fn.isPub;
            fnSchemas_[fn.name] = std::move(schema);
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
                for (const auto& gp : fn.genericParams) {
                    TypePtr v = makeFreshVar();
                    genEnv[gp.name] = v;
                    genVars.push_back(v);
                    genBounds.push_back(gp.bound);
                }
                // Bind Self to the impl's forType while resolving params /
                // return. This lets the impl write `self: Self -> i64`
                // and have it land as `self: ConcreteType -> i64`.
                TypePtr selfTy = resolveTypeRef(impl.forType);
                genEnv["Self"] = selfTy;
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
                std::vector<TypePtr> argTypes;
                for (const auto& p : fn.params) {
                    argTypes.push_back(resolveTypeRef(p.type));
                }
                TypePtr ret = resolveTypeRef(fn.returnType);
                currentGenericEnv_ = nullptr;
                currentEffectRowVarNames_ = nullptr;
                FnSchema sch;
                sch.signature = makeFunction(std::move(argTypes), ret);
                sch.genericVars = std::move(genVars);
                sch.genericBounds = std::move(genBounds);
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
        // forType.
        for (const auto& impl : program.impls) {
            TypePtr selfTy = resolveTypeRef(impl.forType);
            for (const auto& fn : impl.methods) {
                checkImplMethod(fn, impl.traitName, impl.forType, selfTy);
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
        result.structs = std::move(structSchemas_);
        result.enums = std::move(enumSchemas_);
        result.variantIndex = std::move(variantIndex_);
        result.matchTrees = std::move(matchTrees_);
        result.fnSchemas = std::move(fnSchemas_);
        result.callInstantiations = std::move(callInstantiations_);
        result.methodResolutions = std::move(methodResolutions_);
        result.dynCoercions = std::move(dynCoercions_);
        result.dynVtablesNeeded = std::move(dynVtablesNeeded_);
        return result;
    }

private:
    using Scope = std::unordered_map<std::string, TypePtr>;
    using GenericEnv = std::unordered_map<std::string, TypePtr>;

    std::unordered_map<std::string, FnSchema> fnSchemas_;
    std::unordered_map<std::string, StructSchema> structSchemas_;
    std::unordered_map<std::string, EnumSchema> enumSchemas_;

    // Trait declarations: traitName -> ordered list of method signatures.
    std::unordered_map<std::string, std::vector<ast::MethodSig>> traits_;
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
    std::vector<TypeError> errors_;
    std::vector<Scope> scopes_;
    // Phase 9: parallel to scopes_; names of `let mut` (reassignable)
    // bindings in each scope. A name absent here is immutable.
    std::vector<std::unordered_set<std::string>> mutScopes_;
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
        if (dynamic_cast<const ast::StringLitExpr*>(&e)) return;
        if (dynamic_cast<const ast::IdentExpr*>(&e)) return;
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            collectEffects(*bin->lhs, out);
            collectEffects(*bin->rhs, out);
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
                    out.unionWith(fnIt->second.declaredEffects);
                }
            }
            // Constructor calls don't go through fnSchemas_; they're
            // pure (no effect added).
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
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            // Phase 11: the boxed value's effects flow through. (We don't
            // synthesize `alloc` for the malloc itself — consistent with
            // struct literals / closures, whose heap use is also implicit.)
            collectEffects(*bn->value, out);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
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
    std::string implMethodMangledName(const std::string& trait,
                                       const ast::TypeRef& forType,
                                       const std::string& method) {
        return "__impl_" + trait + "_for_" + forType.name + "__" + method;
    }

    void checkImplMethod(const ast::FnDecl& fn, const std::string& traitName,
                         const ast::TypeRef& forType, const TypePtr& selfTy) {
        (void)traitName;
        (void)forType;
        auto it = implMethodMangled_.find(&fn);
        if (it == implMethodMangled_.end()) return;
        auto sit = fnSchemas_.find(it->second);
        if (sit == fnSchemas_.end()) return;
        const FnSchema& schema = sit->second;
        GenericEnv genEnv;
        for (std::size_t i = 0;
             i < fn.genericParams.size() && i < schema.genericVars.size();
             ++i) {
            genEnv[fn.genericParams[i].name] = schema.genericVars[i];
        }
        genEnv["Self"] = selfTy;
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
        pushScope();
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);
        TypePtr bodyType = checkBlock(*fn.body);
        if (fn.body->tail) {
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
        currentGenericEnv_ = nullptr;
        currentEffectRowVarNames_ = nullptr;
        currentEffectRowVarById_.clear();
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

    TypePtr resolveTypeRef(const ast::TypeRef& tr) {
        // Phase 2.4b: peel off the reference wrapper and wrap the inner
        // resolution. Recursive in case nested refs are introduced later.
        if (tr.isRef && !tr.isFn) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(resolveTypeRef(inner), tr.refIsMut);
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
        if (tr.name == "bool") {
            if (!tr.typeArgs.empty())
                error("bool takes no type arguments", tr.line, tr.column);
            return makeBool();
        }
        std::vector<TypePtr> argTypes;
        argTypes.reserve(tr.typeArgs.size());
        for (const auto& a : tr.typeArgs) argTypes.push_back(resolveTypeRef(a));
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
    }
    void popScope() {
        scopes_.pop_back();
        if (!mutScopes_.empty()) mutScopes_.pop_back();
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

        pushScope();
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);

        TypePtr bodyType = checkBlock(*fn.body);
        // If the block has a tail expression, it must match the declared
        // return type. If not (body ends with a stmt — typically a
        // `return`), we rely on the per-`return` check inside checkStmt.
        if (fn.body->tail) {
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
        currentGenericEnv_ = nullptr;
        currentEffectRowVarNames_ = nullptr;
        currentEffectRowVarById_.clear();
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
        if (dynamic_cast<const ast::StringLitExpr*>(&e)) {
            auto it = structSchemas_.find("String");
            return it != structSchemas_.end() ? it->second.type
                                              : makeStruct("String", {});
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            if (auto t = lookupLocal(id->name)) return t;
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
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            // Phase 6.1: `expr.await` requires its operand to be a
            // `Future`, which the type system uses as the opaque
            // wrapper async fns return. Phase 6.1 MVP only supports
            // `async fn () -> i64`, so await is hard-coded to yield
            // i64; generic Future<T> arrives alongside generic Vec<T>.
            TypePtr opTy = checkExpr(*ae->operand);
            TypePtr r = resolve(opTy);
            if (r->kind != TypeKind::Struct || r->structName != "Future") {
                error("`.await` requires a Future value, got " +
                          typeToString(opTy),
                      ae->line, ae->column);
                return makeInt();
            }
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
            std::string bound;
            // Find the bound by scanning the enclosing fn's schema.
            // `currentGenericEnv_` holds name -> Var; we need the parallel
            // bound list. Look up via fnSchemas_.
            for (const auto& [_n, schema] : fnSchemas_) {
                for (std::size_t i = 0; i < schema.genericVars.size(); ++i) {
                    if (schema.genericVars[i]->varId == rId &&
                        i < schema.genericBounds.size() &&
                        !schema.genericBounds[i].empty()) {
                        bound = schema.genericBounds[i];
                        break;
                    }
                }
                if (!bound.empty()) break;
            }
            if (bound.empty()) {
                error("method call on unbounded generic parameter; add a "
                      "trait bound like `<T: Trait>`",
                      mc.line, mc.column);
                for (const auto& a : mc.args) checkExpr(*a);
                return makeFreshVar();
            }
            auto traitIt = traits_.find(bound);
            if (traitIt == traits_.end()) {
                error("unknown trait bound '" + bound + "'",
                      mc.line, mc.column);
                return makeFreshVar();
            }
            const ast::MethodSig* sig = nullptr;
            for (const auto& m : traitIt->second) {
                if (m.name == mc.methodName) { sig = &m; break; }
            }
            if (!sig) {
                error("trait '" + bound + "' has no method '" +
                          mc.methodName + "'",
                      mc.line, mc.column);
                return makeFreshVar();
            }
            // Type-check arg count + arg types against the trait's
            // signature, substituting Self -> r (the schema Var).
            return checkMethodCallAgainstSig(mc, *sig, r,
                                              /*concrete=*/false, bound,
                                              /*concreteTypeName=*/{}, rId);
        }

        // Case B: receiver has a concrete type (struct/enum/i64/bool).
        std::string typeName;
        if (r->kind == TypeKind::Struct) typeName = r->structName;
        else if (r->kind == TypeKind::Enum) typeName = r->enumName;
        else if (r->kind == TypeKind::Int) typeName = "i64";
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
        methodResolutions_[&mc] = std::move(res);
        return instSig->ret;
    }

    TypePtr checkMethodCallAgainstSig(const ast::MethodCallExpr& mc,
                                       const ast::MethodSig& sig,
                                       const TypePtr& receiverTy,
                                       bool concrete,
                                       const std::string& traitName,
                                       const std::string& concreteTypeName,
                                       int boundedVarId) {
        // Resolve sig's param/return types with Self -> receiverTy.
        GenericEnv selfEnv;
        selfEnv["Self"] = receiverTy;
        currentGenericEnv_ = &selfEnv;
        std::vector<TypePtr> paramTypes;
        for (const auto& p : sig.params) {
            paramTypes.push_back(resolveTypeRef(p.type));
        }
        TypePtr retTy = resolveTypeRef(sig.returnType);
        currentGenericEnv_ = nullptr;

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
        const bool isComparison = (bin.op == ast::BinOp::Lt) ||
                                  (bin.op == ast::BinOp::Le) ||
                                  (bin.op == ast::BinOp::Gt) ||
                                  (bin.op == ast::BinOp::Ge) ||
                                  (bin.op == ast::BinOp::Eq) ||
                                  (bin.op == ast::BinOp::NotEq);
        const char* what = isComparison ? "comparison" : "arithmetic";
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
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // The callee name can itself be a captured fn value.
            noteUse(call->callee);
            for (const auto& a : call->args)
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
                    if (bound.insert(let->name).second)
                        added.push_back(let->name);
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
        // LitIntPat / WildPat: no bindings.
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
        cl.captures.clear();
        cl.captures.reserve(order.size());
        std::vector<std::pair<std::string, TypePtr>> captureTypes;
        for (const auto& name : order) {
            TypePtr t = lookupLocal(name);
            if (!t) continue; // defensive — collectFreeVars only adds locals
            // MVP capture-by-value rule: only Copy types (i64, bool, &T) may
            // be captured. Capturing a non-Copy aggregate (struct / enum /
            // &mut / fn-value) by value would be a move the borrow checker
            // cannot currently see (it does not descend into closure bodies),
            // so we reject it with a clear error rather than risk an
            // untracked use-after-move. (Documented Phase 10b limitation;
            // capture-by-reference / FnMut are deferred.)
            TypePtr rt = resolve(t);
            bool copyable = rt->kind == TypeKind::Int ||
                            rt->kind == TypeKind::Bool ||
                            rt->kind == TypeKind::Unit ||
                            (rt->kind == TypeKind::Ref && !rt->refIsMut);
            if (!copyable) {
                error("closure captures `" + name + "` of type " +
                          typeToString(t) +
                          ", but only Copy types (i64, bool, &T) may be "
                          "captured by value in this MVP; aggregate / mutable "
                          "captures are not yet supported",
                      cl.line, cl.column);
                // Continue recording it so codegen still gets a consistent
                // capture list (codegen is only invoked when typecheck is
                // clean, so this error blocks codegen anyway).
            }
            ast::ClosureCapture cap;
            cap.name = name;
            cap.type = t;
            cl.captures.push_back(std::move(cap));
            captureTypes.emplace_back(name, t);
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
        for (const auto& [name, t] : captureTypes) scopes_.back()[name] = t;
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
                    if (isBuiltinEffect(l)) { contrib.add(l); continue; }
                    TypePtr schemaVar;
                    for (const auto& [n, v] : schema.effectRowVars)
                        if (n == l) { schemaVar = v; break; }
                    if (!schemaVar) continue;
                    auto it = subst.find(resolve(schemaVar)->varId);
                    if (it == subst.end()) continue;
                    addRowVarContribution(it->second, contrib);
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

    // Phase 9: `for <pat> in <range> { body }`. Range endpoints must be
    // i64; the pattern binds the (i64) element. Body is unit; the whole
    // expression is unit. Conceptually desugars through Iterator::next.
    TypePtr checkFor(const ast::ForExpr& fe) {
        TypePtr iterT = checkExpr(*fe.iter);
        TypePtr ir = resolve(iterT);
        if (ir->kind != TypeKind::Struct || ir->structName != "Range") {
            error("for-loop iterable must be a range (a..b), got " +
                      typeToString(iterT),
                  fe.iter->line, fe.iter->column);
        }
        // Range elements are i64. Bind the pattern in a fresh scope.
        pushScope();
        Scope bindings;
        checkPattern(*fe.pattern, makeInt(), bindings);
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

    // Is `e` a place we may assign to? A `let mut` Ident, or a field chain
    // rooted at a `let mut` Ident or at a `&mut` reference (e.g. through a
    // `&mut self` receiver).
    bool isAssignablePlace(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            return isMutLocal(id->name);
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            // Walk to the root of the field chain.
            const ast::Expr* root = fe->object.get();
            while (auto* inner = dynamic_cast<const ast::FieldExpr*>(root)) {
                root = inner->object.get();
            }
            if (auto* rootId = dynamic_cast<const ast::IdentExpr*>(root)) {
                // Mutable local, OR a binding of `&mut T` (mutable ref —
                // its pointee fields are writable).
                if (isMutLocal(rootId->name)) return true;
                TypePtr rt = lookupLocal(rootId->name);
                if (rt) {
                    TypePtr r = resolve(rt);
                    if (r->kind == TypeKind::Ref && r->refIsMut) return true;
                }
            }
            return false;
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
            checkPattern(*arm.pattern, scrutT, bindings);
            for (auto& kv : bindings) {
                scopes_.back()[kv.first] = kv.second;
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
                scrutT, me.arms, enumsForPm, variantIndex_)) {
            error("non-exhaustive match: missing pattern `" + w->text + "`",
                  me.line, me.column);
        }
        const bool armsClean = (errors_.size() == errsBeforeArms);
        if (armsClean) {
            // Redundancy: report each arm unreachable given the arms before it.
            auto redundant = pattern_match::findRedundantArms(
                scrutT, me.arms, enumsForPm, variantIndex_);
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
                scrutT, me.arms, enumsForPm, variantIndex_);
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
    void checkPattern(const ast::Pattern& pat, const TypePtr& expected,
                      Scope& bindings) {
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
            bindings[vp->name] = expected;
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
                    checkPattern(*sp, makeFreshVar(), bindings);
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
                             bindings);
            }
            // Walk any extras against fresh vars to surface their bindings/errors.
            for (std::size_t i = n; i < cp->subpatterns.size(); ++i) {
                checkPattern(*cp->subpatterns[i], makeFreshVar(), bindings);
            }
            return;
        }
        error("unknown pattern kind", pat.line, pat.column);
    }

    void checkStmt(const ast::Stmt& s) {
        if (auto* let = dynamic_cast<const ast::LetStmt*>(&s)) {
            TypePtr valT = checkExpr(*let->value);
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
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignStmt*>(&s)) {
            checkAssign(*as);
            return;
        }
        if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (ret->value) {
                TypePtr valT = checkExpr(*ret->value);
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
