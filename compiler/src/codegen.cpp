#include "kardashev/codegen.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kardashev/pattern_match.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

namespace kardashev {
namespace {

class Codegen {
public:
    explicit Codegen(const TypeCheckResult& tc)
        : tc_(tc),
          ctx_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>("kardashev", *ctx_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*ctx_)) {}

    void run(const ast::Program& program) {
        for (const auto& fn : program.functions) {
            fnAst_[fn.name] = &fn;
        }
        // Impl methods register under their mangled name so the codegen
        // tables stay 1-to-1 with fnSchemas. Phase 3.3 has only
        // non-generic impl methods. We also stash the impl's forType
        // (resolved through astTypeRefToConcrete) so emit / declare
        // passes can bind `Self`.
        for (const auto& impl : program.impls) {
            TypePtr selfTy = astTypeRefToConcrete(impl.forType);
            for (const auto& fn : impl.methods) {
                std::string mangled = implMethodMangle(impl.traitName,
                                                        impl.forType,
                                                        fn.name);
                fnAst_[mangled] = &fn;
                implMethodMangle_[&fn] = mangled;
                implMethodSelf_[&fn] = selfTy;
            }
        }
        declareAllStructs();
        declareAllEnums();
        declareBuiltins();
        // Declare monomorphic top-level functions and impl methods.
        for (const auto& fn : program.functions) {
            if (fn.genericParams.empty()) declareMonoFn(fn);
        }
        for (const auto& impl : program.impls) {
            for (const auto& fn : impl.methods) {
                if (fn.genericParams.empty()) {
                    declareMonoFnAs(fn, implMethodMangle_[&fn]);
                }
            }
        }
        // Emit monomorphic bodies. Generic-fn calls discovered along the
        // way push monomorphization requests.
        for (const auto& fn : program.functions) {
            if (fn.genericParams.empty()) emitFunction(fn, {});
        }
        for (const auto& impl : program.impls) {
            for (const auto& fn : impl.methods) {
                if (fn.genericParams.empty()) {
                    emitFunctionAs(fn, implMethodMangle_[&fn], {});
                }
            }
        }
        while (!pendingInstances_.empty()) {
            Instance inst = std::move(pendingInstances_.back());
            pendingInstances_.pop_back();
            std::string mangled = mangleInstance(inst.fnName, inst.typeArgs);
            if (!emittedInstances_.insert(mangled).second) continue;
            const ast::FnDecl* decl = fnAst_[inst.fnName];
            if (!decl) {
                errors_.push_back("codegen: missing AST for instance " +
                                  inst.fnName);
                continue;
            }
            emitFunction(*decl, inst.typeArgs);
        }
    }

    // Mirror typecheck.cpp's mangling. Phase 3.3 keys impls by the
    // implementing type's BASE name only; see typecheck.cpp's comment
    // for the rationale.
    std::string implMethodMangle(const std::string& trait,
                                  const ast::TypeRef& forType,
                                  const std::string& method) {
        return "__impl_" + trait + "_for_" + forType.name + "__" + method;
    }

    CodegenResult finish() {
        std::string verifyErrs;
        llvm::raw_string_ostream os(verifyErrs);
        if (llvm::verifyModule(*module_, &os)) {
            errors_.push_back("module verification failed: " + verifyErrs);
        } else {
            // Phase 8.1: run LLVM's default O2 pipeline so the module
            // codegen hands off to JIT / AOT is release-quality (mem2reg
            // collapses our alloca-heavy bindings, inliner cleans up the
            // built-in `print` wrapper, instcombine + GVN + DCE handle
            // the rest). Skip optimizing if verification already failed
            // — opt passes would compound the diagnostic.
            llvm::PassBuilder pb;
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cam;
            llvm::ModuleAnalysisManager mam;
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cam, mam);
            auto mpm = pb.buildPerModuleDefaultPipeline(
                llvm::OptimizationLevel::O2);
            mpm.run(*module_, mam);
        }
        return {std::move(ctx_), std::move(module_), std::move(errors_)};
    }

private:
    const TypeCheckResult& tc_;
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::vector<std::string> errors_;

    // Module-wide: mangled name -> LLVM Function declaration. Monomorphic
    // fns are keyed by their source name; generic-instance fns are keyed by
    // their mangled-instance name (e.g. `id__i64`).
    std::unordered_map<std::string, llvm::Function*> declaredFns_;

    // Source-fn name -> AST node, populated once up front so the worklist
    // loop can look up bodies without re-walking the program.
    std::unordered_map<std::string, const ast::FnDecl*> fnAst_;

    // Monomorphization worklist + dedupe.
    struct Instance {
        std::string fnName;
        std::vector<TypePtr> typeArgs;
    };
    std::vector<Instance> pendingInstances_;
    std::unordered_set<std::string> emittedInstances_;

    // Phase 3.3: per impl-method AST node, the mangled name that codegen
    // declares it under. Mirrors typecheck.cpp's `implMethodMangled_`.
    std::unordered_map<const ast::FnDecl*, std::string> implMethodMangle_;
    // Per impl-method, the concrete `Self` TypePtr to bind during
    // signature / body resolution so `self: Self` and any other Self
    // mentions map to the impl's forType.
    std::unordered_map<const ast::FnDecl*, TypePtr> implMethodSelf_;

    // Active during emission of a generic fn instance. Maps the source's
    // generic-param name (`T`) to the concrete TypePtr for this instance,
    // so `mapTypeRef` can resolve type names in fn signatures / bodies.
    // Empty during monomorphic-fn emission and during top-level setup.
    std::unordered_map<std::string, TypePtr> currentInstanceTypeMap_;
    // Same information keyed by schema-Var ID, so we can resolve TypePtrs
    // recovered from `tc_.callInstantiations` (which carry Vars, not names).
    std::unordered_map<int, TypePtr> currentSchemaVarSubst_;

    // Module-wide: struct name -> LLVM struct type.
    std::unordered_map<std::string, llvm::StructType*> structTypes_;

    // Module-wide: enum name -> LLVM struct type.
    // The struct layout is `{ i32 tag, payload_slots... }` where every
    // variant's payload slots are flattened in declaration order (variant
    // 0's payloads first, then variant 1's, etc.). For V1 we don't union/
    // overlap slots — wasteful in memory but trivial to codegen.
    std::unordered_map<std::string, llvm::StructType*> enumTypes_;

    // enumName -> for each variant index: vector of LLVM field indices in
    // the flat struct (one per payload slot). Index 0 is the tag.
    std::unordered_map<std::string, std::vector<std::vector<unsigned>>>
        enumPayloadIndices_;

    // Per-fn locals: variable name -> alloca pointer.
    std::unordered_map<std::string, llvm::AllocaInst*> locals_;
    llvm::Function* currentFn_ = nullptr;
    // Kardashev-level return type of currentFn_, resolved through the
    // instance substitution. Used by emitTry to build the propagated
    // `Err(...)` value in the correct enum layout. Reset on every
    // emitFunction invocation.
    TypePtr currentFnReturnType_;

    // Phase 3.2: split monomorphic vs generic struct/enum handling. Bodies
    // for MONOMORPHIC types are filled in up front in two passes (the
    // existing pattern); GENERIC type instances are realized on demand by
    // `getOrDeclareStructInstance` / `getOrDeclareEnumInstance` when
    // mapKardashevType encounters a typeArgs-bearing Struct/Enum.
    // Phase 6.0 built-in: emit a `print` function that wraps libc's
    // `printf` to write one i64 + newline. The typechecker registered a
    // matching schema so user calls type-check; we materialize the body
    // here so both JIT and AOT resolve it without external runtime
    // files. libc's `printf` is always linked into the host process
    // (kardc) for JIT, and clang links libc into AOT outputs by default.
    //
    // Also emits the Vec growable-buffer runtime (Phase 5.x): the LLVM
    // struct layout for Vec + the four operations (vec_new / vec_push /
    // vec_get / vec_len). The implementation depends on libc's malloc +
    // realloc; same linkage logic as printf above.
    void declareBuiltins() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);

        // --- libc externals ---
        auto* printfTy =
            llvm::FunctionType::get(i32Ty, {i8PtrTy}, /*isVarArg=*/true);
        auto* printfFn = llvm::Function::Create(
            printfTy, llvm::Function::ExternalLinkage, "printf",
            module_.get());

        auto* mallocTy =
            llvm::FunctionType::get(i8PtrTy, {i64Ty}, /*isVarArg=*/false);
        auto* mallocFn = llvm::Function::Create(
            mallocTy, llvm::Function::ExternalLinkage, "malloc",
            module_.get());

        auto* reallocTy = llvm::FunctionType::get(
            i8PtrTy, {i8PtrTy, i64Ty}, /*isVarArg=*/false);
        auto* reallocFn = llvm::Function::Create(
            reallocTy, llvm::Function::ExternalLinkage, "realloc",
            module_.get());
        (void)mallocFn; // realloc(NULL, n) == malloc(n), so we drive
                         // growth through realloc alone

        // --- Vec struct layout: { i8* data, i64 len, i64 cap } ---
        auto* vecTy = llvm::StructType::create(
            ctx, {i8PtrTy, i64Ty, i64Ty}, "Vec");
        structTypes_["Vec"] = vecTy;

        // --- String struct layout: { i8* data, i64 len }. Immutable;
        // string literals are emitted as LLVM private global constants
        // and aliased through this view.
        auto* strTy = llvm::StructType::create(
            ctx, {i8PtrTy, i64Ty}, "String");
        structTypes_["String"] = strTy;

        // --- print(i64) -> i64 ---
        {
            auto* printTy = llvm::FunctionType::get(
                i64Ty, {i64Ty}, /*isVarArg=*/false);
            auto* printFn = llvm::Function::Create(
                printTy, llvm::Function::ExternalLinkage, "print",
                module_.get());
            printFn->getArg(0)->setName("n");
            auto* entry =
                llvm::BasicBlock::Create(ctx, "entry", printFn);
            llvm::IRBuilder<> b(entry);
            auto* fmt = b.CreateGlobalString("%lld\n", "kd_print_fmt", 0,
                                               module_.get());
            b.CreateCall(printfFn, {fmt, printFn->getArg(0)});
            b.CreateRet(llvm::ConstantInt::get(i64Ty, 0));
            declaredFns_["print"] = printFn;
        }

        auto* vecPtrTy = llvm::PointerType::get(ctx, 0);
        auto* zero = llvm::ConstantInt::get(i64Ty, 0);

        // --- vec_new() -> Vec ---
        {
            auto* fnTy = llvm::FunctionType::get(vecTy, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "vec_new",
                module_.get());
            auto* entry =
                llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* v = llvm::UndefValue::get(vecTy);
            v = b.CreateInsertValue(
                v, llvm::ConstantPointerNull::get(i8PtrTy), {0}, "vec_data");
            v = b.CreateInsertValue(v, zero, {1}, "vec_len");
            v = b.CreateInsertValue(v, zero, {2}, "vec_cap");
            b.CreateRet(v);
            declaredFns_["vec_new"] = fn;
        }

        // --- vec_push(v: Vec*, x: i64) -> i64 ---
        //
        // C-equivalent:
        //   if (v->len == v->cap) {
        //       v->cap = v->cap == 0 ? 4 : v->cap * 2;
        //       v->data = realloc(v->data, v->cap * 8);
        //   }
        //   ((int64_t*)v->data)[v->len++] = x;
        //   return 0;
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {vecPtrTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "vec_push",
                module_.get());
            fn->getArg(0)->setName("v");
            fn->getArg(1)->setName("x");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* growBB = llvm::BasicBlock::Create(ctx, "grow", fn);
            auto* storeBB = llvm::BasicBlock::Create(ctx, "store", fn);
            llvm::IRBuilder<> b(entry);
            auto* vPtr = fn->getArg(0);
            auto* xVal = fn->getArg(1);
            auto* dataPtr = b.CreateStructGEP(vecTy, vPtr, 0, "data_ptr");
            auto* lenPtr = b.CreateStructGEP(vecTy, vPtr, 1, "len_ptr");
            auto* capPtr = b.CreateStructGEP(vecTy, vPtr, 2, "cap_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            auto* cap = b.CreateLoad(i64Ty, capPtr, "cap");
            auto* needGrow =
                b.CreateICmpEQ(len, cap, "need_grow");
            b.CreateCondBr(needGrow, growBB, storeBB);

            b.SetInsertPoint(growBB);
            auto* capIsZero = b.CreateICmpEQ(cap, zero, "cap_is_zero");
            auto* four = llvm::ConstantInt::get(i64Ty, 4);
            auto* doubled = b.CreateMul(
                cap, llvm::ConstantInt::get(i64Ty, 2), "doubled");
            auto* newCap =
                b.CreateSelect(capIsZero, four, doubled, "new_cap");
            auto* oldData = b.CreateLoad(i8PtrTy, dataPtr, "old_data");
            auto* eight = llvm::ConstantInt::get(i64Ty, 8);
            auto* newBytes = b.CreateMul(newCap, eight, "new_bytes");
            auto* newData =
                b.CreateCall(reallocFn, {oldData, newBytes}, "new_data");
            b.CreateStore(newData, dataPtr);
            b.CreateStore(newCap, capPtr);
            b.CreateBr(storeBB);

            b.SetInsertPoint(storeBB);
            auto* curData = b.CreateLoad(i8PtrTy, dataPtr, "cur_data");
            auto* elemPtr = b.CreateGEP(i64Ty, curData, len, "elem_ptr");
            b.CreateStore(xVal, elemPtr);
            auto* newLen = b.CreateAdd(
                len, llvm::ConstantInt::get(i64Ty, 1), "new_len");
            b.CreateStore(newLen, lenPtr);
            b.CreateRet(zero);
            declaredFns_["vec_push"] = fn;
        }

        // --- vec_get(v: Vec*, i: i64) -> i64 ---
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {vecPtrTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "vec_get",
                module_.get());
            fn->getArg(0)->setName("v");
            fn->getArg(1)->setName("i");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* dataPtr =
                b.CreateStructGEP(vecTy, fn->getArg(0), 0, "data_ptr");
            auto* data = b.CreateLoad(i8PtrTy, dataPtr, "data");
            auto* elemPtr =
                b.CreateGEP(i64Ty, data, fn->getArg(1), "elem_ptr");
            auto* val = b.CreateLoad(i64Ty, elemPtr, "val");
            b.CreateRet(val);
            declaredFns_["vec_get"] = fn;
        }

        // --- vec_len(v: Vec*) -> i64 ---
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {vecPtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "vec_len",
                module_.get());
            fn->getArg(0)->setName("v");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* lenPtr =
                b.CreateStructGEP(vecTy, fn->getArg(0), 1, "len_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            b.CreateRet(len);
            declaredFns_["vec_len"] = fn;
        }

        auto* strPtrTy = llvm::PointerType::get(ctx, 0);

        // --- print_str(s: &String) -> i64 ---
        // Writes len bytes of s->data to stdout via printf("%.*s\n", ...).
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {strPtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "print_str",
                module_.get());
            fn->getArg(0)->setName("s");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* dataPtr =
                b.CreateStructGEP(strTy, fn->getArg(0), 0, "data_ptr");
            auto* data = b.CreateLoad(i8PtrTy, dataPtr, "data");
            auto* lenPtr =
                b.CreateStructGEP(strTy, fn->getArg(0), 1, "len_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            // printf takes int, not int64, for `%.*s` precision arg.
            auto* lenI32 = b.CreateTrunc(len, i32Ty, "len_i32");
            auto* fmt = b.CreateGlobalString("%.*s\n", "kd_print_str_fmt",
                                                0, module_.get());
            b.CreateCall(printfFn, {fmt, lenI32, data});
            b.CreateRet(zero);
            declaredFns_["print_str"] = fn;
        }

        // --- str_len(s: &String) -> i64 ---
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {strPtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "str_len",
                module_.get());
            fn->getArg(0)->setName("s");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* lenPtr =
                b.CreateStructGEP(strTy, fn->getArg(0), 1, "len_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            b.CreateRet(len);
            declaredFns_["str_len"] = fn;
        }
    }

    void declareAllStructs() {
        for (const auto& [name, schema] : tc_.structs) {
            if (!schema.genericVars.empty()) continue;
            structTypes_[name] = llvm::StructType::create(*ctx_, name);
        }
        for (const auto& [name, schema] : tc_.structs) {
            if (!schema.genericVars.empty()) continue;
            std::vector<llvm::Type*> elems;
            elems.reserve(schema.type->structFields.size());
            for (const auto& [_fname, fty] : schema.type->structFields) {
                elems.push_back(mapKardashevType(fty));
            }
            structTypes_[name]->setBody(elems);
        }
    }

    void declareAllEnums() {
        for (const auto& [name, schema] : tc_.enums) {
            if (!schema.genericVars.empty()) continue;
            enumTypes_[name] = llvm::StructType::create(*ctx_, name);
        }
        for (const auto& [name, schema] : tc_.enums) {
            if (!schema.genericVars.empty()) continue;
            buildEnumBody(name, schema.type);
        }
    }

    // Populate an LLVM enum-struct's body and the per-variant payload-slot
    // index table. Factored out so it serves both the eager (monomorphic)
    // pass and the lazy (generic-instance) path.
    void buildEnumBody(const std::string& mangledName,
                       const TypePtr& enumType) {
        std::vector<llvm::Type*> elems;
        elems.push_back(llvm::Type::getInt32Ty(*ctx_)); // tag at slot 0
        std::vector<std::vector<unsigned>> perVariantSlots;
        perVariantSlots.reserve(enumType->enumVariants.size());
        for (const auto& v : enumType->enumVariants) {
            std::vector<unsigned> slots;
            slots.reserve(v.payloadTypes.size());
            for (const auto& pt : v.payloadTypes) {
                slots.push_back(static_cast<unsigned>(elems.size()));
                elems.push_back(mapKardashevType(pt));
            }
            perVariantSlots.push_back(std::move(slots));
        }
        enumTypes_[mangledName]->setBody(elems);
        enumPayloadIndices_[mangledName] = std::move(perVariantSlots);
    }

    // Mangle a struct/enum instance: `Name__arg1_arg2`. Uses the same
    // mangleType helper as fn-instance mangling for argument types.
    std::string mangleStructInstance(const std::string& name,
                                      const std::vector<TypePtr>& typeArgs) {
        return mangleInstance(name, typeArgs);
    }

    // Lazily declare (and body-fill) an LLVM StructType for a generic
    // struct instantiation discovered through mapKardashevType. The
    // declaration goes opaque first to break recursion through self-
    // referential field types (none are valid in Phase 3.2 without
    // boxing, but the guard costs nothing).
    llvm::StructType* getOrDeclareStructInstance(const TypePtr& t) {
        TypePtr r = resolve(t);
        const std::string& base = r->structName;
        std::string mangled = mangleStructInstance(base, r->typeArgs);
        auto it = structTypes_.find(mangled);
        if (it != structTypes_.end()) return it->second;
        auto* st = llvm::StructType::create(*ctx_, mangled);
        structTypes_[mangled] = st;
        std::vector<llvm::Type*> elems;
        elems.reserve(r->structFields.size());
        for (const auto& [_fname, fty] : r->structFields) {
            elems.push_back(mapKardashevType(fty));
        }
        st->setBody(elems);
        return st;
    }

    llvm::StructType* getOrDeclareEnumInstance(const TypePtr& t) {
        TypePtr r = resolve(t);
        const std::string& base = r->enumName;
        std::string mangled = mangleStructInstance(base, r->typeArgs);
        auto it = enumTypes_.find(mangled);
        if (it != enumTypes_.end()) return it->second;
        auto* st = llvm::StructType::create(*ctx_, mangled);
        enumTypes_[mangled] = st;
        buildEnumBody(mangled, r);
        return st;
    }

    llvm::Type* mapKardashevType(const TypePtr& t) {
        // First push the type through the instance's substitution so any
        // schema Vars left behind by `tc_.exprTypes` (e.g. the Pair<X,Y>
        // structFields inside a `make_pair<X,Y>` body) get pinned to
        // concrete types before LLVM-mapping.
        TypePtr r = resolveInInstance(t);
        switch (r->kind) {
            case TypeKind::Int:  return llvm::Type::getInt64Ty(*ctx_);
            case TypeKind::Bool: return llvm::Type::getInt1Ty(*ctx_);
            case TypeKind::Unit: return llvm::Type::getVoidTy(*ctx_);
            case TypeKind::Ref:
                // Phase 2.4b: `&T` lowers to an opaque pointer. LLVM 15+
                // tracks pointee types only at load/GEP sites, not on the
                // pointer type itself. We don't need the pointee here —
                // emitFieldAccess / emitMethodCall pass the right element
                // type at the load site.
                (void)mapKardashevType(r->refInner); // ensure inner is
                                                       // declared
                return llvm::PointerType::get(*ctx_, /*AS=*/0);
            case TypeKind::Struct: {
                // Generic instance (typeArgs non-empty): lazily declare a
                // dedicated LLVM struct per (name, typeArgs) tuple so
                // different instantiations get distinct layouts.
                if (!r->typeArgs.empty()) {
                    return getOrDeclareStructInstance(r);
                }
                auto it = structTypes_.find(r->structName);
                if (it != structTypes_.end()) return it->second;
                errors_.push_back("codegen: unresolved struct " + r->structName);
                return llvm::Type::getInt64Ty(*ctx_);
            }
            case TypeKind::Enum: {
                if (!r->typeArgs.empty()) {
                    return getOrDeclareEnumInstance(r);
                }
                auto it = enumTypes_.find(r->enumName);
                if (it != enumTypes_.end()) return it->second;
                errors_.push_back("codegen: unresolved enum " + r->enumName);
                return llvm::Type::getInt64Ty(*ctx_);
            }
            default:
                errors_.push_back("codegen: unsupported type for codegen");
                return llvm::Type::getInt64Ty(*ctx_);
        }
    }

    // Build a fully-concrete Kardashev TypePtr for an AST TypeRef, honoring
    // the current generic-instance substitution and recursively
    // instantiating generic struct / enum references like `Pair<X, Y>`.
    TypePtr astTypeRefToConcrete(const ast::TypeRef& tr) {
        if (tr.isRef) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(astTypeRefToConcrete(inner), tr.refIsMut);
        }
        if (auto it = currentInstanceTypeMap_.find(tr.name);
            it != currentInstanceTypeMap_.end()) {
            if (!tr.typeArgs.empty()) {
                errors_.push_back("codegen: type param '" + tr.name +
                                  "' cannot take type args");
            }
            return it->second;
        }
        if (tr.name == "i64") return makeInt();
        if (tr.name == "bool") return makeBool();
        std::vector<TypePtr> resolvedArgs;
        resolvedArgs.reserve(tr.typeArgs.size());
        for (const auto& a : tr.typeArgs) {
            resolvedArgs.push_back(astTypeRefToConcrete(a));
        }
        if (auto sit = tc_.structs.find(tr.name);
            sit != tc_.structs.end()) {
            const StructSchema& schema = sit->second;
            if (schema.genericVars.empty()) return schema.type;
            std::unordered_map<int, TypePtr> subst;
            for (std::size_t i = 0;
                 i < schema.genericVars.size() && i < resolvedArgs.size();
                 ++i) {
                subst[schema.genericVars[i]->varId] = resolvedArgs[i];
            }
            TypePtr inst = instantiate(schema.type, subst);
            inst->typeArgs = std::move(resolvedArgs);
            return inst;
        }
        if (auto eit = tc_.enums.find(tr.name); eit != tc_.enums.end()) {
            const EnumSchema& schema = eit->second;
            if (schema.genericVars.empty()) return schema.type;
            std::unordered_map<int, TypePtr> subst;
            for (std::size_t i = 0;
                 i < schema.genericVars.size() && i < resolvedArgs.size();
                 ++i) {
                subst[schema.genericVars[i]->varId] = resolvedArgs[i];
            }
            TypePtr inst = instantiate(schema.type, subst);
            inst->typeArgs = std::move(resolvedArgs);
            return inst;
        }
        errors_.push_back("codegen: unknown type " + tr.name);
        return makeInt();
    }

    llvm::Type* mapTypeRef(const ast::TypeRef& tr) {
        return mapKardashevType(astTypeRefToConcrete(tr));
    }

    llvm::FunctionType* fnTypeFromDecl(const ast::FnDecl& fn) {
        llvm::Type* retT = mapTypeRef(fn.returnType);
        std::vector<llvm::Type*> argTs;
        argTs.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            argTs.push_back(mapTypeRef(p.type));
        }
        return llvm::FunctionType::get(retT, argTs, /*isVarArg=*/false);
    }

    // Stable, name-mangling-quality string for a (resolved) TypePtr. Only
    // the kinds reachable in valid Phase 3.1 instantiations are handled
    // meaningfully — Vars and Functions don't occur as monomorphization
    // targets and just yield a placeholder mangling that surfaces as a
    // diagnostic if it ever leaks into a real call.
    std::string mangleType(const TypePtr& t) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Int:    return "i64";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Unit:   return "unit";
        case TypeKind::Struct: return r->structName;
        case TypeKind::Enum:   return r->enumName;
        case TypeKind::Var:    return "_var" + std::to_string(r->varId);
        case TypeKind::Function: return "_fn";
        case TypeKind::Ref:    return std::string(r->refIsMut ? "refmut_"
                                                              : "ref_") +
                                      mangleType(r->refInner);
        }
        return "_unknown";
    }

    std::string mangleInstance(const std::string& fnName,
                               const std::vector<TypePtr>& typeArgs) {
        if (typeArgs.empty()) return fnName;
        std::string s = fnName + "__";
        for (std::size_t i = 0; i < typeArgs.size(); ++i) {
            if (i > 0) s += "_";
            s += mangleType(typeArgs[i]);
        }
        return s;
    }

    // Resolve a TypePtr in the context of the currently-emitting instance.
    // For Phase 3.2 generic ADTs this walks the whole Type tree, applying
    // currentSchemaVarSubst_ to every Var (with union-find resolve in
    // between) and rebuilding structs / enums with concrete typeArgs +
    // structFields / enumVariants. Outside a generic instance the
    // substitution is empty and we still walk so callers can hand us
    // partially-resolved Vars from `tc_.exprTypes`.
    TypePtr resolveInInstance(const TypePtr& t) {
        TypePtr r = resolve(t);
        switch (r->kind) {
        case TypeKind::Var: {
            if (auto it = currentSchemaVarSubst_.find(r->varId);
                it != currentSchemaVarSubst_.end()) {
                return resolveInInstance(it->second);
            }
            return r;
        }
        case TypeKind::Ref: {
            TypePtr inner = resolveInInstance(r->refInner);
            if (inner.get() == r->refInner.get()) return r;
            return makeRef(inner, r->refIsMut);
        }
        case TypeKind::Struct: {
            if (r->typeArgs.empty() && r->structFields.empty()) return r;
            bool changed = false;
            std::vector<TypePtr> newArgs;
            newArgs.reserve(r->typeArgs.size());
            for (const auto& a : r->typeArgs) {
                TypePtr a2 = resolveInInstance(a);
                if (a2.get() != a.get()) changed = true;
                newArgs.push_back(std::move(a2));
            }
            std::vector<std::pair<std::string, TypePtr>> newFields;
            newFields.reserve(r->structFields.size());
            for (const auto& [n, f] : r->structFields) {
                TypePtr f2 = resolveInInstance(f);
                if (f2.get() != f.get()) changed = true;
                newFields.emplace_back(n, std::move(f2));
            }
            if (!changed) return r;
            TypePtr res = makeStruct(r->structName, std::move(newFields));
            res->typeArgs = std::move(newArgs);
            return res;
        }
        case TypeKind::Enum: {
            if (r->typeArgs.empty() && r->enumVariants.empty()) return r;
            bool changed = false;
            std::vector<TypePtr> newArgs;
            newArgs.reserve(r->typeArgs.size());
            for (const auto& a : r->typeArgs) {
                TypePtr a2 = resolveInInstance(a);
                if (a2.get() != a.get()) changed = true;
                newArgs.push_back(std::move(a2));
            }
            std::vector<EnumVariantType> newVariants;
            newVariants.reserve(r->enumVariants.size());
            for (const auto& v : r->enumVariants) {
                EnumVariantType nv;
                nv.name = v.name;
                nv.payloadTypes.reserve(v.payloadTypes.size());
                for (const auto& p : v.payloadTypes) {
                    TypePtr p2 = resolveInInstance(p);
                    if (p2.get() != p.get()) changed = true;
                    nv.payloadTypes.push_back(std::move(p2));
                }
                newVariants.push_back(std::move(nv));
            }
            if (!changed) return r;
            TypePtr res = makeEnum(r->enumName, std::move(newVariants));
            res->typeArgs = std::move(newArgs);
            return res;
        }
        default:
            return r;
        }
    }

    void setInstanceContext(const ast::FnDecl& fn,
                            const std::vector<TypePtr>& typeArgs) {
        currentInstanceTypeMap_.clear();
        currentSchemaVarSubst_.clear();
        auto schemaIt = tc_.fnSchemas.find(fn.name);
        for (std::size_t i = 0;
             i < fn.genericParams.size() && i < typeArgs.size(); ++i) {
            currentInstanceTypeMap_[fn.genericParams[i].name] = typeArgs[i];
            if (schemaIt != tc_.fnSchemas.end() &&
                i < schemaIt->second.genericVars.size()) {
                currentSchemaVarSubst_[schemaIt->second.genericVars[i]->varId] =
                    typeArgs[i];
            }
        }
    }

    void declareMonoFn(const ast::FnDecl& fn) {
        declareMonoFnAs(fn, fn.name);
    }

    void declareMonoFnAs(const ast::FnDecl& fn, const std::string& name) {
        auto savedMap = currentInstanceTypeMap_;
        auto selfIt = implMethodSelf_.find(&fn);
        if (selfIt != implMethodSelf_.end()) {
            currentInstanceTypeMap_["Self"] = selfIt->second;
        }
        llvm::FunctionType* fty = fnTypeFromDecl(fn);
        auto* f = llvm::Function::Create(
            fty, llvm::Function::ExternalLinkage, name, module_.get());
        unsigned i = 0;
        for (auto& arg : f->args()) {
            if (i < fn.params.size()) arg.setName(fn.params[i].name);
            ++i;
        }
        declaredFns_[name] = f;
        currentInstanceTypeMap_ = std::move(savedMap);
    }

    // Declare an LLVM Function for a generic instance. Caller's
    // responsibility: the call-site context (currentInstanceTypeMap_) must
    // describe the *instance being declared* on entry, so `fnTypeFromDecl`
    // resolves the source's generic-param names correctly.
    llvm::Function* declareInstance(const ast::FnDecl& fn,
                                     const std::vector<TypePtr>& typeArgs,
                                     const std::string& mangledName) {
        auto savedMap = currentInstanceTypeMap_;
        auto savedSubst = currentSchemaVarSubst_;
        setInstanceContext(fn, typeArgs);
        llvm::FunctionType* fty = fnTypeFromDecl(fn);
        auto* f = llvm::Function::Create(
            fty, llvm::Function::ExternalLinkage, mangledName, module_.get());
        unsigned i = 0;
        for (auto& arg : f->args()) {
            if (i < fn.params.size()) arg.setName(fn.params[i].name);
            ++i;
        }
        declaredFns_[mangledName] = f;
        currentInstanceTypeMap_ = std::move(savedMap);
        currentSchemaVarSubst_ = std::move(savedSubst);
        return f;
    }

    bool currentBlockTerminated() const {
        auto* bb = builder_->GetInsertBlock();
        return bb && bb->getTerminator() != nullptr;
    }

    void emitFunction(const ast::FnDecl& fn,
                      const std::vector<TypePtr>& typeArgs) {
        emitFunctionAs(fn, fn.name, typeArgs);
    }

    void emitFunctionAs(const ast::FnDecl& fn, const std::string& baseName,
                         const std::vector<TypePtr>& typeArgs) {
        // Install (or clear, for monomorphic fns) the instance type map so
        // signature / body resolution honors generic-param substitutions.
        setInstanceContext(fn, typeArgs);
        // Impl methods additionally bind `Self` to the impl's forType.
        auto selfIt = implMethodSelf_.find(&fn);
        if (selfIt != implMethodSelf_.end()) {
            currentInstanceTypeMap_["Self"] = selfIt->second;
        }
        const std::string mangled = mangleInstance(baseName, typeArgs);
        auto fnIt = declaredFns_.find(mangled);
        if (fnIt == declaredFns_.end()) {
            // Generic instance not yet declared: declare it now so
            // self-recursive generic calls find it during body emission.
            declareInstance(fn, typeArgs, mangled);
            fnIt = declaredFns_.find(mangled);
        }
        currentFn_ = fnIt->second;
        currentFnReturnType_ = astTypeRefToConcrete(fn.returnType);
        locals_.clear();
        auto* entry =
            llvm::BasicBlock::Create(*ctx_, "entry", currentFn_);
        builder_->SetInsertPoint(entry);

        // Alloca + store each parameter so we can read/write it uniformly
        // with `let` bindings. LLVM's mem2reg will lift these out.
        unsigned i = 0;
        for (auto& arg : currentFn_->args()) {
            const std::string& name = fn.params[i].name;
            auto* alloca = builder_->CreateAlloca(arg.getType(), nullptr, name);
            builder_->CreateStore(&arg, alloca);
            locals_[name] = alloca;
            ++i;
        }

        llvm::Value* bodyVal = emitBlock(*fn.body);

        // If we fell through to the end of the body without a terminator,
        // emit a `ret` using the body's tail value.
        if (!currentBlockTerminated()) {
            if (bodyVal) {
                builder_->CreateRet(bodyVal);
            } else {
                // No tail value — unit body. Either the fn returns void
                // (Phase 1 doesn't actually have such fns), or this is
                // ill-typed; either way emit ret void as a safe default.
                builder_->CreateRetVoid();
            }
        }
    }

    llvm::Value* emitBlock(const ast::BlockExpr& block) {
        for (const auto& stmt : block.stmts) {
            emitStmt(*stmt);
            if (currentBlockTerminated()) {
                // Subsequent stmts and the tail are unreachable.
                return nullptr;
            }
        }
        return block.tail ? emitExpr(*block.tail) : nullptr;
    }

    void emitStmt(const ast::Stmt& s) {
        if (auto* let = dynamic_cast<const ast::LetStmt*>(&s)) {
            llvm::Value* v = emitExpr(*let->value);
            auto* alloca =
                builder_->CreateAlloca(v->getType(), nullptr, let->name);
            builder_->CreateStore(v, alloca);
            locals_[let->name] = alloca;
            return;
        }
        if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (ret->value) {
                llvm::Value* v = emitExpr(*ret->value);
                builder_->CreateRet(v);
            } else {
                builder_->CreateRetVoid();
            }
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
            emitExpr(*es->expr);
            return;
        }
    }

    llvm::Value* emitExpr(const ast::Expr& e) {
        if (auto* lit = dynamic_cast<const ast::IntLitExpr*>(&e)) {
            return llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(*ctx_),
                static_cast<uint64_t>(lit->value), /*isSigned=*/true);
        }
        if (auto* sl = dynamic_cast<const ast::StringLitExpr*>(&e)) {
            return emitStringLit(*sl);
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            auto it = locals_.find(id->name);
            if (it != locals_.end()) {
                auto* alloca = it->second;
                return builder_->CreateLoad(alloca->getAllocatedType(),
                                            alloca, id->name);
            }
            // Fall back to unit-variant constructor (e.g. `None`).
            auto vit = tc_.variantIndex.find(id->name);
            if (vit != tc_.variantIndex.end()) {
                return emitUnitCtor(*id, vit->second.first,
                                    vit->second.second);
            }
            errors_.push_back("codegen: undefined identifier " + id->name);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return emitBinary(*bin);
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            return emitCall(*call);
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            return emitIf(*ie);
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            return emitBlock(*block);
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            return emitStructLit(*sl);
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            return emitFieldAccess(*fe);
        }
        if (auto* m = dynamic_cast<const ast::MatchExpr*>(&e)) {
            return emitMatch(*m);
        }
        if (auto* t = dynamic_cast<const ast::TryExpr*>(&e)) {
            return emitTry(*t);
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            return emitMethodCall(*mc);
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            return emitRef(*re);
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            // Phase 6 (stub): `.await` is currently a value-pass-
            // through. Once the state-machine transform lands this will
            // become the suspend point that yields control to the
            // executor; today it's just emitExpr(operand).
            return emitExpr(*ae->operand);
        }
        errors_.push_back("codegen: unknown expression kind");
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
    }

    // Phase 5.y: lower `"..."` to a private global byte constant and
    // return a String struct view over it. The global is unnamed +
    // unique per source occurrence, so duplicates aren't deduplicated
    // today (LLVM's `GlobalString` mergeable constants would dedupe,
    // but the simple form here is enough for `print_str("hi")` use).
    llvm::Value* emitStringLit(const ast::StringLitExpr& sl) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* strTy = structTypes_["String"];
        llvm::IRBuilder<> tmp(*ctx_);
        // CreateGlobalString needs an IRBuilder with an insertion
        // point; reuse builder_ for that and then restore.
        llvm::Value* ptr = builder_->CreateGlobalString(
            sl.value, "kd_strlit", 0, module_.get());
        auto* len = llvm::ConstantInt::get(
            i64Ty, static_cast<uint64_t>(sl.value.size()));
        llvm::Value* v = llvm::UndefValue::get(strTy);
        v = builder_->CreateInsertValue(v, ptr, {0}, "str_data");
        v = builder_->CreateInsertValue(v, len, {1}, "str_len");
        return v;
    }

    llvm::Value* emitStructLit(const ast::StructLitExpr& sl) {
        // Find the instance's full TypePtr via the typechecker's
        // exprTypes — for generic structs this carries the concrete
        // typeArgs. Then pin any leftover schema Vars (Phase 3.2 generic
        // fn returning generic struct) to the current instance's
        // substitution.
        auto tyIt = tc_.exprTypes.find(&sl);
        if (tyIt == tc_.exprTypes.end()) {
            errors_.push_back("codegen: no type for struct literal " +
                              sl.structName);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        TypePtr instTy = resolveInInstance(tyIt->second);
        llvm::Type* llvmTy = mapKardashevType(instTy);
        auto* st = llvm::dyn_cast<llvm::StructType>(llvmTy);
        if (!st) {
            errors_.push_back("codegen: struct lit '" + sl.structName +
                              "' did not map to a struct LLVM type");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }

        std::unordered_map<std::string, llvm::Value*> values;
        values.reserve(sl.fields.size());
        for (const auto& [name, expr] : sl.fields) {
            values[name] = emitExpr(*expr);
        }

        const auto& fields = instTy->structFields;
        llvm::Value* agg = llvm::UndefValue::get(st);
        for (unsigned i = 0; i < fields.size(); ++i) {
            auto vIt = values.find(fields[i].first);
            if (vIt == values.end()) {
                errors_.push_back("codegen: missing field " + fields[i].first +
                                  " in literal of " + sl.structName);
                continue;
            }
            agg = builder_->CreateInsertValue(agg, vIt->second, {i},
                                              "fld_" + fields[i].first);
        }
        return agg;
    }

    llvm::Value* emitFieldAccess(const ast::FieldExpr& fe) {
        auto tyIt = tc_.exprTypes.find(fe.object.get());
        if (tyIt == tc_.exprTypes.end()) {
            errors_.push_back("codegen: no type for field-access object");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        TypePtr objTy = resolveInInstance(tyIt->second);
        // Phase 2.4b: auto-deref. If the object is `&T` (possibly nested),
        // peel each layer and emit a load through the pointer before the
        // ExtractValue. `objTy` after the loop describes the underlying
        // struct; the value emitted gets dereferenced the right number of
        // times in lock-step.
        unsigned refDepth = 0;
        while (objTy->kind == TypeKind::Ref) {
            objTy = resolveInInstance(objTy->refInner);
            ++refDepth;
        }
        if (objTy->kind != TypeKind::Struct) {
            errors_.push_back("codegen: field access on non-struct value");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        for (unsigned i = 0; i < objTy->structFields.size(); ++i) {
            if (objTy->structFields[i].first == fe.fieldName) {
                llvm::Value* obj = emitExpr(*fe.object);
                // Each `&` layer is one load through the pointer to the
                // underlying value.
                llvm::Type* underlying = mapKardashevType(objTy);
                for (unsigned d = 0; d < refDepth; ++d) {
                    obj = builder_->CreateLoad(underlying, obj,
                                                "deref" + std::to_string(d));
                }
                return builder_->CreateExtractValue(obj, {i},
                                                    "f_" + fe.fieldName);
            }
        }
        errors_.push_back("codegen: no field " + fe.fieldName + " on struct " +
                          objTy->structName);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
    }

    // Phase 2.4b: `&x` lowers to the alloca address of x (a stack slot).
    // We restrict the operand to a bare Ident — borrowing temporaries
    // would require a stack-spill, which lands in Phase 2.4c alongside
    // `&mut` extensions.
    llvm::Value* emitRef(const ast::RefExpr& re) {
        auto* id = dynamic_cast<const ast::IdentExpr*>(re.operand.get());
        if (!id) {
            errors_.push_back(
                "codegen: `&` operand must be a binding (Phase 2.4b)");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        auto it = locals_.find(id->name);
        if (it == locals_.end()) {
            errors_.push_back("codegen: `&` operand unknown binding " +
                              id->name);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        // The alloca's value is already a pointer to the binding's stack
        // slot — exactly what `&T` should be at the LLVM level.
        return it->second;
    }

    llvm::Value* emitBinary(const ast::BinaryExpr& bin) {
        llvm::Value* L = emitExpr(*bin.lhs);
        llvm::Value* R = emitExpr(*bin.rhs);
        switch (bin.op) {
        case ast::BinOp::Add: return builder_->CreateAdd(L, R, "add");
        case ast::BinOp::Sub: return builder_->CreateSub(L, R, "sub");
        case ast::BinOp::Mul: return builder_->CreateMul(L, R, "mul");
        case ast::BinOp::Div: return builder_->CreateSDiv(L, R, "div");
        case ast::BinOp::Lt:  return builder_->CreateICmpSLT(L, R, "lt");
        case ast::BinOp::Le:  return builder_->CreateICmpSLE(L, R, "le");
        case ast::BinOp::Gt:  return builder_->CreateICmpSGT(L, R, "gt");
        case ast::BinOp::Ge:  return builder_->CreateICmpSGE(L, R, "ge");
        case ast::BinOp::Eq:  return builder_->CreateICmpEQ(L, R, "eq");
        case ast::BinOp::NotEq: return builder_->CreateICmpNE(L, R, "ne");
        }
        return nullptr;
    }

    llvm::Value* emitCall(const ast::CallExpr& call) {
        // Generic callee: pick the right monomorphic instance from the
        // call-site type args recorded by the typechecker, resolving them
        // through the current instance's substitution so a generic fn
        // calling another generic fn (or itself) routes to the proper
        // instance for the *current* outer specialization.
        if (auto schemaIt = tc_.fnSchemas.find(call.callee);
            schemaIt != tc_.fnSchemas.end() &&
            !schemaIt->second.genericVars.empty()) {
            auto cit = tc_.callInstantiations.find(&call);
            if (cit == tc_.callInstantiations.end()) {
                errors_.push_back(
                    "codegen: missing callInstantiation for generic call to " +
                    call.callee);
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*ctx_), 0);
            }
            std::vector<TypePtr> concreteTypeArgs;
            concreteTypeArgs.reserve(cit->second.size());
            for (const auto& ta : cit->second) {
                concreteTypeArgs.push_back(resolveInInstance(ta));
            }
            const std::string mangled =
                mangleInstance(call.callee, concreteTypeArgs);
            auto fnIt = declaredFns_.find(mangled);
            if (fnIt == declaredFns_.end()) {
                // Pre-declare so the call IR resolves now; queue for body
                // emission later. Note: declareInstance switches and
                // restores currentInstanceTypeMap_ internally, so the outer
                // context is preserved.
                const ast::FnDecl* decl = fnAst_[call.callee];
                if (!decl) {
                    errors_.push_back(
                        "codegen: missing AST for generic callee " +
                        call.callee);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*ctx_), 0);
                }
                declareInstance(*decl, concreteTypeArgs, mangled);
                fnIt = declaredFns_.find(mangled);
                if (!emittedInstances_.count(mangled)) {
                    pendingInstances_.push_back({call.callee, concreteTypeArgs});
                }
            }
            llvm::Function* fn = fnIt->second;
            std::vector<llvm::Value*> args;
            args.reserve(call.args.size());
            for (const auto& a : call.args) {
                args.push_back(emitExpr(*a));
            }
            return builder_->CreateCall(fn, args, "call_" + call.callee);
        }

        if (auto it = declaredFns_.find(call.callee);
            it != declaredFns_.end()) {
            llvm::Function* fn = it->second;
            std::vector<llvm::Value*> args;
            args.reserve(call.args.size());
            for (const auto& a : call.args) {
                args.push_back(emitExpr(*a));
            }
            return builder_->CreateCall(fn, args, "call_" + call.callee);
        }
        // Constructor with payload (e.g. `Some(42)`).
        if (auto vit = tc_.variantIndex.find(call.callee);
            vit != tc_.variantIndex.end()) {
            return emitCtorCall(call, vit->second.first, vit->second.second);
        }
        errors_.push_back("codegen: undefined function " + call.callee);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
    }

    // Resolve a ctor-bearing expression's inferred Kardashev type via the
    // typechecker's exprTypes table, returning the *instance* TypePtr
    // (with typeArgs populated for generic enums) so we can mangle to the
    // right LLVM struct. Returns nullptr if the expression has no
    // recorded type — which happens only on ill-typed programs codegen
    // shouldn't have reached.
    TypePtr lookupExprType(const ast::Expr& e) {
        auto it = tc_.exprTypes.find(&e);
        if (it == tc_.exprTypes.end()) return nullptr;
        return resolveInInstance(it->second);
    }

    llvm::Value* emitUnitCtor(const ast::IdentExpr& id,
                              const std::string& enumName,
                              unsigned variantIdx) {
        TypePtr instTy = lookupExprType(id);
        std::string mangled =
            instTy ? mangleStructInstance(enumName, instTy->typeArgs)
                   : enumName;
        // Ensure the LLVM type for this instance is declared.
        if (instTy) mapKardashevType(instTy);
        auto* st = enumTypes_[mangled];
        if (!st) {
            errors_.push_back("codegen: enum instance not declared: " +
                              mangled);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        llvm::Value* agg = llvm::UndefValue::get(st);
        return builder_->CreateInsertValue(
            agg,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), variantIdx),
            {0}, "ctor_" + enumName);
    }

    llvm::Value* emitCtorCall(const ast::CallExpr& call,
                              const std::string& enumName,
                              unsigned variantIdx) {
        TypePtr instTy = lookupExprType(call);
        std::string mangled =
            instTy ? mangleStructInstance(enumName, instTy->typeArgs)
                   : enumName;
        if (instTy) mapKardashevType(instTy);
        auto* st = enumTypes_[mangled];
        if (!st) {
            errors_.push_back("codegen: enum instance not declared: " +
                              mangled);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        const auto& payloadSlots = enumPayloadIndices_[mangled][variantIdx];
        llvm::Value* agg = llvm::UndefValue::get(st);
        agg = builder_->CreateInsertValue(
            agg,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), variantIdx),
            {0}, "ctor_" + enumName);
        for (unsigned i = 0; i < call.args.size() && i < payloadSlots.size();
             ++i) {
            llvm::Value* v = emitExpr(*call.args[i]);
            agg = builder_->CreateInsertValue(agg, v, {payloadSlots[i]},
                                              "pld");
        }
        return agg;
    }

    // Walk an occurrence path (sequence of LLVM field indices) from
    // `scrutinee`, chaining ExtractValues. Empty path returns the
    // scrutinee unchanged.
    llvm::Value* walkOccurrence(llvm::Value* scrutinee,
                                 const std::vector<unsigned>& path) {
        llvm::Value* cur = scrutinee;
        for (unsigned idx : path) {
            cur = builder_->CreateExtractValue(cur, {idx}, "occ");
        }
        return cur;
    }

    // Lower a Maranget decision tree. The scrutinee value is constant
    // throughout the recursion — occurrences are paths *into* it.
    void emitDecisionTree(
            const pattern_match::DecisionTree& dt,
            llvm::Value* scrutinee,
            llvm::BasicBlock* mergeBB,
            std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>>& incoming,
            const ast::MatchExpr& me) {
        using DT = pattern_match::DecisionTree;
        switch (dt.kind) {
            case DT::Fail: {
                builder_->CreateUnreachable();
                return;
            }
            case DT::Leaf: {
                auto savedLocals = locals_;
                for (const auto& [name, path] : dt.bindings) {
                    llvm::Value* sub = walkOccurrence(scrutinee, path);
                    auto* alloca = builder_->CreateAlloca(sub->getType(),
                                                          nullptr, name);
                    builder_->CreateStore(sub, alloca);
                    locals_[name] = alloca;
                }
                if (dt.armIndex < me.arms.size()) {
                    llvm::Value* bodyVal = emitExpr(*me.arms[dt.armIndex].body);
                    auto* bodyEnd = builder_->GetInsertBlock();
                    if (!bodyEnd->getTerminator()) {
                        builder_->CreateBr(mergeBB);
                        if (bodyVal) incoming.push_back({bodyEnd, bodyVal});
                    }
                } else {
                    errors_.push_back("codegen: leaf armIndex out of range");
                    builder_->CreateUnreachable();
                }
                locals_ = savedLocals;
                return;
            }
            case DT::Switch: {
                llvm::Value* subVal = walkOccurrence(scrutinee, dt.occurrence);
                // Pre-compute the tag (extract once if any case is DiscCtor;
                // safe and cheap for mixed/all-DiscLit since LLVM dead-code
                // eliminates an unused extract anyway).
                llvm::Value* tag = nullptr;
                for (const auto& c : dt.cases) {
                    if (c.discKind == DT::Case::DiscCtor) {
                        tag = builder_->CreateExtractValue(subVal, {0}, "tag");
                        break;
                    }
                }
                // Chain: for each case, compare; on match -> case child,
                // on miss -> next case's compare block (or default).
                for (std::size_t i = 0; i < dt.cases.size(); ++i) {
                    const auto& c = dt.cases[i];
                    llvm::Value* eq = nullptr;
                    if (c.discKind == DT::Case::DiscCtor) {
                        eq = builder_->CreateICmpEQ(
                            tag,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(*ctx_), c.ctorTag),
                            "tagmatch");
                    } else {
                        llvm::Value* litV = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(*ctx_),
                            static_cast<uint64_t>(c.litValue),
                            /*isSigned=*/true);
                        eq = builder_->CreateICmpEQ(subVal, litV, "litmatch");
                    }
                    auto* hitBB = llvm::BasicBlock::Create(
                        *ctx_, "case", currentFn_);
                    auto* missBB = llvm::BasicBlock::Create(
                        *ctx_, "next", currentFn_);
                    builder_->CreateCondBr(eq, hitBB, missBB);

                    builder_->SetInsertPoint(hitBB);
                    if (c.child) {
                        emitDecisionTree(*c.child, scrutinee, mergeBB,
                                         incoming, me);
                    } else {
                        builder_->CreateUnreachable();
                    }

                    builder_->SetInsertPoint(missBB);
                }
                // After all cases miss, fall through to default (or
                // unreachable for an exhaustive enum signature with no
                // default).
                if (dt.defaultCase) {
                    emitDecisionTree(*dt.defaultCase, scrutinee, mergeBB,
                                     incoming, me);
                } else {
                    builder_->CreateUnreachable();
                }
                return;
            }
        }
    }

    // Find the index of a variant by name in an enum Type's variant list,
    // or return UINT_MAX if missing. (Caller asserts existence — the
    // typechecker has already validated Ok/Err presence.)
    static unsigned variantIndexInEnum(const TypePtr& enumTy,
                                        const std::string& name) {
        for (unsigned i = 0; i < enumTy->enumVariants.size(); ++i) {
            if (enumTy->enumVariants[i].name == name) return i;
        }
        return static_cast<unsigned>(-1);
    }

    // Phase 3.4: lower `operand?` to:
    //
    //     %v = operand
    //     %tag = extractvalue %v, 0
    //     br tag == OkTag ? ok_bb : err_bb
    //   ok_bb:
    //     %inner = extractvalue %v, payloadSlot(Ok, 0)
    //     ; continuation: %inner is the expression's value
    //   err_bb:
    //     %ev = extractvalue %v, payloadSlot(Err, 0)
    //     %ret = build Err(%ev) in enclosing fn's return type
    //     ret %ret
    //
    // The function's return type is fetched from currentFn_ so we
    // construct the right enum struct layout for the propagated error.
    llvm::Value* emitTry(const ast::TryExpr& te) {
        llvm::Value* opVal = emitExpr(*te.operand);
        TypePtr opTy = lookupExprType(*te.operand);
        if (!opTy || opTy->kind != TypeKind::Enum) {
            errors_.push_back("codegen: `?` operand has no enum type");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        unsigned okIdx = variantIndexInEnum(opTy, "Ok");
        unsigned errIdx = variantIndexInEnum(opTy, "Err");
        if (okIdx == static_cast<unsigned>(-1) ||
            errIdx == static_cast<unsigned>(-1)) {
            errors_.push_back("codegen: `?` operand missing Ok/Err");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        const std::string opMangled =
            mangleStructInstance(opTy->enumName, opTy->typeArgs);
        // Ensure the operand enum's LLVM struct + payload indices are
        // declared (would have been by emitting its constructor / a prior
        // match, but be defensive for the `expr?` case where neither
        // happened earlier).
        mapKardashevType(opTy);
        const auto& opPayloadSlots = enumPayloadIndices_[opMangled];

        llvm::Value* tag = builder_->CreateExtractValue(opVal, {0}, "trytag");
        auto* okBB = llvm::BasicBlock::Create(*ctx_, "ok", currentFn_);
        auto* errBB = llvm::BasicBlock::Create(*ctx_, "err", currentFn_);
        auto* eq = builder_->CreateICmpEQ(
            tag,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), okIdx),
            "isok");
        builder_->CreateCondBr(eq, okBB, errBB);

        // Err path: extract payload, rebuild as the enclosing fn's return
        // type, return early.
        builder_->SetInsertPoint(errBB);
        llvm::Value* errVal = builder_->CreateExtractValue(
            opVal, {opPayloadSlots[errIdx][0]}, "errval");
        // Look up the fn's return type — pulled from the AST + instance
        // map so generic Err propagates correctly.
        TypePtr retTy = currentFnReturnType_;
        if (!retTy || retTy->kind != TypeKind::Enum) {
            errors_.push_back(
                "codegen: `?` in fn with non-enum return type");
            builder_->CreateUnreachable();
        } else {
            mapKardashevType(retTy);
            unsigned retErrIdx = variantIndexInEnum(retTy, "Err");
            const std::string retMangled =
                mangleStructInstance(retTy->enumName, retTy->typeArgs);
            auto* retST = enumTypes_[retMangled];
            llvm::Value* propAgg = llvm::UndefValue::get(retST);
            propAgg = builder_->CreateInsertValue(
                propAgg,
                llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(*ctx_), retErrIdx),
                {0}, "errtag");
            propAgg = builder_->CreateInsertValue(
                propAgg, errVal,
                {enumPayloadIndices_[retMangled][retErrIdx][0]},
                "errpld");
            builder_->CreateRet(propAgg);
        }

        // Ok path: unwrap to Ok's payload, continue.
        builder_->SetInsertPoint(okBB);
        llvm::Value* okVal = builder_->CreateExtractValue(
            opVal, {opPayloadSlots[okIdx][0]}, "okval");
        return okVal;
    }

    // Phase 3.3: lower `recv.method(args)`. For a concrete-type resolution
    // we look up the impl method's mangled LLVM Function directly; for a
    // bounded-generic resolution we route through the current instance's
    // substitution to find the concrete type, then look up its impl.
    llvm::Value* emitMethodCall(const ast::MethodCallExpr& mc) {
        auto it = tc_.methodResolutions.find(&mc);
        if (it == tc_.methodResolutions.end()) {
            errors_.push_back("codegen: missing method resolution for ." +
                              mc.methodName);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        const ResolvedMethod& res = it->second;
        std::string targetTypeName;
        if (res.kind == ResolvedMethod::Concrete) {
            targetTypeName = res.concreteTypeName;
        } else {
            // BoundedGeneric: the receiver Var should map (via the current
            // instance's substitution) to a concrete type at this
            // monomorphization. resolveInInstance + a Var lookup picks it.
            auto sit = currentSchemaVarSubst_.find(res.boundedVarId);
            if (sit == currentSchemaVarSubst_.end()) {
                errors_.push_back("codegen: bounded-generic receiver has no "
                                  "concrete type at instance for ." +
                                  res.methodName);
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*ctx_), 0);
            }
            TypePtr concrete = resolveInInstance(sit->second);
            if (concrete->kind == TypeKind::Struct)
                targetTypeName = concrete->structName;
            else if (concrete->kind == TypeKind::Enum)
                targetTypeName = concrete->enumName;
            else if (concrete->kind == TypeKind::Int)
                targetTypeName = "i64";
            else if (concrete->kind == TypeKind::Bool)
                targetTypeName = "bool";
            else {
                errors_.push_back(
                    "codegen: bounded-generic receiver resolved to "
                    "unsupported kind");
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*ctx_), 0);
            }
        }
        // Mangle the same way typecheck does so we can lookup the LLVM
        // Function declared in `run()`.
        ast::TypeRef forTyRef;
        forTyRef.name = targetTypeName;
        const std::string mangled =
            implMethodMangle(res.traitName, forTyRef, res.methodName);
        auto fnIt = declaredFns_.find(mangled);
        if (fnIt == declaredFns_.end()) {
            errors_.push_back("codegen: impl method not declared: " +
                              mangled);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        llvm::Function* fn = fnIt->second;
        std::vector<llvm::Value*> args;
        args.reserve(mc.args.size() + 1);
        // Phase 2.4b: auto-deref the receiver. If the receiver expression
        // has type `&T` (possibly nested) but the impl takes self by
        // value (`fn show(self)`), we need to load through the pointer so
        // the call site provides a `T` value. If the impl was written for
        // `&T` itself, the type-checker would route directly to that impl
        // and no extra deref is needed.
        llvm::Value* recv = emitExpr(*mc.receiver);
        TypePtr recvTy = lookupExprType(*mc.receiver);
        unsigned refDepth = 0;
        while (recvTy && recvTy->kind == TypeKind::Ref) {
            recvTy = resolveInInstance(recvTy->refInner);
            ++refDepth;
        }
        for (unsigned d = 0; d < refDepth; ++d) {
            llvm::Type* underlying = mapKardashevType(recvTy);
            recv = builder_->CreateLoad(underlying, recv,
                                         "deref" + std::to_string(d));
        }
        args.push_back(recv);
        for (const auto& a : mc.args) args.push_back(emitExpr(*a));
        return builder_->CreateCall(fn, args, "mcall_" + mc.methodName);
    }

    llvm::Value* emitMatch(const ast::MatchExpr& me) {
        llvm::Value* scrutinee = emitExpr(*me.scrutinee);
        auto* mergeBB = llvm::BasicBlock::Create(*ctx_, "matchmerge",
                                                  currentFn_);
        std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;

        auto it = tc_.matchTrees.find(&me);
        if (it == tc_.matchTrees.end() || !it->second) {
            errors_.push_back("codegen: no decision tree for match");
            builder_->CreateUnreachable();
            builder_->SetInsertPoint(mergeBB);
            return llvm::UndefValue::get(llvm::Type::getInt64Ty(*ctx_));
        }
        emitDecisionTree(*it->second, scrutinee, mergeBB, incoming, me);

        builder_->SetInsertPoint(mergeBB);
        if (incoming.empty()) {
            return llvm::UndefValue::get(llvm::Type::getInt64Ty(*ctx_));
        }
        if (incoming.size() == 1) return incoming[0].second;
        auto* phi = builder_->CreatePHI(incoming[0].second->getType(),
                                         incoming.size(), "matchval");
        for (auto& [bb, val] : incoming) phi->addIncoming(val, bb);
        return phi;
    }

    llvm::Value* emitIf(const ast::IfExpr& ie) {
        llvm::Value* cond = emitExpr(*ie.cond);
        // Comparisons already return i1; if cond's type isn't i1
        // (shouldn't happen post-typecheck), coerce defensively.
        if (cond->getType() != llvm::Type::getInt1Ty(*ctx_)) {
            cond = builder_->CreateICmpNE(
                cond, llvm::Constant::getNullValue(cond->getType()),
                "tobool");
        }

        auto* thenBB = llvm::BasicBlock::Create(*ctx_, "then", currentFn_);
        auto* elseBB = llvm::BasicBlock::Create(*ctx_, "else", currentFn_);
        auto* mergeBB = llvm::BasicBlock::Create(*ctx_, "ifcont", currentFn_);

        builder_->CreateCondBr(cond, thenBB, elseBB);

        // Then.
        builder_->SetInsertPoint(thenBB);
        llvm::Value* thenV = emitExpr(*ie.thenBranch);
        auto* thenEndBB = builder_->GetInsertBlock();
        const bool thenTerm = thenEndBB->getTerminator() != nullptr;
        if (!thenTerm) builder_->CreateBr(mergeBB);

        // Else.
        builder_->SetInsertPoint(elseBB);
        llvm::Value* elseV = emitExpr(*ie.elseBranch);
        auto* elseEndBB = builder_->GetInsertBlock();
        const bool elseTerm = elseEndBB->getTerminator() != nullptr;
        if (!elseTerm) builder_->CreateBr(mergeBB);

        // Merge.
        if (thenTerm && elseTerm) {
            builder_->SetInsertPoint(mergeBB);
            builder_->CreateUnreachable();
            return nullptr;
        }

        builder_->SetInsertPoint(mergeBB);
        if (!thenTerm && !elseTerm) {
            auto* phi = builder_->CreatePHI(thenV->getType(), 2, "ifval");
            phi->addIncoming(thenV, thenEndBB);
            phi->addIncoming(elseV, elseEndBB);
            return phi;
        }
        // Only one branch flows into the merge; its value is the if-expr's.
        return thenTerm ? elseV : thenV;
    }
};

} // namespace

CodegenResult codegen(const ast::Program& program,
                       const TypeCheckResult& tc) {
    Codegen cg(tc);
    cg.run(program);
    return cg.finish();
}

} // namespace kardashev
