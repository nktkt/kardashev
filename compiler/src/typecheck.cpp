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
            std::unordered_map<std::string, TypePtr> genEnv;
            std::vector<TypePtr> genVars;
            std::vector<std::string> genBounds;
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
                if (!gp.bound.empty() && !traits_.count(gp.bound)) {
                    error("unknown trait bound '" + gp.bound + "' on '" +
                              gp.name + "'",
                          gp.line, gp.column);
                }
                TypePtr v = makeFreshVar();
                genEnv[gp.name] = v;
                genVars.push_back(v);
                genBounds.push_back(gp.bound);
            }
            currentGenericEnv_ = &genEnv;

            std::vector<TypePtr> argTypes;
            argTypes.reserve(fn.params.size());
            for (const auto& p : fn.params) {
                argTypes.push_back(resolveTypeRef(p.type));
            }
            TypePtr ret = resolveTypeRef(fn.returnType);

            currentGenericEnv_ = nullptr;

            FnSchema schema;
            schema.signature = makeFunction(std::move(argTypes), ret);
            schema.genericVars = std::move(genVars);
            schema.genericBounds = std::move(genBounds);
            schema.declaredEffects = buildEffectSet(fn.effects,
                                                       fn.genericParams,
                                                       fn.name);
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
                currentGenericEnv_ = &genEnv;
                std::vector<TypePtr> argTypes;
                for (const auto& p : fn.params) {
                    argTypes.push_back(resolveTypeRef(p.type));
                }
                TypePtr ret = resolveTypeRef(fn.returnType);
                currentGenericEnv_ = nullptr;
                FnSchema sch;
                sch.signature = makeFunction(std::move(argTypes), ret);
                sch.genericVars = std::move(genVars);
                sch.genericBounds = std::move(genBounds);
                sch.declaredEffects = buildEffectSet(fn.effects,
                                                       fn.genericParams,
                                                       fn.name);
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
    // Per-MethodCallExpr resolution (Concrete or BoundedGeneric).
    std::unordered_map<const ast::MethodCallExpr*, ResolvedMethod>
        methodResolutions_;
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
    std::unordered_map<const ast::MatchExpr*,
                       std::unique_ptr<pattern_match::DecisionTree>>
        matchTrees_;
    std::vector<TypeError> errors_;
    std::vector<Scope> scopes_;
    TypePtr currentReturnType_;
    // Generic-param-name -> Type Var, scoped to the current fn declaration.
    // Set during Pass 1b sig resolution and during Pass 2 body checking
    // (with a fresh-instantiation copy for body checking, so accidental
    // specialization in a body — e.g. `x + 1` in `fn id<T>(x: T) -> T` —
    // doesn't taint the stored schema).
    const GenericEnv* currentGenericEnv_ = nullptr;

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
            auto fnIt = fnSchemas_.find(call->callee);
            if (fnIt != fnSchemas_.end()) {
                out.unionWith(fnIt->second.declaredEffects);
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
                } else if (auto* es = dynamic_cast<const ast::ExprStmt*>(
                               stmt.get())) {
                    collectEffects(*es->expr, out);
                }
            }
            if (block->tail) collectEffects(*block->tail, out);
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
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            collectEffects(*ae->operand, out);
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
                              const std::string& fnName) {
        EffectSet result;
        for (const auto& l : row.labels) {
            if (isBuiltinEffect(l)) {
                result.add(l);
                continue;
            }
            // Effect-row variable: must match a generic parameter name.
            // (Phase 4.3 wires these into instantiation; Phase 4.2 only
            // validates the label is declared.)
            bool isRowVar = false;
            for (const auto& gp : genericParams) {
                if (gp.name == l) { isRowVar = true; break; }
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
        currentGenericEnv_ = &genEnv;
        scopes_.push_back({});
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);
        TypePtr bodyType = checkBlock(*fn.body);
        if (fn.body->tail) {
            if (!unify(bodyType, currentReturnType_)) {
                error("impl method '" + fn.name + "' body type " +
                          typeToString(bodyType) +
                          " does not match declared return type " +
                          typeToString(currentReturnType_),
                      fn.body->tail->line, fn.body->tail->column);
            }
        }
        scopes_.pop_back();
        currentReturnType_.reset();
        currentGenericEnv_ = nullptr;
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
        if (tr.isRef) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(resolveTypeRef(inner), tr.refIsMut);
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
        currentGenericEnv_ = &genEnv;

        scopes_.push_back({});
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = resolveTypeRef(p.type);
        }
        currentReturnType_ = resolveTypeRef(fn.returnType);

        TypePtr bodyType = checkBlock(*fn.body);
        // If the block has a tail expression, it must match the declared
        // return type. If not (body ends with a stmt — typically a
        // `return`), we rely on the per-`return` check inside checkStmt.
        if (fn.body->tail) {
            if (!unify(bodyType, currentReturnType_)) {
                error("function '" + fn.name + "' body type " +
                          typeToString(bodyType) +
                          " does not match declared return type " +
                          typeToString(currentReturnType_),
                      fn.body->tail->line, fn.body->tail->column);
            }
        }
        scopes_.pop_back();
        currentReturnType_.reset();
        currentGenericEnv_ = nullptr;
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
                // Bare Ident referring to a fn name: instantiate so callers
                // never see the raw schema Vars escape. Phase 3.1 has no
                // first-class fn values, so this branch is effectively
                // unused; we instantiate defensively in case future code
                // (e.g. let-bind a fn name) reaches it.
                std::unordered_map<int, TypePtr> subst;
                for (const auto& gv : fnIt->second.genericVars) {
                    subst[gv->varId] = makeFreshVar();
                }
                return instantiate(fnIt->second.signature, subst);
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
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            return checkTry(*te);
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            return checkMethodCall(*mc);
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

    // Phase 3.3: resolve `recv.method(args)` against trait impls.
    TypePtr checkMethodCall(const ast::MethodCallExpr& mc) {
        TypePtr recvT = checkExpr(*mc.receiver);
        TypePtr r = resolve(recvT);
        // Phase 2.4b auto-deref: if the receiver is `&T` (or `&mut T`),
        // dispatch as though the receiver were the underlying `T`. Phase
        // 2.4c will refine this so impls of `Trait for &T` (when written
        // explicitly) take precedence over implicit deref.
        while (r->kind == TypeKind::Ref) r = resolve(r->refInner);

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
        std::string mangled = implMethodMangledName(
            trait, ast::TypeRef{typeName, {}, 0, 0}, mc.methodName);
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
        // Unify receiver against the impl's `self` slot (first arg).
        if (!instSig->args.empty()) {
            if (!unify(recvT, instSig->args[0])) {
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

    TypePtr checkCall(const ast::CallExpr& call) {
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
                if (!unify(argType, instSig->args[i])) {
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
            if ((call.callee == "vec_new" || call.callee == "vec_push" ||
                 call.callee == "vec_get" || call.callee == "vec_len") &&
                !typeArgs.empty()) {
                TypePtr elemTy = resolve(typeArgs[0]);
                if (elemTy->kind == TypeKind::Unit) {
                    error("Vec element type cannot be unit", call.line,
                          call.column);
                }
            }
            if (!schema.genericVars.empty()) {
                callInstantiations_[&call] = std::move(typeArgs);
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

    TypePtr checkBlock(const ast::BlockExpr& block) {
        scopes_.push_back({});
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
        scopes_.pop_back();
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
            scopes_.push_back({});
            // Type the pattern against the scrutinee type. Errors are
            // recorded inline; we still process the body so secondary
            // type errors surface.
            Scope bindings;
            checkPattern(*arm.pattern, scrutT, bindings);
            for (auto& kv : bindings) {
                scopes_.back()[kv.first] = kv.second;
            }
            TypePtr bodyT = checkExpr(*arm.body);
            scopes_.pop_back();
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
            scopes_.back()[let->name] = valT;
            return;
        }
        if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (ret->value) {
                TypePtr valT = checkExpr(*ret->value);
                if (currentReturnType_ &&
                    !unify(valT, currentReturnType_)) {
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
