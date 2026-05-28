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
        // Phase 11: record each trait's method names in declaration order so
        // vtable slot layout matches the typechecker's `dynMethodSlot`.
        for (const auto& td : program.traits) {
            std::vector<std::string> order;
            order.reserve(td.methods.size());
            for (const auto& m : td.methods) order.push_back(m.name);
            traitMethodOrder_[td.name] = std::move(order);
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
        // Phase 11: emit one vtable global (+ its method thunks) per
        // (trait, type) pair that a `dyn` coercion needs. Done after impl
        // methods are declared so the thunks can call them, and before bodies
        // so coercion sites can reference the vtable global.
        for (const auto& [traitName, typeName] : tc_.dynVtablesNeeded) {
            getOrEmitVtable(traitName, typeName);
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
    // Phase 9: loop-target stack, innermost last. `continueBB` is the
    // block a `continue` branches to (loop header for `while`/`loop`, the
    // step block for `for`); `breakBB` is the post-loop block a `break`
    // jumps to. For value-carrying `loop`s, `breakValueAlloca` holds the
    // slot each `break <v>` stores into and that the loop expression reads
    // after the loop (null for unit loops).
    struct LoopFrame {
        llvm::BasicBlock* continueBB = nullptr;
        llvm::BasicBlock* breakBB = nullptr;
        llvm::AllocaInst* breakValueAlloca = nullptr;
        bool sawBreak = false; // true once any `break` targets this loop
    };
    std::vector<LoopFrame> loopFrames_;
    // Phase 4.3: per-fn binding -> kardashev TypePtr, used by emitCall
    // to recover the FunctionType for indirect calls through a let-bound
    // fn-pointer. Empty for non-fn-typed bindings; we just don't query.
    std::unordered_map<std::string, TypePtr> localTypes_;
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
    // Phase 5.z: libc externs cached as members so getOrEmitVecOp can
    // reuse them when specializing vec_* per T.
    llvm::Function* reallocFn_ = nullptr;
    llvm::Function* printfFn_ = nullptr;
    llvm::Function* mallocFn_ = nullptr;

    // Phase 10b: the uniform fat-pointer LLVM type for ALL first-class fn
    // VALUES — `{ i8* fn, i8* env }`. `fn` points at a generated function
    // whose first parameter is the env pointer (`ret(i8* env, params...)`);
    // `env` points at a heap-allocated capture struct, or is null for a
    // top-level fn used as a value. Direct calls to named globals do NOT use
    // this type (they keep their natural signature); only fn-typed values
    // (let-bound, params, if-selected, closures) do. This subsumes the
    // Phase 4.3 bare-fn-pointer representation under one calling convention.
    llvm::StructType* fnValTy_ = nullptr;

    // Phase 11: the trait-object fat-pointer LLVM type `{ i8* data, i8*
    // vtable }`, shared by `&dyn Trait` and `Box<dyn Trait>`. Per-impl vtable
    // globals are cached by "Trait/Type" key so repeated coercions reuse one.
    llvm::StructType* dynPtrTy_ = nullptr;
    std::unordered_map<std::string, llvm::GlobalVariable*> vtables_;
    // Phase 11: traitName -> method names in declaration order (the vtable
    // slot order, matching ResolvedMethod::dynMethodSlot).
    std::unordered_map<std::string, std::vector<std::string>> traitMethodOrder_;

    // Phase 10b: monotonically-increasing id for generated closure / thunk
    // functions, so each gets a unique LLVM name.
    unsigned nextClosureId_ = 0;
    // Cache of generated `__fnval_<name>` thunks, keyed by the target
    // global's name, so re-using a fn value many times emits one thunk.
    std::unordered_map<std::string, llvm::Function*> fnvalThunks_;

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
        reallocFn_ = reallocFn;
        printfFn_ = printfFn;
        mallocFn_ = mallocFn; // Phase 10b: closure env allocation

        // --- Phase 10b: fat-pointer type for fn VALUES: { i8* fn, i8* env }
        fnValTy_ = llvm::StructType::create(
            ctx, {i8PtrTy, i8PtrTy}, "kd.fnval");

        // --- Phase 11: trait-object fat pointer: { i8* data, i8* vtable }.
        // Both `&dyn Trait` and `Box<dyn Trait>` lower to this. `data` points
        // at the concrete object; `vtable` points at the impl's vtable global
        // (a struct of method fn-pointers), bitcast to i8* for a uniform type.
        dynPtrTy_ = llvm::StructType::create(
            ctx, {i8PtrTy, i8PtrTy}, "kd.dynptr");

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

        // --- Future struct layout: { i64 state, i64 result }.
        // Phase 6.1 MVP only supports `async fn () -> i64`, so the
        // result slot is hard-coded i64. Async fns return Future at the
        // user-visible LLVM signature; their actual body lives in a
        // sibling `__async_body_<name>` fn whose wrapper builds the
        // Future at every return point.
        auto* futureTy = llvm::StructType::create(
            ctx, {i64Ty, i64Ty}, "Future");
        structTypes_["Future"] = futureTy;

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

        // Phase 5.z: vec_* are now generic over T; their bodies are
        // emitted lazily per-T via `getOrEmitVecOp` when a call site
        // demands them. Nothing eager to emit here for vec_*.
        (void)reallocFn;

        auto* zeroI64 = llvm::ConstantInt::get(i64Ty, 0);

        // --- print_str(s: &String) -> i64 ---
        // Writes len bytes of s->data to stdout via printf("%.*s\n", ...).
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i8PtrTy}, false);
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
            auto* lenI32 = b.CreateTrunc(len, i32Ty, "len_i32");
            auto* fmt = b.CreateGlobalString(
                "%.*s\n", "kd_print_str_fmt", 0, module_.get());
            b.CreateCall(printfFn, {fmt, lenI32, data});
            b.CreateRet(zeroI64);
            declaredFns_["print_str"] = fn;
        }

        // --- str_len(s: &String) -> i64 ---
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i8PtrTy}, false);
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

    // Phase 5.z: lazily emit a per-element-type specialization of one of
    // the four `vec_*` built-ins. Mangled name = `<op>__<T_mangle>`,
    // matching the codegen generic-instance naming scheme so emitCall's
    // existing generic-callee branch can find it in declaredFns_ after
    // we register it here. Returns the LLVM Function.
    llvm::Function* getOrEmitVecOp(const std::string& op, const TypePtr& T) {
        std::string mangled = op + "__" + mangleType(T);
        auto it = declaredFns_.find(mangled);
        if (it != declaredFns_.end()) return it->second;

        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* zero = llvm::ConstantInt::get(i64Ty, 0);
        auto* vecTy = structTypes_["Vec"];
        auto* vecPtrTy = i8PtrTy; // opaque pointer to Vec

        llvm::Type* elemLlvmTy = mapKardashevType(T);
        // Element byte size — sourced from LLVM's DataLayout. We grab
        // the layout off the module to stay portable across platforms
        // (i8=1 byte, i64=8 bytes, struct=natural size, etc).
        auto dl = module_->getDataLayout();
        uint64_t elemBytes = dl.getTypeAllocSize(elemLlvmTy);
        auto* elemBytesK = llvm::ConstantInt::get(i64Ty, elemBytes);

        if (op == "vec_new") {
            auto* fnTy = llvm::FunctionType::get(vecTy, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, mangled,
                module_.get());
            auto* entry =
                llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* v = llvm::UndefValue::get(vecTy);
            v = b.CreateInsertValue(
                v, llvm::ConstantPointerNull::get(i8PtrTy), {0}, "data");
            v = b.CreateInsertValue(v, zero, {1}, "len");
            v = b.CreateInsertValue(v, zero, {2}, "cap");
            b.CreateRet(v);
            declaredFns_[mangled] = fn;
            return fn;
        }
        if (op == "vec_push") {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {vecPtrTy, elemLlvmTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, mangled,
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
            auto* needGrow = b.CreateICmpEQ(len, cap, "need_grow");
            b.CreateCondBr(needGrow, growBB, storeBB);

            b.SetInsertPoint(growBB);
            auto* capIsZero = b.CreateICmpEQ(cap, zero, "cap_is_zero");
            auto* four = llvm::ConstantInt::get(i64Ty, 4);
            auto* doubled = b.CreateMul(
                cap, llvm::ConstantInt::get(i64Ty, 2), "doubled");
            auto* newCap =
                b.CreateSelect(capIsZero, four, doubled, "new_cap");
            auto* oldData = b.CreateLoad(i8PtrTy, dataPtr, "old_data");
            auto* newBytes = b.CreateMul(newCap, elemBytesK, "new_bytes");
            auto* newData =
                b.CreateCall(reallocFn_, {oldData, newBytes}, "new_data");
            b.CreateStore(newData, dataPtr);
            b.CreateStore(newCap, capPtr);
            b.CreateBr(storeBB);

            b.SetInsertPoint(storeBB);
            auto* curData = b.CreateLoad(i8PtrTy, dataPtr, "cur_data");
            auto* elemPtr =
                b.CreateGEP(elemLlvmTy, curData, len, "elem_ptr");
            b.CreateStore(xVal, elemPtr);
            auto* newLen = b.CreateAdd(
                len, llvm::ConstantInt::get(i64Ty, 1), "new_len");
            b.CreateStore(newLen, lenPtr);
            b.CreateRet(zero);
            declaredFns_[mangled] = fn;
            return fn;
        }
        if (op == "vec_get") {
            auto* fnTy = llvm::FunctionType::get(
                elemLlvmTy, {vecPtrTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, mangled,
                module_.get());
            fn->getArg(0)->setName("v");
            fn->getArg(1)->setName("i");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* dataPtr =
                b.CreateStructGEP(vecTy, fn->getArg(0), 0, "data_ptr");
            auto* data = b.CreateLoad(i8PtrTy, dataPtr, "data");
            auto* elemPtr =
                b.CreateGEP(elemLlvmTy, data, fn->getArg(1), "elem_ptr");
            auto* val = b.CreateLoad(elemLlvmTy, elemPtr, "val");
            b.CreateRet(val);
            declaredFns_[mangled] = fn;
            return fn;
        }
        if (op == "vec_len") {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {vecPtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, mangled,
                module_.get());
            fn->getArg(0)->setName("v");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* lenPtr =
                b.CreateStructGEP(vecTy, fn->getArg(0), 1, "len_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            b.CreateRet(len);
            declaredFns_[mangled] = fn;
            return fn;
        }
        return nullptr;
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
            case TypeKind::Ref: {
                // Phase 11: `&dyn Trait` is a fat pointer, not a thin one.
                TypePtr inner = resolveInInstance(r->refInner);
                if (inner->kind == TypeKind::Dyn) return dynPtrTy_;
                // Phase 2.4b: `&T` lowers to an opaque pointer. LLVM 15+
                // tracks pointee types only at load/GEP sites, not on the
                // pointer type itself. We don't need the pointee here —
                // emitFieldAccess / emitMethodCall pass the right element
                // type at the load site.
                (void)mapKardashevType(r->refInner); // ensure inner is
                                                       // declared
                return llvm::PointerType::get(*ctx_, /*AS=*/0);
            }
            case TypeKind::Dyn:
                // Phase 11: a trait object is unsized, but its single
                // representation (used behind &/Box) is the fat pointer.
                return dynPtrTy_;
            case TypeKind::Box: {
                // Phase 11: `Box<dyn Trait>` is the same fat pointer as
                // `&dyn Trait` (it just owns the heap data). `Box<Concrete>`
                // is a plain heap pointer `T*` (opaque).
                TypePtr inner = resolveInInstance(r->refInner);
                if (inner->kind == TypeKind::Dyn) return dynPtrTy_;
                (void)mapKardashevType(inner); // ensure pointee declared
                return llvm::PointerType::get(*ctx_, /*AS=*/0);
            }
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
            case TypeKind::Function:
                // Phase 10b: a function-typed VALUE (a let-bound fn, a
                // `fn(i64)->i64 ! {e}` parameter, an if-selected fn, or a
                // closure) lowers to the uniform fat pointer `{ i8* fn, i8*
                // env }`. Effects are erased here — they never affect the
                // calling convention. The generated callee's signature is
                // `ret(i8* env, params...)`; emitCall rebuilds it from the
                // value's arg/ret types at the indirect call site. This
                // subsumes the Phase 4.3 bare-fn-pointer representation.
                return fnValTy_;
            default:
                errors_.push_back("codegen: unsupported type for codegen");
                return llvm::Type::getInt64Ty(*ctx_);
        }
    }

    // Build a fully-concrete Kardashev TypePtr for an AST TypeRef, honoring
    // the current generic-instance substitution and recursively
    // instantiating generic struct / enum references like `Pair<X, Y>`.
    TypePtr astTypeRefToConcrete(const ast::TypeRef& tr) {
        if (tr.isRef && !tr.isFn) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(astTypeRefToConcrete(inner), tr.refIsMut);
        }
        // Phase 11: `dyn Trait` and the built-in `Box<T>` (a user-declared
        // `Box` struct/enum shadows the built-in — mirror typecheck).
        if (tr.isDyn) {
            return makeDyn(tr.name);
        }
        if (tr.name == "Box" && !tr.isFn && tr.typeArgs.size() == 1 &&
            !tc_.structs.count("Box") && !tc_.enums.count("Box")) {
            return makeBox(astTypeRefToConcrete(tr.typeArgs[0]));
        }
        // Phase 10a: a function type lowers to a Function TypePtr; the
        // effect row is dropped (compile-time only). mapKardashevType then
        // turns this into an opaque pointer.
        if (tr.isFn) {
            std::vector<TypePtr> argTys;
            argTys.reserve(tr.fnParams.size());
            for (const auto& p : tr.fnParams)
                argTys.push_back(astTypeRefToConcrete(p));
            TypePtr retTy = tr.fnRet ? astTypeRefToConcrete(*tr.fnRet)
                                     : makeUnit();
            TypePtr fnTy = makeFunction(std::move(argTys), retTy);
            if (tr.isRef) return makeRef(fnTy, tr.refIsMut);
            return fnTy;
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
        case TypeKind::Dyn:    return "dyn_" + r->dynTraitName;
        case TypeKind::Box:    return "box_" + mangleType(r->refInner);
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
        case TypeKind::Box: {
            // Phase 11: descend into the boxed type so a `Box<T>` with a
            // generic T pins to the instance's concrete type.
            TypePtr inner = resolveInInstance(r->refInner);
            if (inner.get() == r->refInner.get()) return r;
            return makeBox(inner);
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
        if (fn.isAsync) {
            // Phase 6.1: async fn — declare two LLVM functions. The
            // user-visible `name` returns Future and is a wrapper; the
            // sibling `__async_body_<name>` carries the actual body
            // and returns the declared inner type. fnTypeFromDecl uses
            // fn.returnType which is the declared T, so the body
            // signature falls out naturally.
            auto* futureTy = structTypes_["Future"];
            std::vector<llvm::Type*> argTs;
            argTs.reserve(fn.params.size());
            for (const auto& p : fn.params) argTs.push_back(mapTypeRef(p.type));
            auto* wrapperFty = llvm::FunctionType::get(
                futureTy, argTs, /*isVarArg=*/false);
            auto* wrapper = llvm::Function::Create(
                wrapperFty, llvm::Function::ExternalLinkage, name,
                module_.get());
            unsigned i = 0;
            for (auto& arg : wrapper->args()) {
                if (i < fn.params.size()) arg.setName(fn.params[i].name);
                ++i;
            }
            declaredFns_[name] = wrapper;

            llvm::FunctionType* bodyFty = fnTypeFromDecl(fn);
            auto* bodyFn = llvm::Function::Create(
                bodyFty, llvm::Function::InternalLinkage,
                "__async_body_" + name, module_.get());
            i = 0;
            for (auto& arg : bodyFn->args()) {
                if (i < fn.params.size()) arg.setName(fn.params[i].name);
                ++i;
            }
            declaredFns_["__async_body_" + name] = bodyFn;
            currentInstanceTypeMap_ = std::move(savedMap);
            return;
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
        // Phase 6.1: async fns emit the body INTO the
        // `__async_body_<name>` sibling we declared. Then we generate
        // the Future-wrapping body of the user-visible name.
        std::string mangled;
        if (fn.isAsync) {
            mangled = "__async_body_" + baseName;
        } else {
            mangled = mangleInstance(baseName, typeArgs);
        }
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
        localTypes_.clear();
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
            // Phase 10a: record fn-typed params' Kardashev type so an
            // indirect call `f(args)` through the parameter can rebuild the
            // concrete LLVM FunctionType (same machinery as let-bound fn
            // values). Effects on the type are irrelevant to lowering.
            TypePtr paramTy = astTypeRefToConcrete(fn.params[i].type);
            if (resolve(paramTy)->kind == TypeKind::Function) {
                localTypes_[name] = paramTy;
            }
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

        // Phase 6.1: for async fns, also emit the Future-wrapping
        // wrapper that the user-visible name points at. The body we
        // just emitted lives in `__async_body_<name>`; the wrapper
        // calls it and packages the result into Future { 1, result }.
        if (fn.isAsync) {
            auto wrapperIt = declaredFns_.find(baseName);
            if (wrapperIt == declaredFns_.end()) {
                errors_.push_back(
                    "codegen: async wrapper not declared for " + baseName);
                return;
            }
            auto* wrapper = wrapperIt->second;
            auto* bodyFn = currentFn_; // the __async_body_<name> we just filled
            auto& ctx = *ctx_;
            auto* i64Ty = llvm::Type::getInt64Ty(ctx);
            auto* futureTy = structTypes_["Future"];
            auto* wEntry =
                llvm::BasicBlock::Create(ctx, "entry", wrapper);
            llvm::IRBuilder<> b(wEntry);
            std::vector<llvm::Value*> args;
            args.reserve(wrapper->arg_size());
            for (auto& arg : wrapper->args()) args.push_back(&arg);
            auto* result =
                b.CreateCall(bodyFn, args, "async_inner_result");
            llvm::Value* fut = llvm::UndefValue::get(futureTy);
            fut = b.CreateInsertValue(
                fut, llvm::ConstantInt::get(i64Ty, 1), {0},
                "future_state");
            fut = b.CreateInsertValue(fut, result, {1}, "future_result");
            b.CreateRet(fut);
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
            // Phase 4.3: remember the kardashev type so emitCall can
            // do indirect dispatch through fn-pointer locals.
            auto tyIt = tc_.exprTypes.find(let->value.get());
            if (tyIt != tc_.exprTypes.end()) {
                localTypes_[let->name] = tyIt->second;
            }
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
        if (auto* as = dynamic_cast<const ast::AssignStmt*>(&s)) {
            emitAssign(*as);
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
            emitExpr(*es->expr);
            return;
        }
    }

    // Phase 9: store `rhs` into the place named by `target`. Supports a
    // bare local Ident and a (possibly nested) field-access chain rooted
    // at a local or a `&mut` reference. Computes a pointer to the place,
    // then stores. For field chains rooted at an aggregate-by-value local
    // we round-trip through the alloca (load/insert/store handled by the
    // GEP-on-alloca path).
    void emitAssign(const ast::AssignStmt& as) {
        llvm::Value* slot = emitPlaceAddr(*as.target);
        if (!slot) {
            errors_.push_back("codegen: unsupported assignment target");
            return;
        }
        llvm::Value* v = emitExpr(*as.value);
        builder_->CreateStore(v, slot);
    }

    // Compute an address (pointer) for an assignable place. Returns null
    // if the target shape isn't supported.
    llvm::Value* emitPlaceAddr(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            auto it = locals_.find(id->name);
            if (it == locals_.end()) return nullptr;
            return it->second; // the alloca is the slot's address
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            // Address of the base place, plus the element type describing
            // what that address points at.
            TypePtr baseTy;
            llvm::Value* baseAddr = emitPlaceBase(*fe->object, baseTy);
            if (!baseAddr || !baseTy) return nullptr;
            TypePtr st = resolveInInstance(baseTy);
            if (st->kind != TypeKind::Struct) return nullptr;
            llvm::Type* structLlvm = mapKardashevType(st);
            for (unsigned i = 0; i < st->structFields.size(); ++i) {
                if (st->structFields[i].first == fe->fieldName) {
                    return builder_->CreateStructGEP(
                        structLlvm, baseAddr, i, "fld_addr_" + fe->fieldName);
                }
            }
            return nullptr;
        }
        return nullptr;
    }

    // Compute the address of a place that a field-access is rooted at, and
    // report the kardashev type the address points to via `outTy`. A bare
    // local yields its alloca (pointing at the struct value). A `&mut`
    // local yields the loaded pointer (pointing at the referent). Nested
    // field accesses recurse through emitPlaceAddr.
    llvm::Value* emitPlaceBase(const ast::Expr& e, TypePtr& outTy) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            auto it = locals_.find(id->name);
            if (it == locals_.end()) return nullptr;
            TypePtr t = lookupExprType(e);
            if (t) {
                TypePtr r = resolveInInstance(t);
                if (r->kind == TypeKind::Ref) {
                    // Through-reference: load the pointer; it addresses the
                    // referent struct directly.
                    outTy = resolveInInstance(r->refInner);
                    return builder_->CreateLoad(
                        it->second->getAllocatedType(), it->second,
                        id->name + "_ref");
                }
            }
            outTy = t;
            return it->second;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            // The address of a nested field is itself a place.
            llvm::Value* addr = emitPlaceAddr(*fe);
            outTy = lookupExprType(*fe);
            return addr;
        }
        return nullptr;
    }

    llvm::Value* emitExpr(const ast::Expr& e) {
        llvm::Value* v = emitExprRaw(e);
        // Phase 11: apply a recorded `&T`->`&dyn` / `Box<T>`->`Box<dyn>`
        // coercion at the producing expression, so it fires uniformly at
        // every site (call arg, annotated let RHS, return tail). `v` is the
        // thin data pointer; we pair it with the impl's vtable global.
        auto cit = tc_.dynCoercions.find(&e);
        if (cit != tc_.dynCoercions.end()) {
            return makeDynPtr(v, cit->second.traitName,
                              cit->second.concreteTypeName);
        }
        return v;
    }

    llvm::Value* emitExprRaw(const ast::Expr& e) {
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
            // Phase 10b: a bare fn name evaluated in expression position
            // yields a fat-pointer fn VALUE `{ __fnval_<name>, null }`. The
            // generated thunk adapts the global's natural signature to the
            // env-calling convention, so this fn value is callable through
            // the exact same indirect path as a closure. (This subsumes the
            // Phase 4.3 raw-Function* representation.)
            if (auto fnIt = declaredFns_.find(id->name);
                fnIt != declaredFns_.end()) {
                TypePtr fnTy = lookupExprType(*id);
                if (fnTy) fnTy = resolve(fnTy);
                if (!fnTy || fnTy->kind != TypeKind::Function) {
                    errors_.push_back(
                        "codegen: fn value '" + id->name +
                        "' has no Function type at use site");
                    return llvm::UndefValue::get(fnValTy_);
                }
                auto* thunk = getOrEmitFnvalThunk(fnIt->second, fnTy);
                auto* i8PtrTy = llvm::PointerType::get(*ctx_, 0);
                return makeFnVal(thunk,
                                 llvm::ConstantPointerNull::get(i8PtrTy));
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
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            return emitWhile(*we);
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            return emitLoop(*le);
        }
        if (auto* fe = dynamic_cast<const ast::ForExpr*>(&e)) {
            return emitFor(*fe);
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            return emitRange(*re);
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e)) {
            return emitBreak(*be);
        }
        if (auto* ce = dynamic_cast<const ast::ContinueExpr*>(&e)) {
            return emitContinue(*ce);
        }
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            return emitClosure(*cl);
        }
        if (auto* t = dynamic_cast<const ast::TryExpr*>(&e)) {
            return emitTry(*t);
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            return emitMethodCall(*mc);
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            return emitBoxNew(*bn);
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            return emitRef(*re);
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            // Phase 6.2: `.await` lowers to an inline poll loop — the
            // synchronous "executor" baked into the call site. Future
            // is spilled to a stack alloca so a future suspension
            // primitive can swap state across iterations; today every
            // Future is built with state == READY (1), so the loop
            // exits on the first check. The IR shape is the textbook
            // state-machine + executor pairing:
            //
            //   alloca Future fut_slot
            //   store <operand result>, fut_slot
            //   br .poll
            // .poll:
            //   state = load fut_slot.state
            //   br state == READY, .done, .pending
            // .pending:
            //   ; future: yield to scheduler, advance state via poll()
            //   br .poll
            // .done:
            //   result = load fut_slot.result
            auto& ctx = *ctx_;
            auto* i64Ty = llvm::Type::getInt64Ty(ctx);
            auto* futureTy = structTypes_["Future"];
            llvm::Value* fut = emitExpr(*ae->operand);
            auto* slot = builder_->CreateAlloca(
                futureTy, nullptr, "fut_slot");
            builder_->CreateStore(fut, slot);
            auto* pollBB = llvm::BasicBlock::Create(ctx, "await.poll",
                                                     currentFn_);
            auto* pendingBB = llvm::BasicBlock::Create(ctx, "await.pending",
                                                        currentFn_);
            auto* doneBB = llvm::BasicBlock::Create(ctx, "await.done",
                                                     currentFn_);
            builder_->CreateBr(pollBB);
            builder_->SetInsertPoint(pollBB);
            auto* statePtr = builder_->CreateStructGEP(
                futureTy, slot, 0, "fut_state_ptr");
            auto* state = builder_->CreateLoad(i64Ty, statePtr, "fut_state");
            auto* isReady = builder_->CreateICmpEQ(
                state, llvm::ConstantInt::get(i64Ty, 1),
                "await.is_ready");
            builder_->CreateCondBr(isReady, doneBB, pendingBB);
            builder_->SetInsertPoint(pendingBB);
            // No scheduler primitive yet — spin back to poll. This
            // branch is unreachable for any Future our codegen builds
            // today (they all initialise to state=READY), but the IR
            // is shaped so a future `kd_yield`-style intrinsic plugs
            // in here.
            builder_->CreateBr(pollBB);
            builder_->SetInsertPoint(doneBB);
            auto* resultPtr = builder_->CreateStructGEP(
                futureTy, slot, 1, "fut_result_ptr");
            return builder_->CreateLoad(i64Ty, resultPtr, "await_result");
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

    // Phase 11: `Box::new(v)` — malloc sizeof(T), move v into the heap slot,
    // return the (opaque) heap pointer. That pointer is the `Box<T>` value,
    // and is exactly what a `&dyn`/`Box<dyn>` coercion uses as the data slot.
    llvm::Value* emitBoxNew(const ast::BoxNewExpr& bn) {
        llvm::Value* inner = emitExpr(*bn.value);
        TypePtr innerTy = lookupExprType(*bn.value);
        llvm::Type* llvmInner =
            innerTy ? mapKardashevType(innerTy) : inner->getType();
        auto dl = module_->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(llvmInner);
        auto* i64Ty = llvm::Type::getInt64Ty(*ctx_);
        llvm::Value* raw = builder_->CreateCall(
            mallocFn_, {llvm::ConstantInt::get(i64Ty, size)}, "box.raw");
        builder_->CreateStore(inner, raw);
        return raw; // opaque pointer == Box<T>
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

    // Phase 10b: the env-aware LLVM signature of a fn VALUE whose kardashev
    // type is `fnTy` — `ret(i8* env, params...)`. Every generated callee
    // (closure body or `__fnval_<name>` thunk) uses this shape, and every
    // indirect call site rebuilds it to type the `call` instruction.
    llvm::FunctionType* envCalleeType(const TypePtr& fnTy) {
        auto* i8PtrTy = llvm::PointerType::get(*ctx_, 0);
        std::vector<llvm::Type*> argTs;
        argTs.reserve(fnTy->args.size() + 1);
        argTs.push_back(i8PtrTy); // env
        for (const auto& a : fnTy->args) argTs.push_back(mapKardashevType(a));
        llvm::Type* retT = mapKardashevType(fnTy->ret);
        return llvm::FunctionType::get(retT, argTs, /*isVarArg=*/false);
    }

    // Phase 10b: build a fat-pointer fn VALUE `{ fnPtr, envPtr }`. `fnPtr`
    // and `envPtr` are both i8* (opaque). Returns the aggregate value.
    llvm::Value* makeFnVal(llvm::Value* fnPtr, llvm::Value* envPtr) {
        llvm::Value* agg = llvm::UndefValue::get(fnValTy_);
        agg = builder_->CreateInsertValue(agg, fnPtr, {0}, "fnval.fn");
        agg = builder_->CreateInsertValue(agg, envPtr, {1}, "fnval.env");
        return agg;
    }

    // Phase 10b: get-or-create the `__fnval_<name>` thunk that adapts a
    // top-level fn `target` (natural signature `ret(params...)`) to the
    // env-calling convention `ret(i8* env, params...)`. The thunk ignores
    // env and forwards the remaining args to `target`. This is what folds
    // the Phase 4.3 "fn name as a value" path into the uniform fat pointer:
    // `let f = add;` becomes `{ __fnval_add, null }`.
    llvm::Function* getOrEmitFnvalThunk(llvm::Function* target,
                                        const TypePtr& fnTy) {
        std::string thunkName = "__fnval_" + target->getName().str();
        auto it = fnvalThunks_.find(thunkName);
        if (it != fnvalThunks_.end()) return it->second;

        // Save the builder's insertion point — we emit the thunk as a
        // sibling function, mirroring how async wrappers / vec ops are
        // emitted mid-compilation.
        auto* savedBB = builder_->GetInsertBlock();

        llvm::FunctionType* thunkTy = envCalleeType(fnTy);
        auto* thunk = llvm::Function::Create(
            thunkTy, llvm::Function::InternalLinkage, thunkName,
            module_.get());
        auto* entry = llvm::BasicBlock::Create(*ctx_, "entry", thunk);
        llvm::IRBuilder<> b(entry);
        std::vector<llvm::Value*> fwd;
        fwd.reserve(thunk->arg_size() - 1);
        unsigned idx = 0;
        for (auto& arg : thunk->args()) {
            if (idx++ == 0) continue; // skip env
            fwd.push_back(&arg);
        }
        auto* callee = llvm::cast<llvm::Function>(target);
        llvm::Value* ret = b.CreateCall(callee->getFunctionType(), callee, fwd);
        if (callee->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(ret);

        if (savedBB) builder_->SetInsertPoint(savedBB);
        fnvalThunks_[thunkName] = thunk;
        return thunk;
    }

    // Phase 11: get-or-create the vtable global for `impl Trait for Type`.
    // Layout: a struct with one i8* (fn pointer) slot per trait method, in
    // the trait's declaration order. Each slot points to a thunk
    // `__vt_<Trait>_<Type>_<method>(i8* self, args...)` that forwards `self`
    // (the object's data pointer) and the args to the real impl method. With
    // LLVM opaque pointers the "bitcast self to ConcreteType*" is implicit
    // (all pointers share one type), so the thunk is a thin forwarder; its
    // existence keeps the vtable slot a uniform fn-pointer type and gives a
    // single indirection point per (trait,type,method).
    llvm::GlobalVariable* getOrEmitVtable(const std::string& traitName,
                                          const std::string& typeName) {
        std::string key = traitName + "/" + typeName;
        auto cached = vtables_.find(key);
        if (cached != vtables_.end()) return cached->second;

        auto* i8PtrTy = llvm::PointerType::get(*ctx_, 0);
        auto orderIt = traitMethodOrder_.find(traitName);
        if (orderIt == traitMethodOrder_.end()) {
            errors_.push_back("codegen: no method order for trait " + traitName);
            return nullptr;
        }
        const std::vector<std::string>& order = orderIt->second;

        // The vtable struct type: N fn-pointer slots (opaque pointers).
        std::vector<llvm::Type*> slotTys(order.size(), i8PtrTy);
        auto* vtTy = llvm::StructType::get(*ctx_, slotTys);

        std::vector<llvm::Constant*> slots;
        slots.reserve(order.size());
        ast::TypeRef forTyRef;
        forTyRef.name = typeName;
        for (const auto& methodName : order) {
            const std::string implMangled =
                implMethodMangle(traitName, forTyRef, methodName);
            auto implIt = declaredFns_.find(implMangled);
            if (implIt == declaredFns_.end()) {
                errors_.push_back("codegen: vtable references undeclared impl "
                                  "method " + implMangled);
                slots.push_back(llvm::ConstantPointerNull::get(i8PtrTy));
                continue;
            }
            llvm::Function* thunk =
                emitVtableThunk(traitName, typeName, methodName, implIt->second);
            slots.push_back(thunk);
        }

        auto* init = llvm::ConstantStruct::get(vtTy, slots);
        auto* gv = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, init,
            "__vtable_" + traitName + "_" + typeName);
        vtables_[key] = gv;
        return gv;
    }

    // Emit (idempotently) the thunk that adapts a trait-object call to the
    // concrete impl method. Signature mirrors the impl method exactly (the
    // `self` slot is a pointer either way under opaque pointers), so the body
    // is a straight forward-and-return.
    llvm::Function* emitVtableThunk(const std::string& traitName,
                                    const std::string& typeName,
                                    const std::string& methodName,
                                    llvm::Function* impl) {
        std::string thunkName =
            "__vt_" + traitName + "_" + typeName + "_" + methodName;
        if (auto* existing = module_->getFunction(thunkName)) return existing;

        auto* savedBB = builder_->GetInsertBlock();
        llvm::FunctionType* implTy = impl->getFunctionType();
        auto* thunk = llvm::Function::Create(
            implTy, llvm::Function::InternalLinkage, thunkName, module_.get());
        auto* entry = llvm::BasicBlock::Create(*ctx_, "entry", thunk);
        llvm::IRBuilder<> b(entry);
        std::vector<llvm::Value*> fwd;
        fwd.reserve(thunk->arg_size());
        for (auto& arg : thunk->args()) fwd.push_back(&arg);
        llvm::Value* ret = b.CreateCall(implTy, impl, fwd);
        if (impl->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(ret);

        if (savedBB) builder_->SetInsertPoint(savedBB);
        return thunk;
    }

    // Phase 11: build the trait-object fat pointer `{ i8* data, i8* vtable }`
    // for coercing a thin pointer (`dataPtr`) of concrete `(trait,type)` into
    // a `&dyn Trait` / `Box<dyn Trait>`.
    llvm::Value* makeDynPtr(llvm::Value* dataPtr, const std::string& traitName,
                            const std::string& typeName) {
        llvm::GlobalVariable* vt = getOrEmitVtable(traitName, typeName);
        llvm::Value* vtPtr =
            vt ? static_cast<llvm::Value*>(vt)
               : llvm::ConstantPointerNull::get(
                     llvm::PointerType::get(*ctx_, 0));
        llvm::Value* agg = llvm::UndefValue::get(dynPtrTy_);
        agg = builder_->CreateInsertValue(agg, dataPtr, {0}, "dyn.data");
        agg = builder_->CreateInsertValue(agg, vtPtr, {1}, "dyn.vtable");
        return agg;
    }

    llvm::Value* emitCall(const ast::CallExpr& call) {
        // Phase 10b: indirect call through a fn VALUE (fat pointer). Applies
        // to let-bound fn values, fn-typed params, and if-selected fns. We
        // try this BEFORE the direct/generic paths so a binding that shadows
        // a top-level fn dispatches through the binding (matches the
        // typechecker's order). We load the `{ fn, env }` pair and call
        // `fn(env, args...)`.
        if (auto localIt = locals_.find(call.callee);
            localIt != locals_.end()) {
            auto tyIt = localTypes_.find(call.callee);
            if (tyIt != localTypes_.end()) {
                TypePtr fnTy = resolve(tyIt->second);
                if (fnTy->kind == TypeKind::Function) {
                    auto* llvmFnTy = envCalleeType(fnTy);
                    // Load the fat pointer, extract fn + env.
                    auto* fatVal = builder_->CreateLoad(
                        localIt->second->getAllocatedType(),
                        localIt->second, call.callee);
                    auto* fnPtr = builder_->CreateExtractValue(
                        fatVal, {0}, call.callee + ".fn");
                    auto* envPtr = builder_->CreateExtractValue(
                        fatVal, {1}, call.callee + ".env");
                    std::vector<llvm::Value*> args;
                    args.reserve(call.args.size() + 1);
                    args.push_back(envPtr);
                    for (const auto& a : call.args) {
                        args.push_back(emitExpr(*a));
                    }
                    return builder_->CreateCall(
                        llvmFnTy, fnPtr, args, "indir_" + call.callee);
                }
            }
        }
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
            // Phase 5.z: vec_* built-ins are generic but have no AST
            // body — synthesize their specialization directly.
            if ((call.callee == "vec_new" || call.callee == "vec_push" ||
                 call.callee == "vec_get" || call.callee == "vec_len") &&
                !concreteTypeArgs.empty()) {
                llvm::Function* fn =
                    getOrEmitVecOp(call.callee, concreteTypeArgs[0]);
                if (!fn) {
                    errors_.push_back(
                        "codegen: cannot specialize " + call.callee);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*ctx_), 0);
                }
                std::vector<llvm::Value*> args;
                args.reserve(call.args.size());
                for (const auto& a : call.args) args.push_back(emitExpr(*a));
                return builder_->CreateCall(fn, args, "call_" + call.callee);
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

        // Phase 11: dynamic dispatch. The receiver is a `&dyn Trait` /
        // `Box<dyn Trait>` fat pointer `{ data, vtable }`. Load the vtable,
        // index to this method's slot, load the fn pointer, and call it with
        // the data pointer as `self`. The SAME call site reaches different
        // impls at runtime depending on which vtable the object carries.
        if (res.kind == ResolvedMethod::Dyn) {
            return emitDynMethodCall(mc, res);
        }

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
        // Determine whether the impl method takes `self` by reference
        // (`&self`, Phase 11) or by value (`self`), so we pass the receiver
        // accordingly. The FnSchema's first arg type is the source of truth.
        bool selfByRef = false;
        if (auto sIt = tc_.fnSchemas.find(mangled); sIt != tc_.fnSchemas.end()) {
            const TypePtr& sig = sIt->second.signature;
            if (!sig->args.empty() &&
                resolve(sig->args[0])->kind == TypeKind::Ref) {
                selfByRef = true;
            }
        }
        // Receiver value vs address. The receiver's own type may be a pointer
        // to the object (`&T` borrow or `Box<T>` heap pointer) or a `T` value
        // place. A loaded `&T`/`Box<T>` value is already a pointer to the
        // object's data.
        TypePtr recvTy = lookupExprType(*mc.receiver);
        TypePtr recvR = recvTy ? resolveInInstance(recvTy) : nullptr;
        bool recvIsPtr = recvR && (recvR->kind == TypeKind::Ref ||
                                   recvR->kind == TypeKind::Box);
        llvm::Value* recv;
        if (selfByRef) {
            // `&self`: pass a pointer to the object. A pointer-shaped receiver
            // (`&T`/`Box<T>`) forwards directly; a value place yields its
            // address.
            if (recvIsPtr) {
                recv = emitExpr(*mc.receiver);
            } else {
                recv = emitPlaceAddr(*mc.receiver);
                if (!recv) {
                    // Fall back: spill the value to a temp and pass its
                    // address (handles non-place receivers).
                    llvm::Value* val = emitExpr(*mc.receiver);
                    auto* slot = builder_->CreateAlloca(
                        val->getType(), nullptr, "self.spill");
                    builder_->CreateStore(val, slot);
                    recv = slot;
                }
            }
        } else {
            // Phase 2.4b: by-value `self`. Auto-deref a `&T`/`Box<T>` receiver
            // to the `T` value the method expects.
            recv = emitExpr(*mc.receiver);
            TypePtr t = recvTy;
            unsigned refDepth = 0;
            while (t && (resolveInInstance(t)->kind == TypeKind::Ref ||
                         resolveInInstance(t)->kind == TypeKind::Box)) {
                t = resolveInInstance(t)->refInner;
                ++refDepth;
            }
            for (unsigned d = 0; d < refDepth; ++d) {
                llvm::Type* underlying = mapKardashevType(t);
                recv = builder_->CreateLoad(underlying, recv,
                                             "deref" + std::to_string(d));
            }
        }
        args.push_back(recv);
        for (const auto& a : mc.args) args.push_back(emitExpr(*a));
        return builder_->CreateCall(fn, args, "mcall_" + mc.methodName);
    }

    // Phase 11: lower a dynamic-dispatch method call through a trait object's
    // vtable. `mc.receiver` evaluates to the `{ data, vtable }` fat pointer.
    llvm::Value* emitDynMethodCall(const ast::MethodCallExpr& mc,
                                   const ResolvedMethod& res) {
        auto* i8PtrTy = llvm::PointerType::get(*ctx_, 0);
        llvm::Value* fat = emitExpr(*mc.receiver);
        // If the receiver is `&(&dyn ...)` etc., it is still represented as
        // the dynPtr aggregate value here (mapKardashevType collapses a Ref-
        // to-Dyn to the fat pointer), so a direct ExtractValue is correct.
        // But a receiver loaded as a pointer to the fat struct must be loaded
        // first; handle that by checking the LLVM type.
        if (fat->getType()->isPointerTy()) {
            fat = builder_->CreateLoad(dynPtrTy_, fat, "dyn.load");
        }
        llvm::Value* dataPtr =
            builder_->CreateExtractValue(fat, {0}, "dyn.data");
        llvm::Value* vtPtr =
            builder_->CreateExtractValue(fat, {1}, "dyn.vtable");

        // The vtable's static layout: N fn-pointer slots. We only need this
        // method's slot, so build a struct type up to and including it.
        unsigned slot = static_cast<unsigned>(res.dynMethodSlot);
        std::vector<llvm::Type*> slotTys(slot + 1, i8PtrTy);
        auto* vtTy = llvm::StructType::get(*ctx_, slotTys);
        llvm::Value* slotPtr =
            builder_->CreateStructGEP(vtTy, vtPtr, slot, "vt.slot");
        llvm::Value* fnPtr =
            builder_->CreateLoad(i8PtrTy, slotPtr, "vt.fn");

        // Reconstruct the callee signature: `ret(ptr self, args...)`, where
        // ret/arg types come from the call's resolved types.
        TypePtr retKd = lookupExprType(mc);
        llvm::Type* retTy = retKd ? mapKardashevType(retKd)
                                  : llvm::Type::getInt64Ty(*ctx_);
        std::vector<llvm::Type*> argTys;
        argTys.push_back(i8PtrTy); // self (data pointer)
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(dataPtr);
        for (const auto& a : mc.args) {
            llvm::Value* av = emitExpr(*a);
            argTys.push_back(av->getType());
            callArgs.push_back(av);
        }
        auto* fnTy = llvm::FunctionType::get(retTy, argTys, /*isVarArg=*/false);
        return builder_->CreateCall(fnTy, fnPtr, callArgs,
                                    retTy->isVoidTy() ? "" : "dyncall");
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

    // Phase 9: `while cond { body }`.
    //   entry -> header
    //   header: cond ? body : exit
    //   body:   <body>; -> header   (continue target = header)
    //   exit:   (break target)
    // The whole expression is unit (returns null).
    llvm::Value* emitWhile(const ast::WhileExpr& we) {
        auto* headerBB =
            llvm::BasicBlock::Create(*ctx_, "while.header", currentFn_);
        auto* bodyBB =
            llvm::BasicBlock::Create(*ctx_, "while.body", currentFn_);
        auto* exitBB =
            llvm::BasicBlock::Create(*ctx_, "while.exit", currentFn_);

        builder_->CreateBr(headerBB);
        builder_->SetInsertPoint(headerBB);
        llvm::Value* cond = emitExpr(*we.cond);
        if (cond->getType() != llvm::Type::getInt1Ty(*ctx_)) {
            cond = builder_->CreateICmpNE(
                cond, llvm::Constant::getNullValue(cond->getType()),
                "tobool");
        }
        builder_->CreateCondBr(cond, bodyBB, exitBB);

        builder_->SetInsertPoint(bodyBB);
        loopFrames_.push_back({headerBB, exitBB, nullptr});
        emitExpr(*we.body);
        loopFrames_.pop_back();
        if (!currentBlockTerminated()) builder_->CreateBr(headerBB);

        builder_->SetInsertPoint(exitBB);
        return nullptr; // unit
    }

    // Phase 9: `loop { body }`.
    //   entry -> body
    //   body:  <body>; -> body   (continue target = body; back-edge)
    //   exit:  (break target)
    // If the loop yields a value (every `break` carries one), we spill the
    // value through `breakValueAlloca` and load it at exit.
    llvm::Value* emitLoop(const ast::LoopExpr& le) {
        auto* bodyBB =
            llvm::BasicBlock::Create(*ctx_, "loop.body", currentFn_);
        auto* exitBB =
            llvm::BasicBlock::Create(*ctx_, "loop.exit", currentFn_);

        // Determine the loop's value type from the typechecker. A non-unit
        // type means there's at least one `break <value>`; allocate a slot.
        llvm::AllocaInst* valSlot = nullptr;
        llvm::Type* valTy = nullptr;
        TypePtr loopTy = lookupExprType(le);
        if (loopTy) {
            TypePtr r = resolveInInstance(loopTy);
            // Only a concrete value type means there's a `break <value>`.
            // Unit (valueless breaks) and an unbound Var ("never" — the
            // loop exits only via `return` or never) carry no value.
            bool concreteValue =
                r->kind == TypeKind::Int || r->kind == TypeKind::Bool ||
                r->kind == TypeKind::Struct || r->kind == TypeKind::Enum ||
                r->kind == TypeKind::Ref;
            if (concreteValue) {
                valTy = mapKardashevType(r);
                valSlot = builder_->CreateAlloca(valTy, nullptr, "loopval");
            }
        }

        builder_->CreateBr(bodyBB);
        builder_->SetInsertPoint(bodyBB);
        loopFrames_.push_back({bodyBB, exitBB, valSlot, /*sawBreak=*/false});
        emitExpr(*le.body);
        bool sawBreak = loopFrames_.back().sawBreak;
        loopFrames_.pop_back();
        if (!currentBlockTerminated()) builder_->CreateBr(bodyBB);

        builder_->SetInsertPoint(exitBB);
        if (!sawBreak) {
            // No `break` targets this loop: the exit is unreachable (the
            // loop spins forever or only leaves via `return`). Emitting
            // `unreachable` keeps the IR well-formed even when the fn's
            // declared return type isn't unit.
            builder_->CreateUnreachable();
            return nullptr;
        }
        if (valSlot) {
            return builder_->CreateLoad(valTy, valSlot, "loopval.out");
        }
        return nullptr; // unit
    }

    // Phase 9: `for <pat> in a..b { body }`. Direct integer-range lowering
    // (the trait-level spelling is `loop { match it.next() { Some(x) =>
    // body, None => break } }`; we emit the equivalent counted loop). The
    // pattern is a VarPat for ranges; we bind it to the induction var.
    //   entry: i = start
    //   header: i </<= end ? body : exit
    //   body:   bind pat = i; <body>; -> step
    //   step:   i = i + 1; -> header   (continue target = step)
    //   exit:   (break target)
    llvm::Value* emitFor(const ast::ForExpr& fe) {
        auto* i64Ty = llvm::Type::getInt64Ty(*ctx_);
        // Evaluate the range bounds. We optimize the common case where the
        // iterable is written directly as `a..b`; otherwise read the Range
        // struct value's fields.
        llvm::Value* startV = nullptr;
        llvm::Value* endV = nullptr;
        // For a literal range we know `inclusive` at compile time; for a
        // general Range value we carry the runtime flag (i64, !=0 means
        // inclusive) and fold it into the loop condition with a select.
        bool literalRange =
            dynamic_cast<const ast::RangeExpr*>(fe.iter.get()) != nullptr;
        bool inclusiveConst = false;
        llvm::Value* inclusiveFlag = nullptr; // runtime i64, general case
        if (auto* rng = dynamic_cast<const ast::RangeExpr*>(fe.iter.get())) {
            startV = emitExpr(*rng->start);
            endV = emitExpr(*rng->end);
            inclusiveConst = rng->inclusive;
        } else {
            // General Range value: extract fields {start, end, inclusive}.
            llvm::Value* rv = emitExpr(*fe.iter);
            startV = builder_->CreateExtractValue(rv, {0}, "rng.start");
            endV = builder_->CreateExtractValue(rv, {1}, "rng.end");
            inclusiveFlag =
                builder_->CreateExtractValue(rv, {2}, "rng.incl");
        }

        auto* iSlot = builder_->CreateAlloca(i64Ty, nullptr, "for.i");
        builder_->CreateStore(startV, iSlot);

        auto* headerBB =
            llvm::BasicBlock::Create(*ctx_, "for.header", currentFn_);
        auto* bodyBB =
            llvm::BasicBlock::Create(*ctx_, "for.body", currentFn_);
        auto* stepBB =
            llvm::BasicBlock::Create(*ctx_, "for.step", currentFn_);
        auto* exitBB =
            llvm::BasicBlock::Create(*ctx_, "for.exit", currentFn_);

        builder_->CreateBr(headerBB);
        builder_->SetInsertPoint(headerBB);
        llvm::Value* iCur = builder_->CreateLoad(i64Ty, iSlot, "for.i.cur");
        llvm::Value* cond;
        if (literalRange) {
            cond = inclusiveConst
                       ? builder_->CreateICmpSLE(iCur, endV, "for.cond")
                       : builder_->CreateICmpSLT(iCur, endV, "for.cond");
        } else {
            // Runtime inclusive flag: select the SLE result when
            // inclusive != 0, else the SLT result.
            llvm::Value* lt = builder_->CreateICmpSLT(iCur, endV, "for.lt");
            llvm::Value* le = builder_->CreateICmpSLE(iCur, endV, "for.le");
            llvm::Value* inclBool = builder_->CreateICmpNE(
                inclusiveFlag, llvm::ConstantInt::get(i64Ty, 0), "for.incl1");
            cond = builder_->CreateSelect(inclBool, le, lt, "for.cond");
        }
        builder_->CreateCondBr(cond, bodyBB, exitBB);

        builder_->SetInsertPoint(bodyBB);
        // Bind the loop variable to the current induction value.
        if (auto* vp = dynamic_cast<const ast::VarPat*>(fe.pattern.get())) {
            auto* vAlloca =
                builder_->CreateAlloca(i64Ty, nullptr, vp->name);
            builder_->CreateStore(iCur, vAlloca);
            locals_[vp->name] = vAlloca;
        }
        loopFrames_.push_back({stepBB, exitBB, nullptr});
        emitExpr(*fe.body);
        loopFrames_.pop_back();
        if (!currentBlockTerminated()) builder_->CreateBr(stepBB);

        builder_->SetInsertPoint(stepBB);
        llvm::Value* iStep = builder_->CreateLoad(i64Ty, iSlot, "for.i.step");
        llvm::Value* iNext = builder_->CreateAdd(
            iStep, llvm::ConstantInt::get(i64Ty, 1), "for.i.next");
        builder_->CreateStore(iNext, iSlot);
        builder_->CreateBr(headerBB);

        builder_->SetInsertPoint(exitBB);
        return nullptr; // unit
    }

    // Phase 9: `break` / `break <value>`. Stores the value into the
    // innermost loop's value slot (if any) and branches to its exit. The
    // block becomes terminated; the expression value is unused.
    llvm::Value* emitBreak(const ast::BreakExpr& be) {
        if (loopFrames_.empty()) {
            errors_.push_back("codegen: `break` outside loop");
            return nullptr;
        }
        LoopFrame& frame = loopFrames_.back();
        frame.sawBreak = true;
        if (be.value) {
            llvm::Value* v = emitExpr(*be.value);
            if (frame.breakValueAlloca) {
                builder_->CreateStore(v, frame.breakValueAlloca);
            }
        }
        builder_->CreateBr(frame.breakBB);
        return nullptr;
    }

    // Phase 9: `continue`. Branches to the innermost loop's continue
    // target (header for while/loop, step block for for).
    llvm::Value* emitContinue(const ast::ContinueExpr& ce) {
        if (loopFrames_.empty()) {
            errors_.push_back("codegen: `continue` outside loop");
            return nullptr;
        }
        builder_->CreateBr(loopFrames_.back().continueBB);
        return nullptr;
    }

    // Phase 10b: lower a capturing closure to (1) a heap-allocated env
    // struct populated by value from the captured locals, (2) a generated
    // top-level `__closure_<n>(i8* env, params...)` function whose prologue
    // reloads the captures, and (3) the fat-pointer value `{ __closure_<n>,
    // env }`. The env is heap-allocated (via libc malloc) so a simple
    // returned closure stays sound even if the defining frame pops — the
    // captures are Copy scalars copied into the heap env, so there is no
    // dangling reference. (Freeing the env / FnMut / by-ref capture are
    // deferred; see the report.)
    llvm::Value* emitClosure(const ast::ClosureExpr& cl) {
        auto& ctx = *ctx_;
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);

        // The closure's kardashev Function type (params + ret), resolved in
        // the current instance so generic enclosing fns pin their type vars.
        TypePtr fnTy = lookupExprType(cl); // applies resolveInInstance
        if (!fnTy || resolve(fnTy)->kind != TypeKind::Function) {
            errors_.push_back("codegen: closure has no Function type");
            return llvm::UndefValue::get(fnValTy_);
        }
        fnTy = resolve(fnTy);

        // Build the env struct type from the captures (in declaration order).
        std::vector<llvm::Type*> capLlvmTys;
        std::vector<TypePtr> capKdTys;
        capLlvmTys.reserve(cl.captures.size());
        capKdTys.reserve(cl.captures.size());
        for (const auto& cap : cl.captures) {
            TypePtr ct = resolveInInstance(cap.type);
            capKdTys.push_back(ct);
            capLlvmTys.push_back(mapKardashevType(ct));
        }
        std::string envName = "kd.closure_env." + std::to_string(nextClosureId_);
        auto* envTy = llvm::StructType::create(ctx, capLlvmTys, envName);

        // --- In the ENCLOSING fn: allocate + populate the env on the heap.
        llvm::Value* envPtr;
        if (cl.captures.empty()) {
            // No captures => null env (like a top-level fn value). Avoids a
            // pointless zero-byte malloc.
            envPtr = llvm::ConstantPointerNull::get(i8PtrTy);
        } else {
            uint64_t envBytes =
                module_->getDataLayout().getTypeAllocSize(envTy);
            envPtr = builder_->CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, envBytes)},
                "closure_env");
            for (unsigned i = 0; i < cl.captures.size(); ++i) {
                // Load the captured local from the enclosing scope and copy
                // it (by value) into the env slot.
                const std::string& name = cl.captures[i].name;
                auto lit = locals_.find(name);
                if (lit == locals_.end()) {
                    errors_.push_back(
                        "codegen: closure capture '" + name +
                        "' not a local in the enclosing scope");
                    continue;
                }
                llvm::Value* loaded = builder_->CreateLoad(
                    lit->second->getAllocatedType(), lit->second,
                    "cap_" + name);
                auto* slot = builder_->CreateStructGEP(
                    envTy, envPtr, i, "cap_slot_" + name);
                builder_->CreateStore(loaded, slot);
            }
        }

        // --- Generate the closure body function as a sibling.
        std::string fnName = "__closure_" + std::to_string(nextClosureId_);
        ++nextClosureId_;
        llvm::FunctionType* closureFnTy = envCalleeType(fnTy);
        auto* closureFn = llvm::Function::Create(
            closureFnTy, llvm::Function::InternalLinkage, fnName,
            module_.get());
        // Name args: arg0 = env, then params.
        {
            unsigned ai = 0;
            for (auto& arg : closureFn->args()) {
                if (ai == 0) arg.setName("env");
                else if (ai - 1 < cl.params.size())
                    arg.setName(cl.params[ai - 1].name);
                ++ai;
            }
        }

        // Save the full emission context — we are emitting a new function
        // body mid-stream (same pattern as async wrappers, but here the
        // companion is a complete fn with its own locals).
        auto* savedFn = currentFn_;
        auto* savedBB = builder_->GetInsertBlock();
        auto savedLocals = std::move(locals_);
        auto savedLocalTypes = std::move(localTypes_);
        auto savedLoopFrames = std::move(loopFrames_);
        TypePtr savedRetTy = currentFnReturnType_;

        locals_.clear();
        localTypes_.clear();
        loopFrames_.clear();
        currentFn_ = closureFn;
        currentFnReturnType_ = resolveInInstance(fnTy->ret);

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", closureFn);
        builder_->SetInsertPoint(entry);

        // Prologue: reload captures from env into locals. We bitcast the
        // i8* env arg to the env struct pointer (opaque pointers make this a
        // no-op, but the GEPs use envTy).
        llvm::Value* envArg = closureFn->getArg(0);
        for (unsigned i = 0; i < cl.captures.size(); ++i) {
            const std::string& name = cl.captures[i].name;
            auto* slot = builder_->CreateStructGEP(
                envTy, envArg, i, "cap_slot_" + name);
            llvm::Value* val = builder_->CreateLoad(
                capLlvmTys[i], slot, "cap_" + name);
            auto* alloca =
                builder_->CreateAlloca(capLlvmTys[i], nullptr, name);
            builder_->CreateStore(val, alloca);
            locals_[name] = alloca;
            if (capKdTys[i] && resolve(capKdTys[i])->kind ==
                                   TypeKind::Function) {
                localTypes_[name] = capKdTys[i];
            }
        }
        // Params: alloca + store, mirroring emitFunctionAs.
        for (unsigned i = 0; i < cl.params.size(); ++i) {
            auto* paramArg = closureFn->getArg(i + 1);
            auto* alloca = builder_->CreateAlloca(
                paramArg->getType(), nullptr, cl.params[i].name);
            builder_->CreateStore(paramArg, alloca);
            locals_[cl.params[i].name] = alloca;
            TypePtr pt = resolveInInstance(fnTy->args[i]);
            if (resolve(pt)->kind == TypeKind::Function) {
                localTypes_[cl.params[i].name] = pt;
            }
        }

        llvm::Value* bodyVal = emitExpr(*cl.body);
        if (!currentBlockTerminated()) {
            if (bodyVal && !closureFn->getReturnType()->isVoidTy()) {
                builder_->CreateRet(bodyVal);
            } else {
                builder_->CreateRetVoid();
            }
        }

        // Restore the enclosing context.
        currentFn_ = savedFn;
        locals_ = std::move(savedLocals);
        localTypes_ = std::move(savedLocalTypes);
        loopFrames_ = std::move(savedLoopFrames);
        currentFnReturnType_ = savedRetTy;
        if (savedBB) builder_->SetInsertPoint(savedBB);

        return makeFnVal(closureFn, envPtr);
    }

    // Phase 9: `a..b` / `a..=b` as a first-class value — build the Range
    // struct aggregate { start, end, inclusive }.
    llvm::Value* emitRange(const ast::RangeExpr& re) {
        TypePtr rangeTy = lookupExprType(re);
        llvm::Type* llvmTy = rangeTy ? mapKardashevType(rangeTy) : nullptr;
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(llvmTy);
        auto* i64Ty = llvm::Type::getInt64Ty(*ctx_);
        if (!st) {
            errors_.push_back("codegen: Range did not map to a struct type");
            return llvm::ConstantInt::get(i64Ty, 0);
        }
        llvm::Value* startV = emitExpr(*re.start);
        llvm::Value* endV = emitExpr(*re.end);
        llvm::Value* incV =
            llvm::ConstantInt::get(i64Ty, re.inclusive ? 1 : 0, true);
        llvm::Value* agg = llvm::UndefValue::get(st);
        agg = builder_->CreateInsertValue(agg, startV, {0}, "rng.s");
        agg = builder_->CreateInsertValue(agg, endV, {1}, "rng.e");
        agg = builder_->CreateInsertValue(agg, incV, {2}, "rng.i");
        return agg;
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
