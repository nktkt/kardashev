#include "kardashev/codegen.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kardashev/pattern_match.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

namespace kardashev {
namespace {

// Phase 18: the `clockid_t` integer constant for CLOCK_MONOTONIC, chosen at
// COMPILE time for the host that will also run the emitted program (CI builds
// and runs on the same OS: ubuntu glibc => 1, macOS/Darwin => 6). The emitted
// IR passes this literal to clock_gettime; we can't reference the C macro from
// raw IR so we hard-code its platform value. (CLOCK_MONOTONIC, not _REALTIME,
// so wall-clock adjustments never make a timer fire early/late.)
#if defined(__APPLE__)
constexpr int kMonotonicClockId = 6; // <time.h>: CLOCK_MONOTONIC on Darwin
#else
constexpr int kMonotonicClockId = 1; // <bits/time.h>: CLOCK_MONOTONIC on Linux
#endif

class Codegen {
public:
    explicit Codegen(const TypeCheckResult& tc, bool emitDebugInfo = false,
                     const std::string& sourceFile = "<kardashev>",
                     OptLevel optLevel = OptLevel::O2)
        : tc_(tc),
          ctx_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>("kardashev", *ctx_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*ctx_)),
          optLevel_(optLevel),
          emitDebugInfo_(emitDebugInfo) {
        if (emitDebugInfo_) initDebugInfo(sourceFile);
    }

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
            // Phase 16: record `impl Drop for T` so drop glue can invoke the
            // user destructor when a value of type T goes out of scope. Keyed
            // by the implementing type's base name (e.g. "Noisy"); the value
            // is the mangled LLVM name of its `drop(&mut self)` method.
            if (impl.traitName == "Drop") {
                for (const auto& fn : impl.methods) {
                    if (fn.name == "drop") {
                        dropImpls_[impl.forType.name] =
                            implMethodMangle(impl.traitName, impl.forType,
                                             "drop");
                    }
                }
            }
        }
        // Phase 17a: the fat-pointer fn-value type `fnValTy_` must exist
        // BEFORE user structs are laid out, because a user struct may have a
        // function-typed field (`struct Adder { f: fn(i64)->i64 }`) that maps
        // to `fnValTy_`; otherwise setBody() would receive a null element type
        // and crash. `declareBuiltins` (below) also needs it, so create it
        // once here and have declareBuiltins reuse it (guarded). We keep
        // declareBuiltins AFTER struct/enum declaration to preserve the
        // existing lazy `Option<i64>` instantiation order the HashMap
        // built-ins depend on.
        ensureFnValTy();
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
    static constexpr const char* kInherentImplSentinel = "__kd_inherent_impl";

    std::string implMethodMangle(const std::string& trait,
                                  const ast::TypeRef& forType,
                                  const std::string& method) {
        // Inherent impls (empty trait name) mangle under an internal-only
        // sentinel — must match typecheck's implMethodMangledName.
        const std::string t = trait.empty() ? kInherentImplSentinel : trait;
        return "__impl_" + t + "_for_" + forType.name + "__" + method;
    }

    CodegenResult finish() {
        // Phase 14a: finalize DWARF before verification. The module flags tell
        // LLVM the metadata is debug-info v3 and to emit DWARF v4; finalize()
        // resolves the DIBuilder's temporary nodes. Order matters: the
        // verifier rejects debug intrinsics whose CU isn't finalized / flagged.
        if (emitDebugInfo_ && dib_) {
            module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                                   llvm::DEBUG_METADATA_VERSION);
            module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
            dib_->finalize();
        }
        std::string verifyErrs;
        llvm::raw_string_ostream os(verifyErrs);
        if (llvm::verifyModule(*module_, &os)) {
            errors_.push_back("module verification failed: " + verifyErrs);
        } else {
            // Phase 8.1: run an LLVM optimization pipeline so the module
            // codegen hands off to JIT / AOT is release-quality (mem2reg
            // collapses our alloca-heavy bindings, inliner cleans up the
            // built-in `print` wrapper, instcombine + GVN + DCE handle
            // the rest). Skip optimizing if verification already failed
            // — opt passes would compound the diagnostic.
            //
            // Phase 20a: the level is now selectable via `-O0..-O3` (default
            // O2, so this is unchanged when no flag is passed). At O0 we run
            // LLVM's dedicated minimal O0 pipeline (buildPerModuleDefaultPipeline
            // asserts on O0); the alloca-heavy bindings and the `print` /
            // trivial-wrapper calls survive un-inlined, so O0 IR is materially
            // larger / less optimized than O2 — which the smoke test asserts.
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
            llvm::OptimizationLevel lvl;
            switch (optLevel_) {
                case OptLevel::O0: lvl = llvm::OptimizationLevel::O0; break;
                case OptLevel::O1: lvl = llvm::OptimizationLevel::O1; break;
                case OptLevel::O3: lvl = llvm::OptimizationLevel::O3; break;
                case OptLevel::O2:
                default:           lvl = llvm::OptimizationLevel::O2; break;
            }
            auto mpm = (optLevel_ == OptLevel::O0)
                           ? pb.buildO0DefaultPipeline(lvl)
                           : pb.buildPerModuleDefaultPipeline(lvl);
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

    // Phase 20a: which LLVM optimization pipeline finish() runs (default O2).
    OptLevel optLevel_ = OptLevel::O2;

    // --- Phase 14a: DWARF debug info (only populated when emitDebugInfo_) ---
    bool emitDebugInfo_ = false;
    std::unique_ptr<llvm::DIBuilder> dib_;
    llvm::DICompileUnit* diCU_ = nullptr;
    llvm::DIFile* diFile_ = nullptr;
    // The DISubprogram scope for the function currently being emitted, and a
    // reusable i64 debug type (every kardashev value lowers through i64-shaped
    // integers / pointers for line-table purposes; we describe params/locals
    // with a single signed-64 base type, which is enough for DW_AT_type to be
    // present and well-formed).
    llvm::DISubprogram* diCurrentSP_ = nullptr;
    llvm::DIType* diI64_ = nullptr;

    // Set up the compile unit + file. Called once from the constructor.
    void initDebugInfo(const std::string& sourceFile) {
        dib_ = std::make_unique<llvm::DIBuilder>(*module_);
        // Split the path into directory + filename for DIFile.
        std::string dir = ".";
        std::string file = sourceFile;
        auto slash = sourceFile.find_last_of('/');
        if (slash != std::string::npos) {
            dir = sourceFile.substr(0, slash);
            file = sourceFile.substr(slash + 1);
        }
        diFile_ = dib_->createFile(file, dir);
        diCU_ = dib_->createCompileUnit(
            llvm::dwarf::DW_LANG_C,  // closest stock language tag; kardashev
                                      // has no dedicated DWARF language code.
            diFile_, "kardc (kardashev)",
            /*isOptimized=*/false, /*Flags=*/"", /*RV=*/0);
        // A single signed 64-bit base type for params / locals.
        diI64_ = dib_->createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed);
    }

    // Build a DISubroutineType for a function from its kardashev signature.
    // For line-table + scope purposes the element types only need to be
    // present and well-formed, so we describe everything as i64 (return type
    // is element 0). Null entries are not used.
    llvm::DISubroutineType* makeDISubroutineType(std::size_t numParams) {
        std::vector<llvm::Metadata*> elts;
        elts.reserve(numParams + 1);
        elts.push_back(diI64_); // return type
        for (std::size_t i = 0; i < numParams; ++i) elts.push_back(diI64_);
        return dib_->createSubroutineType(
            dib_->getOrCreateTypeArray(elts));
    }

    // Make a DILocation for an AST node's 1-based line/column, scoped to the
    // current subprogram. Returns a DebugLoc that SetCurrentDebugLocation can
    // consume. No-op-safe: returns an empty DebugLoc when debug is off or no
    // subprogram scope is active.
    llvm::DebugLoc debugLocFor(std::size_t line, std::size_t column) {
        if (!emitDebugInfo_ || !diCurrentSP_) return llvm::DebugLoc();
        unsigned l = line ? static_cast<unsigned>(line) : 1;
        unsigned c = column ? static_cast<unsigned>(column) : 1;
        return llvm::DILocation::get(*ctx_, l, c, diCurrentSP_);
    }

    // Create + attach a DISubprogram for `fn` to the current LLVM function,
    // and make it the active scope. No-op when debug is off.
    void beginDebugSubprogram(const ast::FnDecl& fn) {
        diCurrentSP_ = nullptr;
        if (!emitDebugInfo_) return;
        unsigned line = fn.line ? static_cast<unsigned>(fn.line) : 1;
        auto* spType = makeDISubroutineType(fn.params.size());
        llvm::DISubprogram::DISPFlags spFlags =
            llvm::DISubprogram::SPFlagDefinition;
        llvm::DINode::DIFlags flags = llvm::DINode::FlagPrototyped;
        auto* sp = dib_->createFunction(
            diFile_, currentFn_->getName(), currentFn_->getName(), diFile_,
            line, spType, line, flags, spFlags);
        currentFn_->setSubprogram(sp);
        diCurrentSP_ = sp;
    }

    // Finalize per-function debug emission and clear the active scope so any
    // module-level instructions emitted afterwards carry no stale location.
    void endDebugSubprogram() {
        if (!emitDebugInfo_ || !diCurrentSP_) return;
        dib_->finalizeSubprogram(diCurrentSP_);
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        diCurrentSP_ = nullptr;
    }

    // Emit a DILocalVariable + dbg.declare for parameter #argNo (bonus).
    void declareDebugParam(const ast::FnDecl& fn, unsigned argNo,
                           const std::string& name, llvm::Value* alloca) {
        if (!emitDebugInfo_ || !diCurrentSP_) return;
        unsigned line = fn.line ? static_cast<unsigned>(fn.line) : 1;
        auto* lv = dib_->createParameterVariable(
            diCurrentSP_, name, argNo + 1, diFile_, line, diI64_,
            /*AlwaysPreserve=*/true);
        dib_->insertDeclare(
            alloca, lv, dib_->createExpression(),
            llvm::DILocation::get(*ctx_, line, 1, diCurrentSP_),
            builder_->GetInsertBlock());
    }

    // Emit a DILocalVariable + dbg.declare for a `let` binding (bonus).
    void declareDebugLocal(const std::string& name, std::size_t line,
                           std::size_t column, llvm::Value* alloca) {
        if (!emitDebugInfo_ || !diCurrentSP_) return;
        unsigned l = line ? static_cast<unsigned>(line) : 1;
        unsigned c = column ? static_cast<unsigned>(column) : 1;
        auto* lv = dib_->createAutoVariable(diCurrentSP_, name, diFile_, l,
                                            diI64_, /*AlwaysPreserve=*/true);
        dib_->insertDeclare(alloca, lv, dib_->createExpression(),
                            llvm::DILocation::get(*ctx_, l, c, diCurrentSP_),
                            builder_->GetInsertBlock());
    }

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

    // Phase 17a (FnMut): by-reference closure captures. Inside a closure body,
    // a by-ref capture's storage is NOT a local alloca but a POINTER (loaded
    // from the env) into the enclosing variable's alloca. Maps the captured
    // name -> { pointer-to-storage, value's LLVM type }. Reads/writes/`&`/`&mut`
    // of such a name go through this pointer so mutations persist after the
    // call. Consulted before `locals_` in the IdentExpr read, emitPlaceAddr,
    // emitPlaceBase, and emitRef paths. Saved/cleared/restored across closure
    // body emission exactly like `locals_`.
    std::unordered_map<std::string, std::pair<llvm::Value*, llvm::Type*>>
        refLocals_;

    // --- Phase 16: deterministic Drop (RAII) ---
    // implementing-type base name -> mangled LLVM name of its `drop(&mut self)`
    // method. Populated in run() from `impl Drop for T` blocks.
    std::unordered_map<std::string, std::string> dropImpls_;
    // libc `free` extern (declared in declareBuiltins alongside malloc/realloc).
    llvm::Function* freeFn_ = nullptr;
    // Memoized droppability per mangled type name (Vec/String/HashMap/Box,
    // any type with an `impl Drop`, or any aggregate transitively owning one).
    std::unordered_map<std::string, bool> droppableCache_;
    // One tracked owning local pending drop at scope exit. `flag` is an i1
    // alloca (in the fn entry block) that is true while the value is still
    // owned by this binding and false once it has been moved away; the drop at
    // scope exit is guarded by it (the runtime "drop flag", giving correct
    // at-most-once semantics across conditional moves). `storage` is the
    // binding's alloca (a pointer to the value); `type` is its resolved
    // Kardashev type.
    struct DropLocal {
        std::string name;
        llvm::AllocaInst* storage = nullptr;
        llvm::AllocaInst* flag = nullptr;
        TypePtr type;
    };
    // Lexical scope stack of droppable locals, innermost last. Each entry is
    // the declaration-ordered list of owning locals in that scope; scope exit
    // drops them in REVERSE order (Rust semantics). A function body opens the
    // outermost scope (params live there); every BlockExpr opens a nested one.
    std::vector<std::vector<DropLocal>> dropScopes_;
    // Parallel to loopFrames_: the dropScopes_ depth at the point each loop was
    // entered, so `break`/`continue` can drop exactly the scopes opened inside
    // the loop body (indices >= this depth) without touching outer scopes.
    std::vector<std::size_t> loopDropDepth_;
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
    llvm::Function* memcpyFn_ = nullptr; // Phase 13b: String/HashMap copies
    llvm::Function* memsetFn_ = nullptr; // Phase 13b: HashMap bucket zeroing
    // Phase 17b: the HashMap bucket entry struct is now per-V (emitted lazily
    // by getOrEmitHashMapOp), so no single cached entry type lives here.

    // --- Phase 18: real async I/O + multitask executor ---
    // libc time/sleep externs the timer future + reactor depend on. Same
    // linkage story as malloc/printf (host process for JIT, clang's libc for
    // AOT). `clock_gettime(clockid_t, struct timespec*)` reads the monotonic
    // clock; `nanosleep(const struct timespec*, struct timespec*)` sleeps the
    // calling thread until the requested duration elapses (the reactor uses it
    // to wait for the nearest deadline instead of hot-spinning).
    llvm::Function* clockGettimeFn_ = nullptr;
    llvm::Function* nanosleepFn_ = nullptr;
    // `struct timespec { i64 tv_sec, i64 tv_nsec }` (LP64; matches glibc and
    // macOS where both fields are 64-bit). Used to size the buffers passed to
    // clock_gettime / nanosleep and to GEP their fields.
    llvm::StructType* timespecTy_ = nullptr;
    // The process-global executor singleton + the per-task entry struct. The
    // executor holds a growable array of tasks (each a type-erased Future +
    // its result Poll slot), the nearest pending timer deadline, and (Linux)
    // the epoll fd. Built once in declareAsyncRuntime.
    llvm::StructType* execTy_ = nullptr;
    llvm::StructType* taskTy_ = nullptr;
    llvm::GlobalVariable* execGlobal_ = nullptr;
    llvm::Function* execEnsureFn_ = nullptr;   // __kd_exec_ensure
    llvm::Function* execPushFn_ = nullptr;     // __kd_exec_push
    llvm::Function* execStepFn_ = nullptr;     // __kd_exec_step
    llvm::Function* execWaitFn_ = nullptr;     // __kd_exec_wait (reactor sleep)
    llvm::Function* execDriveFn_ = nullptr;    // __kd_exec_drive_until
    llvm::Function* execSlotFn_ = nullptr;     // __kd_exec_task_slot
#if defined(__linux__)
    // Phase 18 stretch (Linux): epoll-based fd-readiness reactor externs.
    llvm::Function* epollCreate1Fn_ = nullptr;
    llvm::Function* epollCtlFn_ = nullptr;
    llvm::Function* epollWaitFn_ = nullptr;
    llvm::Function* pipeFn_ = nullptr;
    llvm::Function* readFn_ = nullptr;
    llvm::Function* writeFn_ = nullptr;
    llvm::Function* closeFn_ = nullptr;
    llvm::Function* fcntlFn_ = nullptr;
    llvm::StructType* epollEventTy_ = nullptr;
    llvm::Function* readPipePollFn_ = nullptr; // __kd_readpipe_poll
#endif

    // --- Phase 19: OS threads + Mutex (pthread) ---
    // pthread externs. POSIX — present in libc on both glibc (folded since
    // 2.34) and macOS, so NO platform `#if` guard (unlike Phase 18's epoll).
    // Same linkage story as malloc/printf: the host kardc process for JIT
    // (it links libc/pthread), clang's libc for AOT (with -lpthread as a
    // belt-and-braces flag). `pthread_t`/`pthread_mutex_t` are opaque to us —
    // we heap-allocate generously-sized storage and only ever pass pointers /
    // the 8-byte tid by value, so we never depend on their exact layout.
    llvm::Function* pthreadCreateFn_ = nullptr;  // pthread_create
    llvm::Function* pthreadJoinFn_ = nullptr;    // pthread_join
    llvm::Function* pthreadMutexInitFn_ = nullptr;   // pthread_mutex_init
    llvm::Function* pthreadMutexLockFn_ = nullptr;   // pthread_mutex_lock
    llvm::Function* pthreadMutexUnlockFn_ = nullptr; // pthread_mutex_unlock
    // The thread control block `{ i64 tid, i8* fn, i8* env, i64 result }` and
    // the Mutex block `{ [64 x i8] pthread_mutex_t storage, i64 value }`.
    // 64 bytes covers pthread_mutex_t on both Linux (40) and macOS (64); the
    // value cell is i64 (the MVP payload). Built once in declareThreadRuntime.
    llvm::StructType* threadBlkTy_ = nullptr;
    llvm::StructType* mutexBlkTy_ = nullptr;
    llvm::Function* threadTrampolineFn_ = nullptr; // __kd_thread_trampoline

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

    // --- Phase 12 real async runtime ---
    // Poll = { i1 ready, i64 value }; the poll-fn type void(i8* frame,
    // kd.poll* out); the global poll counter; and the leaf yield poll fn.
    // `pollTy_` is the canonical `Poll<i64>` used by `yield_now`; Phase 17b
    // adds `pollTypeFor(T)` for arbitrary result types (sized via DataLayout),
    // cached in `pollTypes_` keyed by the mangled T. The poll-fn TYPE stays
    // uniform `void(ptr, ptr)` — the out-param is opaque, so each future
    // writes its own `Poll<T>` shape into a slot its caller allocated.
    llvm::StructType* pollTy_ = nullptr;
    std::unordered_map<std::string, llvm::StructType*> pollTypes_;
    llvm::FunctionType* pollFnTy_ = nullptr;
    llvm::GlobalVariable* kdPollCount_ = nullptr;
    llvm::Function* yieldPollFn_ = nullptr;       // __kd_yield_poll
    unsigned nextAsyncId_ = 0;                     // unique frame/poll names

    // State threaded through the body of the async fn currently being lowered
    // into a resumable poll function. Empty/null when not emitting an async
    // body. `asyncFrameTy_` is the heap frame struct `{ i64 state, params...,
    // promoted-locals..., Future cur_subfut }`; `asyncFramePtr_` is the i8*
    // frame argument GEPs are taken against; `asyncPollOut_` is the kd.poll*
    // out-param; `asyncSwitch_` is the entry switch that resumes at the saved
    // state (await points add cases to it); `asyncFrameIndex_` maps a
    // promoted-local / param name to its frame field index; `asyncStateIdx_`,
    // `asyncSubfutIdx_` are the fixed frame field indices for the state word
    // and the current sub-future. `asyncNextState_` hands out resume-state ids.
    bool inAsyncFn_ = false;
    llvm::StructType* asyncFrameTy_ = nullptr;
    llvm::Value* asyncFramePtr_ = nullptr;
    llvm::Value* asyncPollOut_ = nullptr;
    // Phase 17b: the `Poll<T>` layout of the async fn currently being lowered,
    // T = its declared result type. The poll fn writes Ready/Pending + value
    // into `*asyncPollOut_` through THIS shape (its block_on/await caller
    // allocated a matching slot).
    llvm::StructType* asyncPollTy_ = nullptr;
    llvm::SwitchInst* asyncSwitch_ = nullptr;
    std::unordered_map<std::string, unsigned> asyncFrameIndex_;
    std::vector<std::string> asyncPromotedLocals_; // spill/reload order
    unsigned asyncStateIdx_ = 0;
    unsigned asyncSubfutIdx_ = 0;
    int asyncNextState_ = 1; // 0 is the entry/start state

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
    // Phase 17a: create the fat-pointer fn-value type `{ i8* fn, i8* env }`
    // exactly once. Called from run() before user-struct layout (so an
    // fn-typed struct field maps to a non-null type) and again (idempotently)
    // from declareBuiltins.
    void ensureFnValTy() {
        if (fnValTy_) return;
        auto* i8PtrTy = llvm::PointerType::get(*ctx_, 0);
        fnValTy_ = llvm::StructType::create(*ctx_, {i8PtrTy, i8PtrTy},
                                            "kd.fnval");
    }

    void declareBuiltins() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
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
        // Phase 13b: memcpy for String/HashMap byte copies — libc's, same
        // linkage story as malloc/printf (host process for JIT, clang's
        // libc for AOT). Signature mirrors C `void* memcpy(dst, src, n)`.
        auto* memcpyTy = llvm::FunctionType::get(
            i8PtrTy, {i8PtrTy, i8PtrTy, i64Ty}, /*isVarArg=*/false);
        auto* memcpyFn = llvm::Function::Create(
            memcpyTy, llvm::Function::ExternalLinkage, "memcpy",
            module_.get());
        // Phase 16: `void free(void*)` — frees a heap buffer. Compiler-inserted
        // drop glue calls this when a Vec/String/HashMap/Box (or an aggregate
        // owning one) goes out of scope. Same linkage story as malloc.
        auto* freeTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {i8PtrTy}, /*isVarArg=*/false);
        auto* freeFn = llvm::Function::Create(
            freeTy, llvm::Function::ExternalLinkage, "free", module_.get());

        reallocFn_ = reallocFn;
        memcpyFn_ = memcpyFn;
        printfFn_ = printfFn;
        mallocFn_ = mallocFn; // Phase 10b: closure env allocation
        freeFn_ = freeFn;     // Phase 16: drop glue
        // Phase 19: the pthread externs + thread/Mutex runtime are declared
        // LAZILY (ensureThreadRuntime, on first use of a thread/mutex builtin)
        // so a program that never touches threads stays byte-identical (no
        // stray pthread declarations or a @free-bearing thread_join body in
        // its IR). See declareThreadRuntime below.

        // --- Phase 18: time / sleep externs for the timer future + reactor.
        // `struct timespec { time_t tv_sec; long tv_nsec; }` — both 64-bit on
        // the LP64 targets we build on (ubuntu glibc, macOS). We model it as
        // `{ i64, i64 }`; clock_gettime/nanosleep take pointers to it.
        timespecTy_ = llvm::StructType::create(ctx, {i64Ty, i64Ty},
                                               "kd.timespec");
        auto* tsPtrTy = i8PtrTy; // opaque ptr to timespec
        // int clock_gettime(clockid_t clk_id, struct timespec* tp)
        auto* clockGettimeTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, tsPtrTy}, /*isVarArg=*/false);
        clockGettimeFn_ = llvm::Function::Create(
            clockGettimeTy, llvm::Function::ExternalLinkage, "clock_gettime",
            module_.get());
        // int nanosleep(const struct timespec* req, struct timespec* rem)
        auto* nanosleepTy = llvm::FunctionType::get(
            i32Ty, {tsPtrTy, tsPtrTy}, /*isVarArg=*/false);
        nanosleepFn_ = llvm::Function::Create(
            nanosleepTy, llvm::Function::ExternalLinkage, "nanosleep",
            module_.get());
#if defined(__linux__)
        // --- Phase 18 stretch (Linux only): epoll + pipe/read/write/close for
        // the fd-readiness reactor. Guarded so the macOS build (kqueue, LLVM
        // 21) never references these Linux-only symbols. `struct epoll_event`
        // is `{ uint32_t events; epoll_data_t data; }` and is declared
        // `__attribute__((packed))` on x86-64 Linux, so its size is 12 bytes
        // (4 + 8) — we build a PACKED LLVM struct `<{ i32, i64 }>` to match the
        // kernel ABI exactly (a non-packed { i32, i64 } would be 16 bytes and
        // misalign the array epoll_wait writes into).
        epollEventTy_ = llvm::StructType::create(ctx, "kd.epoll_event");
        epollEventTy_->setBody({i32Ty, i64Ty}, /*isPacked=*/true);
        auto* epollCreate1Ty =
            llvm::FunctionType::get(i32Ty, {i32Ty}, /*isVarArg=*/false);
        epollCreate1Fn_ = llvm::Function::Create(
            epollCreate1Ty, llvm::Function::ExternalLinkage, "epoll_create1",
            module_.get());
        // int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev)
        auto* epollCtlTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, i32Ty, i32Ty, i8PtrTy}, /*isVarArg=*/false);
        epollCtlFn_ = llvm::Function::Create(
            epollCtlTy, llvm::Function::ExternalLinkage, "epoll_ctl",
            module_.get());
        // int epoll_wait(int epfd, struct epoll_event* evs, int max, int timeout)
        auto* epollWaitTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, i8PtrTy, i32Ty, i32Ty}, /*isVarArg=*/false);
        epollWaitFn_ = llvm::Function::Create(
            epollWaitTy, llvm::Function::ExternalLinkage, "epoll_wait",
            module_.get());
        // int pipe(int fds[2])
        auto* pipeTy =
            llvm::FunctionType::get(i32Ty, {i8PtrTy}, /*isVarArg=*/false);
        pipeFn_ = llvm::Function::Create(
            pipeTy, llvm::Function::ExternalLinkage, "pipe", module_.get());
        // ssize_t read(int fd, void* buf, size_t n) / write(...) / close(fd)
        auto* readTy = llvm::FunctionType::get(
            i64Ty, {i32Ty, i8PtrTy, i64Ty}, /*isVarArg=*/false);
        readFn_ = llvm::Function::Create(
            readTy, llvm::Function::ExternalLinkage, "read", module_.get());
        writeFn_ = llvm::Function::Create(
            readTy, llvm::Function::ExternalLinkage, "write", module_.get());
        auto* closeTy =
            llvm::FunctionType::get(i32Ty, {i32Ty}, /*isVarArg=*/false);
        closeFn_ = llvm::Function::Create(
            closeTy, llvm::Function::ExternalLinkage, "close", module_.get());
        // int fcntl(int fd, int cmd, ... /* int flag */) — used to flip the
        // pipe's read end to O_NONBLOCK so read_pipe's poll can probe for data
        // without ever blocking the whole reactor.
        auto* fcntlTy = llvm::FunctionType::get(
            i32Ty, {i32Ty, i32Ty}, /*isVarArg=*/true);
        fcntlFn_ = llvm::Function::Create(
            fcntlTy, llvm::Function::ExternalLinkage, "fcntl", module_.get());
#endif

        // --- Phase 10b: fat-pointer type for fn VALUES: { i8* fn, i8* env }
        // Phase 17a: may already be created by ensureFnValTy() (run() builds
        // it before user structs so an fn-typed struct field can reference
        // it); create only if not yet present so we keep a single type.
        ensureFnValTy();

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

        // --- String struct layout: { i8* data, i64 len, i64 cap }.
        // Phase 13b made String growable (mirroring Vec's {ptr,len,cap}).
        // A string literal is a read-only view: data points at an LLVM
        // private global constant, len is its byte length, and cap is 0
        // (the "borrowed / not heap-owned" marker). `string_new` allocates
        // a heap buffer (cap > 0) and `string_push_str` reallocs to grow;
        // it copies the literal's bytes in on first append so the original
        // global is never mutated. `print_str` / `str_len` only read
        // data+len, so they work uniformly over literals and grown strings.
        auto* strTy = llvm::StructType::create(
            ctx, {i8PtrTy, i64Ty, i64Ty}, "String");
        structTypes_["String"] = strTy;

        // --- Phase 13b: slice fat pointer { i8* ptr, i64 len }. Like Vec /
        // String, every `&[T]` lowers to this single layout regardless of T
        // (the element stride is computed separately at slice_get sites).
        auto* sliceTy = llvm::StructType::create(
            ctx, {i8PtrTy, i64Ty}, "Slice");
        structTypes_["Slice"] = sliceTy;

        // --- Phase 12 real async runtime ---
        // Poll = { i1 ready, i64 value }: the result of polling a future.
        // `ready` false means Pending (value ignored); true means Ready(value).
        pollTy_ = llvm::StructType::create(ctx, {i1Ty, i64Ty}, "kd.poll");
        // Future = { i8* poll, i8* frame }. `poll` is a function pointer of
        // type `void(i8* frame, kd.poll* out)`; calling it advances the
        // future by one step and writes Ready/Pending into `out`. `frame` is
        // the future's heap-allocated state (a counter for yield_now, or the
        // async-fn state-machine frame). Replaces the Phase 6 fake
        // {state,result} pair — futures are now genuinely pollable/resumable.
        auto* futureTy = llvm::StructType::create(
            ctx, {i8PtrTy, i8PtrTy}, "Future");
        structTypes_["Future"] = futureTy;
        // The poll-function type shared by every future: void(i8*, kd.poll*).
        pollFnTy_ = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {i8PtrTy, llvm::PointerType::get(ctx, 0)},
            /*isVarArg=*/false);

        // Global poll counter — incremented on every future poll (by
        // yield_now's poll fn). Lets tests observe that the Pending path is
        // taken (criterion b): poll count exceeds the number of awaits iff at
        // least one Pending was returned and re-polled. The `poll_count()`
        // builtin reads it.
        kdPollCount_ = new llvm::GlobalVariable(
            *module_, i64Ty, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i64Ty, 0), "__kd_poll_count");

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

        // --- Phase 13b: growable String runtime ---

        // string_new() -> String : an empty heap string {null, 0, 0}. The
        // first string_push_str allocates the buffer (cap 0 -> grow), so a
        // fresh String costs no allocation until something is appended.
        {
            auto* fnTy = llvm::FunctionType::get(strTy, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "string_new",
                module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* v = llvm::UndefValue::get(strTy);
            v = b.CreateInsertValue(
                v, llvm::ConstantPointerNull::get(i8PtrTy), {0}, "data");
            v = b.CreateInsertValue(v, zeroI64, {1}, "len");
            v = b.CreateInsertValue(v, zeroI64, {2}, "cap");
            b.CreateRet(v);
            declaredFns_["string_new"] = fn;
        }

        // string_push_str(s: &mut String, other: String) -> i64 : append
        // other's bytes to s, growing s's heap buffer if needed. `other` is
        // passed by value (an aggregate) so a string LITERAL — itself a
        // String value `{ptr,len,0}` — can be appended directly without an
        // explicit `&`. Growth policy: ensure cap >= newLen, doubling from a
        // base of 8. Because a literal view has cap 0 but a non-null data
        // pointer, the grow path (cap < newLen) reallocs from the OLD data
        // only when it was heap-owned; for a borrowed literal we malloc fresh
        // and memcpy the existing len bytes first, so the literal global is
        // never realloc'd.
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {i8PtrTy, strTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "string_push_str",
                module_.get());
            fn->getArg(0)->setName("s");
            fn->getArg(1)->setName("other");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* growBB = llvm::BasicBlock::Create(ctx, "grow", fn);
            auto* freshBB = llvm::BasicBlock::Create(ctx, "fresh", fn);
            auto* reuseBB = llvm::BasicBlock::Create(ctx, "reuse", fn);
            auto* copyBB = llvm::BasicBlock::Create(ctx, "copy", fn);
            llvm::IRBuilder<> b(entry);
            auto* sPtr = fn->getArg(0);
            auto* oVal = fn->getArg(1); // String aggregate, by value
            auto* sDataP = b.CreateStructGEP(strTy, sPtr, 0, "s_data_p");
            auto* sLenP = b.CreateStructGEP(strTy, sPtr, 1, "s_len_p");
            auto* sCapP = b.CreateStructGEP(strTy, sPtr, 2, "s_cap_p");
            auto* sLen = b.CreateLoad(i64Ty, sLenP, "s_len");
            auto* sCap = b.CreateLoad(i64Ty, sCapP, "s_cap");
            auto* sData = b.CreateLoad(i8PtrTy, sDataP, "s_data");
            auto* oData = b.CreateExtractValue(oVal, {0}, "o_data");
            auto* oLen = b.CreateExtractValue(oVal, {1}, "o_len");
            auto* newLen = b.CreateAdd(sLen, oLen, "new_len");
            auto* needGrow = b.CreateICmpULT(sCap, newLen, "need_grow");
            b.CreateCondBr(needGrow, growBB, copyBB);

            // grow: compute newCap = max(newLen, 8, sCap*2); then decide
            // whether the existing buffer is heap-owned (cap != 0) — realloc —
            // or a borrowed literal (cap == 0) — malloc + copy old bytes.
            b.SetInsertPoint(growBB);
            auto* eight = llvm::ConstantInt::get(i64Ty, 8);
            auto* doubled =
                b.CreateMul(sCap, llvm::ConstantInt::get(i64Ty, 2), "dbl");
            auto* atLeast8 = b.CreateSelect(
                b.CreateICmpULT(doubled, eight, "lt8"), eight, doubled,
                "atleast8");
            auto* newCap = b.CreateSelect(
                b.CreateICmpULT(atLeast8, newLen, "ltnew"), newLen, atLeast8,
                "new_cap");
            auto* wasHeap = b.CreateICmpNE(sCap, zeroI64, "was_heap");
            b.CreateCondBr(wasHeap, reuseBB, freshBB);

            // reuse: heap-owned -> realloc preserves the existing bytes.
            b.SetInsertPoint(reuseBB);
            auto* realloced =
                b.CreateCall(reallocFn_, {sData, newCap}, "realloced");
            b.CreateStore(realloced, sDataP);
            b.CreateStore(newCap, sCapP);
            b.CreateBr(copyBB);

            // fresh: borrowed literal (or empty) -> malloc, copy existing
            // sLen bytes from the old data (only if sLen > 0; memcpy with a
            // null src + 0 len is technically UB so guard it implicitly by
            // copying sLen bytes which is 0 when empty — and the old data is
            // non-null whenever sLen > 0 since a literal view always has a
            // valid pointer for its bytes).
            b.SetInsertPoint(freshBB);
            auto* mallocked =
                b.CreateCall(mallocFn_, {newCap}, "fresh_buf");
            b.CreateCall(memcpyFn_, {mallocked, sData, sLen}, "copy_old");
            b.CreateStore(mallocked, sDataP);
            b.CreateStore(newCap, sCapP);
            b.CreateBr(copyBB);

            // copy: append other's bytes at offset sLen, bump len.
            b.SetInsertPoint(copyBB);
            auto* curData = b.CreateLoad(i8PtrTy, sDataP, "cur_data");
            auto* i8Ty = llvm::Type::getInt8Ty(ctx);
            auto* dst = b.CreateGEP(i8Ty, curData, sLen, "dst");
            b.CreateCall(memcpyFn_, {dst, oData, oLen}, "append");
            b.CreateStore(newLen, sLenP);
            b.CreateRet(zeroI64);
            declaredFns_["string_push_str"] = fn;
        }

        // string_len(s: &String) -> i64 : same as str_len; provided so the
        // growable-String API has a consistent `string_*` family.
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {i8PtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "string_len",
                module_.get());
            fn->getArg(0)->setName("s");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* lenPtr = b.CreateStructGEP(strTy, fn->getArg(0), 1, "len_ptr");
            b.CreateRet(b.CreateLoad(i64Ty, lenPtr, "len"));
            declaredFns_["string_len"] = fn;
        }

        // print_string(s: &String) -> i64 : same body as print_str but a
        // distinct name so growable-string code reads uniformly.
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {i8PtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "print_string",
                module_.get());
            fn->getArg(0)->setName("s");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* dataPtr = b.CreateStructGEP(strTy, fn->getArg(0), 0, "data_ptr");
            auto* data = b.CreateLoad(i8PtrTy, dataPtr, "data");
            auto* lenPtr = b.CreateStructGEP(strTy, fn->getArg(0), 1, "len_ptr");
            auto* len = b.CreateLoad(i64Ty, lenPtr, "len");
            auto* lenI32 = b.CreateTrunc(len, i32Ty, "len_i32");
            auto* fmt = b.CreateGlobalString(
                "%.*s\n", "kd_print_string_fmt", 0, module_.get());
            b.CreateCall(printfFn, {fmt, lenI32, data});
            b.CreateRet(zeroI64);
            declaredFns_["print_string"] = fn;
        }

        // --- Phase 13b: slice read ops. A slice value is the {ptr,len} fat
        // pointer produced by `&v[a..b]` (emitSlice). Both ops take the slice
        // BY VALUE (a 16-byte aggregate). MVP element = i64; slice_get uses an
        // i64 element stride. ---
        {
            // slice_len(s: Slice) -> i64
            auto* fnTy = llvm::FunctionType::get(i64Ty, {sliceTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "slice_len",
                module_.get());
            fn->getArg(0)->setName("s");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* len = b.CreateExtractValue(fn->getArg(0), {1}, "len");
            b.CreateRet(len);
            declaredFns_["slice_len"] = fn;
        }
        {
            // slice_get(s: Slice, i: i64) -> i64 : load element i (i64 stride).
            auto* i64ElemTy = llvm::Type::getInt64Ty(ctx);
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {sliceTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "slice_get",
                module_.get());
            fn->getArg(0)->setName("s");
            fn->getArg(1)->setName("i");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* data = b.CreateExtractValue(fn->getArg(0), {0}, "data");
            auto* elemPtr =
                b.CreateGEP(i64ElemTy, data, fn->getArg(1), "elem_ptr");
            auto* val = b.CreateLoad(i64ElemTy, elemPtr, "val");
            b.CreateRet(val);
            declaredFns_["slice_get"] = fn;
        }

        // --- Phase 13b: HashMap<i64,i64> runtime (open addressing) ---
        declareHashMapRuntime();

        // --- Phase 12 async runtime ---
        declareAsyncRuntime();

        // Phase 19: the OS-thread + Mutex runtime (pthread) is declared lazily
        // on first use (ensureThreadRuntime), not here — see declareThreadRuntime.
    }

    // Phase 13b / 17b: HashMap<i64, V> open-addressing runtime.
    //
    // Layout (struct %HashMap): { i8* buckets, i64 len, i64 cap } — V-agnostic
    // (a single layout, like Vec). `buckets` is a malloc'd flat array of `cap`
    // entries; Phase 17b makes the entry per-V: { i64 state, i64 key, V value }
    // (sized via DataLayout) where state 0 = empty, 1 = occupied. Collisions
    // resolve by linear probing (slot, slot+1, ... mod cap). On insert, when
    // (len+1)*2 >= cap the table doubles and every occupied entry is
    // re-inserted (rehashed) into the new buffer. The key stays i64 (no Hash
    // trait yet).
    //
    // Only the V-agnostic `%HashMap` struct is declared eagerly here; the
    // per-V bucket-entry struct + the five ops are emitted lazily by
    // getOrEmitHashMapOp (mirrors getOrEmitVecOp).
    void declareHashMapRuntime() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        // %HashMap = { i8* buckets, i64 len, i64 cap }
        auto* hmTy = llvm::StructType::create(
            ctx, {i8PtrTy, i64Ty, i64Ty}, "HashMap");
        structTypes_["HashMap"] = hmTy;
    }

    // Phase 17b: build the concrete `Option<V>` Kardashev TypePtr by
    // instantiating the prelude's generic `Option<T>` enum schema with T=V.
    // hashmap_get<V> returns this; going through the schema keeps the variant
    // order / payload slots matching what pattern matching / emitCtorCall
    // expect for `Some`/`None`.
    TypePtr optionType(const TypePtr& V) {
        auto eit = tc_.enums.find("Option");
        if (eit == tc_.enums.end()) return nullptr;
        const EnumSchema& schema = eit->second;
        if (schema.genericVars.empty()) return schema.type;
        std::unordered_map<int, TypePtr> subst;
        subst[schema.genericVars[0]->varId] = V;
        TypePtr inst = instantiate(schema.type, subst);
        inst->typeArgs = {V};
        return inst;
    }

    // Phase 17b: lazily emit a per-value-type specialization of the HashMap
    // runtime. On first call for a given V we emit all five mangled fns
    // (`__hm_raw_insert__<V>`, `hashmap_new__<V>`, `hashmap_insert__<V>`,
    // `hashmap_get__<V>`, `hashmap_len__<V>`) over a per-V bucket entry
    // `{ i64 state, i64 key, V value }`, then return the one named by `op`.
    // Mirrors getOrEmitVecOp; the `%HashMap` struct itself stays single-layout.
    llvm::Function* getOrEmitHashMapOp(const std::string& op, const TypePtr& V) {
        std::string vMangle = mangleType(V);
        std::string want = op + "__" + vMangle;
        if (auto it = declaredFns_.find(want); it != declaredFns_.end())
            return it->second;

        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* zero = llvm::ConstantInt::get(i64Ty, 0);
        auto* one = llvm::ConstantInt::get(i64Ty, 1);
        auto* hmTy = structTypes_["HashMap"];
        auto* valTy = mapKardashevType(V);
        // entry = { i64 state, i64 key, V value }
        auto* entryTy = llvm::StructType::create(
            ctx, {i64Ty, i64Ty, valTy}, "kd.hm_entry." + vMangle);

        // __hm_raw_insert__<V>(buckets: i8*, cap: i64, k: i64, v: V) -> i64.
        // Linear-probe from k % cap; if it finds the key, overwrite value and
        // return 0 (no new slot); if it finds an empty slot, write
        // {1,k,v} and return 1 (caller bumps len). Assumes cap > 0 and at
        // least one empty slot exists (guaranteed by the load-factor grow).
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {i8PtrTy, i64Ty, i64Ty, valTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                "__hm_raw_insert__" + vMangle, module_.get());
            fn->getArg(0)->setName("buckets");
            fn->getArg(1)->setName("cap");
            fn->getArg(2)->setName("k");
            fn->getArg(3)->setName("v");
            auto* buckets = fn->getArg(0);
            auto* cap = fn->getArg(1);
            auto* k = fn->getArg(2);
            auto* v = fn->getArg(3);
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* loopBB = llvm::BasicBlock::Create(ctx, "probe", fn);
            auto* checkBB = llvm::BasicBlock::Create(ctx, "check", fn);
            auto* emptyBB = llvm::BasicBlock::Create(ctx, "empty", fn);
            auto* notEmptyBB = llvm::BasicBlock::Create(ctx, "notempty", fn);
            auto* hitBB = llvm::BasicBlock::Create(ctx, "hit", fn);
            auto* nextBB = llvm::BasicBlock::Create(ctx, "next", fn);
            llvm::IRBuilder<> b(entry);
            auto* idxSlot = b.CreateAlloca(i64Ty, nullptr, "idx");
            // start = ((k % cap) + cap) % cap  — urem keeps it in range for
            // non-negative; for negative keys the extra +cap,%cap normalizes.
            auto* rem = b.CreateSRem(k, cap, "rem");
            auto* plus = b.CreateAdd(rem, cap, "plus");
            auto* start = b.CreateURem(plus, cap, "start");
            b.CreateStore(start, idxSlot);
            b.CreateBr(loopBB);

            b.SetInsertPoint(loopBB);
            auto* idx = b.CreateLoad(i64Ty, idxSlot, "i");
            auto* eptr = b.CreateGEP(entryTy, buckets, idx, "eptr");
            auto* stateP = b.CreateStructGEP(entryTy, eptr, 0, "stateP");
            auto* state = b.CreateLoad(i64Ty, stateP, "state");
            auto* isEmpty = b.CreateICmpEQ(state, zero, "isEmpty");
            b.CreateCondBr(isEmpty, emptyBB, checkBB);

            b.SetInsertPoint(emptyBB);
            b.CreateStore(one, stateP);
            auto* keyP = b.CreateStructGEP(entryTy, eptr, 1, "keyP");
            b.CreateStore(k, keyP);
            auto* valP = b.CreateStructGEP(entryTy, eptr, 2, "valP");
            b.CreateStore(v, valP);
            b.CreateRet(one);

            b.SetInsertPoint(checkBB);
            auto* keyP2 = b.CreateStructGEP(entryTy, eptr, 1, "keyP2");
            auto* curKey = b.CreateLoad(i64Ty, keyP2, "curKey");
            auto* keyEq = b.CreateICmpEQ(curKey, k, "keyEq");
            b.CreateCondBr(keyEq, hitBB, notEmptyBB);

            b.SetInsertPoint(hitBB);
            auto* valP2 = b.CreateStructGEP(entryTy, eptr, 2, "valP2");
            b.CreateStore(v, valP2);
            b.CreateRet(zero);

            b.SetInsertPoint(notEmptyBB);
            b.CreateBr(nextBB);
            b.SetInsertPoint(nextBB);
            auto* inc = b.CreateAdd(idx, one, "inc");
            auto* wrapped = b.CreateURem(inc, cap, "wrap");
            b.CreateStore(wrapped, idxSlot);
            b.CreateBr(loopBB);
            declaredFns_["__hm_raw_insert__" + vMangle] = fn;
            (void)notEmptyBB;
        }

        // hashmap_new__<V>() -> HashMap : empty {null,0,0} (V-agnostic value).
        {
            auto* fnTy = llvm::FunctionType::get(hmTy, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                "hashmap_new__" + vMangle, module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            llvm::Value* m = llvm::UndefValue::get(hmTy);
            m = b.CreateInsertValue(
                m, llvm::ConstantPointerNull::get(i8PtrTy), {0}, "buckets");
            m = b.CreateInsertValue(m, zero, {1}, "len");
            m = b.CreateInsertValue(m, zero, {2}, "cap");
            b.CreateRet(m);
            declaredFns_["hashmap_new__" + vMangle] = fn;
        }

        // hashmap_insert__<V>(m: &mut HashMap, k: i64, v: V) -> i64. Ensures
        // capacity (grow + rehash when (len+1)*2 >= cap), then raw-inserts the
        // pair and bumps len if it occupied a fresh slot.
        {
            auto dl = module_->getDataLayout();
            uint64_t entryBytes = dl.getTypeAllocSize(entryTy);
            auto* entryBytesK = llvm::ConstantInt::get(i64Ty, entryBytes);
            auto* rawInsert = declaredFns_["__hm_raw_insert__" + vMangle];

            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {i8PtrTy, i64Ty, valTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                "hashmap_insert__" + vMangle, module_.get());
            fn->getArg(0)->setName("m");
            fn->getArg(1)->setName("k");
            fn->getArg(2)->setName("v");
            auto* mPtr = fn->getArg(0);
            auto* k = fn->getArg(1);
            auto* v = fn->getArg(2);
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* growBB = llvm::BasicBlock::Create(ctx, "grow", fn);
            auto* rehashHdr = llvm::BasicBlock::Create(ctx, "rehash.hdr", fn);
            auto* rehashBody = llvm::BasicBlock::Create(ctx, "rehash.body", fn);
            auto* rehashOcc = llvm::BasicBlock::Create(ctx, "rehash.occ", fn);
            auto* rehashStep = llvm::BasicBlock::Create(ctx, "rehash.step", fn);
            auto* rehashDone = llvm::BasicBlock::Create(ctx, "rehash.done", fn);
            auto* doInsertBB = llvm::BasicBlock::Create(ctx, "doinsert", fn);
            llvm::IRBuilder<> b(entry);
            auto* bucketsP = b.CreateStructGEP(hmTy, mPtr, 0, "bucketsP");
            auto* lenP = b.CreateStructGEP(hmTy, mPtr, 1, "lenP");
            auto* capP = b.CreateStructGEP(hmTy, mPtr, 2, "capP");
            auto* len = b.CreateLoad(i64Ty, lenP, "len");
            auto* cap = b.CreateLoad(i64Ty, capP, "cap");
            // need grow if (len+1)*2 >= cap   (covers cap==0 too).
            auto* lenPlus1 = b.CreateAdd(len, one, "lenPlus1");
            auto* twiceLen = b.CreateMul(
                lenPlus1, llvm::ConstantInt::get(i64Ty, 2), "twiceLen");
            auto* needGrow = b.CreateICmpUGE(twiceLen, cap, "needGrow");
            b.CreateCondBr(needGrow, growBB, doInsertBB);

            // grow: newCap = (cap==0 ? 8 : cap*2). malloc + zero-init new
            // buffer, then rehash existing entries into it, free old (we
            // leak instead — V1 has no free; matches Vec which never frees).
            b.SetInsertPoint(growBB);
            auto* capIsZero = b.CreateICmpEQ(cap, zero, "capIsZero");
            auto* doubled =
                b.CreateMul(cap, llvm::ConstantInt::get(i64Ty, 2), "dbl");
            auto* newCap = b.CreateSelect(
                capIsZero, llvm::ConstantInt::get(i64Ty, 8), doubled,
                "newCap");
            auto* newBytes = b.CreateMul(newCap, entryBytesK, "newBytes");
            auto* newBuf = b.CreateCall(mallocFn_, {newBytes}, "newBuf");
            // zero the new buffer so all states start empty.
            b.CreateCall(
                getOrDeclareMemset(),
                {newBuf, llvm::ConstantInt::get(
                             llvm::Type::getInt32Ty(ctx), 0),
                 newBytes},
                "");
            auto* oldBuf = b.CreateLoad(i8PtrTy, bucketsP, "oldBuf");
            // rehash loop over old [0, cap).
            auto* jSlot = b.CreateAlloca(i64Ty, nullptr, "j");
            b.CreateStore(zero, jSlot);
            b.CreateBr(rehashHdr);

            b.SetInsertPoint(rehashHdr);
            auto* j = b.CreateLoad(i64Ty, jSlot, "j.cur");
            auto* more = b.CreateICmpULT(j, cap, "more");
            b.CreateCondBr(more, rehashBody, rehashDone);

            b.SetInsertPoint(rehashBody);
            auto* oeptr = b.CreateGEP(entryTy, oldBuf, j, "oeptr");
            auto* ostateP = b.CreateStructGEP(entryTy, oeptr, 0, "ostateP");
            auto* ostate = b.CreateLoad(i64Ty, ostateP, "ostate");
            auto* occ = b.CreateICmpNE(ostate, zero, "occ");
            b.CreateCondBr(occ, rehashOcc, rehashStep);

            b.SetInsertPoint(rehashOcc);
            auto* okeyP = b.CreateStructGEP(entryTy, oeptr, 1, "okeyP");
            auto* okey = b.CreateLoad(i64Ty, okeyP, "okey");
            auto* ovalP = b.CreateStructGEP(entryTy, oeptr, 2, "ovalP");
            auto* oval = b.CreateLoad(valTy, ovalP, "oval");
            b.CreateCall(rawInsert, {newBuf, newCap, okey, oval}, "");
            b.CreateBr(rehashStep);

            b.SetInsertPoint(rehashStep);
            auto* jNext = b.CreateAdd(j, one, "jNext");
            b.CreateStore(jNext, jSlot);
            b.CreateBr(rehashHdr);

            b.SetInsertPoint(rehashDone);
            b.CreateStore(newBuf, bucketsP);
            b.CreateStore(newCap, capP);
            b.CreateBr(doInsertBB);

            // doinsert: raw-insert into current buffer; bump len if new.
            b.SetInsertPoint(doInsertBB);
            auto* curBuf = b.CreateLoad(i8PtrTy, bucketsP, "curBuf");
            auto* curCap = b.CreateLoad(i64Ty, capP, "curCap");
            auto* added =
                b.CreateCall(rawInsert, {curBuf, curCap, k, v}, "added");
            auto* curLen = b.CreateLoad(i64Ty, lenP, "curLen");
            auto* newLen = b.CreateAdd(curLen, added, "newLen");
            b.CreateStore(newLen, lenP);
            b.CreateRet(zero);
            declaredFns_["hashmap_insert__" + vMangle] = fn;
        }

        // hashmap_get__<V>(m: &HashMap, k: i64) -> Option<V> : linear-probe;
        // Some(v) if found, None if an empty slot is hit first (or cap==0). The
        // probe index lives in an alloca (`idxSlot`); `entry` seeds it (or
        // returns None for an empty map), `probe` reloads it each iteration,
        // `step` advances it and branches back.
        {
            TypePtr optTy = optionType(V);
            if (!optTy) {
                // No `Option` in scope (a self-contained unit-test fixture
                // with no prelude). The typechecker likewise skipped
                // registering `hashmap_get`, so a program here can't call it
                // — skip emitting it silently rather than erroring out.
                return nullptr;
            }
            mapKardashevType(optTy); // declare LLVM struct + payload slots
            const std::string optMangled =
                mangleStructInstance(optTy->enumName, optTy->typeArgs);
            auto* optLlvm = enumTypes_[optMangled];
            unsigned someIdx = variantIndexInEnum(optTy, "Some");
            unsigned noneIdx = variantIndexInEnum(optTy, "None");
            unsigned someSlot = enumPayloadIndices_[optMangled][someIdx][0];
            auto* i32Ty = llvm::Type::getInt32Ty(ctx);

            auto* fnTy = llvm::FunctionType::get(
                optLlvm, {i8PtrTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                "hashmap_get__" + vMangle, module_.get());
            fn->getArg(0)->setName("m");
            fn->getArg(1)->setName("k");
            auto* mPtr = fn->getArg(0);
            auto* k = fn->getArg(1);
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* loopBB = llvm::BasicBlock::Create(ctx, "probe", fn);
            auto* checkBB = llvm::BasicBlock::Create(ctx, "check", fn);
            auto* noneBB = llvm::BasicBlock::Create(ctx, "none", fn);
            auto* hitBB = llvm::BasicBlock::Create(ctx, "hit", fn);
            auto* stepBB = llvm::BasicBlock::Create(ctx, "step", fn);

            // build a None aggregate at the current insert point.
            auto buildNone = [&](llvm::IRBuilder<>& bb) {
                llvm::Value* agg = llvm::UndefValue::get(optLlvm);
                agg = bb.CreateInsertValue(
                    agg, llvm::ConstantInt::get(i32Ty, noneIdx), {0}, "none");
                return agg;
            };

            llvm::IRBuilder<> b(entry);
            auto* idxSlot = b.CreateAlloca(i64Ty, nullptr, "idx");
            auto* bucketsP = b.CreateStructGEP(hmTy, mPtr, 0, "bucketsP");
            auto* capP = b.CreateStructGEP(hmTy, mPtr, 2, "capP");
            auto* cap = b.CreateLoad(i64Ty, capP, "cap");
            auto* buckets = b.CreateLoad(i8PtrTy, bucketsP, "buckets");
            auto* capZero = b.CreateICmpEQ(cap, zero, "capZero");
            // Seed idxSlot = ((k % cap)+cap)%cap, but guard the modulo: when
            // cap==0 we'd divide by zero, so branch to None first.
            auto* nonZeroBB = llvm::BasicBlock::Create(ctx, "nonzero", fn);
            b.CreateCondBr(capZero, noneBB, nonZeroBB);
            b.SetInsertPoint(nonZeroBB);
            auto* rem = b.CreateSRem(k, cap, "rem");
            auto* plus = b.CreateAdd(rem, cap, "plus");
            auto* start = b.CreateURem(plus, cap, "start");
            b.CreateStore(start, idxSlot);
            b.CreateBr(loopBB);

            b.SetInsertPoint(loopBB);
            auto* idx = b.CreateLoad(i64Ty, idxSlot, "i");
            auto* eptr = b.CreateGEP(entryTy, buckets, idx, "eptr");
            auto* stateP = b.CreateStructGEP(entryTy, eptr, 0, "stateP");
            auto* state = b.CreateLoad(i64Ty, stateP, "state");
            auto* isEmpty = b.CreateICmpEQ(state, zero, "isEmpty");
            b.CreateCondBr(isEmpty, noneBB, checkBB);

            b.SetInsertPoint(noneBB);
            b.CreateRet(buildNone(b));

            b.SetInsertPoint(checkBB);
            auto* keyP = b.CreateStructGEP(entryTy, eptr, 1, "keyP");
            auto* curKey = b.CreateLoad(i64Ty, keyP, "curKey");
            auto* keyEq = b.CreateICmpEQ(curKey, k, "keyEq");
            b.CreateCondBr(keyEq, hitBB, stepBB);

            b.SetInsertPoint(hitBB);
            auto* valP = b.CreateStructGEP(entryTy, eptr, 2, "valP");
            auto* val = b.CreateLoad(valTy, valP, "val");
            llvm::Value* someAgg = llvm::UndefValue::get(optLlvm);
            someAgg = b.CreateInsertValue(
                someAgg, llvm::ConstantInt::get(i32Ty, someIdx), {0}, "some");
            someAgg = b.CreateInsertValue(someAgg, val, {someSlot}, "someV");
            b.CreateRet(someAgg);

            b.SetInsertPoint(stepBB);
            auto* inc = b.CreateAdd(idx, one, "inc");
            auto* wrapped = b.CreateURem(inc, cap, "wrap");
            b.CreateStore(wrapped, idxSlot);
            b.CreateBr(loopBB);
            declaredFns_["hashmap_get__" + vMangle] = fn;
        }

        // hashmap_len__<V>(m: &HashMap) -> i64 (value-agnostic, but emitted
        // per-V so the interception routes uniformly).
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {i8PtrTy}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                "hashmap_len__" + vMangle, module_.get());
            fn->getArg(0)->setName("m");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* lenP = b.CreateStructGEP(hmTy, fn->getArg(0), 1, "lenP");
            b.CreateRet(b.CreateLoad(i64Ty, lenP, "len"));
            declaredFns_["hashmap_len__" + vMangle] = fn;
        }

        // All five are now emitted for this V; return the requested one.
        auto it = declaredFns_.find(want);
        return it != declaredFns_.end() ? it->second : nullptr;
    }

    // memset extern (used to zero a freshly malloc'd HashMap bucket array).
    llvm::Function* getOrDeclareMemset() {
        if (memsetFn_) return memsetFn_;
        auto& ctx = *ctx_;
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* ty = llvm::FunctionType::get(
            i8PtrTy, {i8PtrTy, i32Ty, i64Ty}, false);
        memsetFn_ = llvm::Function::Create(
            ty, llvm::Function::ExternalLinkage, "memset", module_.get());
        return memsetFn_;
    }

    // Phase 12: materialize the cooperative-future runtime — the leaf
    // suspending future `yield_now`, the executor `block_on`, and the
    // `poll_count` observability hook. The async-fn state-machine poll
    // functions (one per async fn) are emitted later, in emitFunctionAs.
    void declareAsyncRuntime() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* futureTy = structTypes_["Future"];

        // The yield frame: { i64 count, i64 value }. `count` starts 0; the
        // first poll bumps it to 1 and returns Pending, the second returns
        // Ready(value). One genuine suspension per yield_now.
        auto* yieldFrameTy =
            llvm::StructType::create(ctx, {i64Ty, i64Ty}, "kd.yield_frame");

        // void __kd_yield_poll(i8* frame, kd.poll* out)
        {
            auto* fn = llvm::Function::Create(
                pollFnTy_, llvm::Function::InternalLinkage, "__kd_yield_poll",
                module_.get());
            fn->getArg(0)->setName("frame");
            fn->getArg(1)->setName("out");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* frame = fn->getArg(0);
            auto* out = fn->getArg(1);
            // Observe the poll globally so tests can prove suspension.
            auto* pc = b.CreateLoad(i64Ty, kdPollCount_, "poll_count");
            b.CreateStore(
                b.CreateAdd(pc, llvm::ConstantInt::get(i64Ty, 1)),
                kdPollCount_);
            auto* countPtr =
                b.CreateStructGEP(yieldFrameTy, frame, 0, "count_ptr");
            auto* count = b.CreateLoad(i64Ty, countPtr, "count");
            auto* firstPoll = b.CreateICmpEQ(
                count, llvm::ConstantInt::get(i64Ty, 0), "is_first_poll");
            auto* pendingBB = llvm::BasicBlock::Create(ctx, "pending", fn);
            auto* readyBB = llvm::BasicBlock::Create(ctx, "ready", fn);
            b.CreateCondBr(firstPoll, pendingBB, readyBB);
            // First poll: record we've suspended once, report Pending.
            b.SetInsertPoint(pendingBB);
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 1), countPtr);
            auto* rdyP = b.CreateStructGEP(pollTy_, out, 0, "ready_ptr");
            b.CreateStore(llvm::ConstantInt::getFalse(ctx), rdyP);
            b.CreateRetVoid();
            // Second+ poll: report Ready(value).
            b.SetInsertPoint(readyBB);
            auto* valPtr =
                b.CreateStructGEP(yieldFrameTy, frame, 1, "value_ptr");
            auto* val = b.CreateLoad(i64Ty, valPtr, "value");
            auto* rdyP2 = b.CreateStructGEP(pollTy_, out, 0, "ready_ptr");
            b.CreateStore(llvm::ConstantInt::getTrue(ctx), rdyP2);
            auto* outValP = b.CreateStructGEP(pollTy_, out, 1, "out_val_ptr");
            b.CreateStore(val, outValP);
            b.CreateRetVoid();
            yieldPollFn_ = fn;
        }

        // Future yield_now(i64 v): malloc a yield frame { count=0, value=v }
        // and return Future { __kd_yield_poll, frame }.
        {
            auto* fnTy = llvm::FunctionType::get(futureTy, {i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "yield_now",
                module_.get());
            fn->getArg(0)->setName("v");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            uint64_t sz =
                module_->getDataLayout().getTypeAllocSize(yieldFrameTy);
            auto* frame = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "yield_frame");
            auto* countPtr =
                b.CreateStructGEP(yieldFrameTy, frame, 0, "count_ptr");
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), countPtr);
            auto* valPtr =
                b.CreateStructGEP(yieldFrameTy, frame, 1, "value_ptr");
            b.CreateStore(fn->getArg(0), valPtr);
            llvm::Value* fut = llvm::UndefValue::get(futureTy);
            fut = b.CreateInsertValue(fut, yieldPollFn_, {0}, "fut.poll");
            fut = b.CreateInsertValue(fut, frame, {1}, "fut.frame");
            b.CreateRet(fut);
            declaredFns_["yield_now"] = fn;
        }

        // Phase 18: the process-global multitask executor (task queue +
        // round-robin scheduler + timer/fd reactor) and the `sleep_ms` timer
        // future. Must come before block_on/spawn/join, which drive it.
        declareExecutor();
        declareSleepMs();
#if defined(__linux__)
        // Phase 18 stretch (Linux/epoll): the fd-readiness primitives
        // (pipe_make / pipe_send / read_pipe). macOS (kqueue) is deferred.
        declareFdReactor();
#endif

        // i64 block_on__i64(Future f): the i64 specialization of the executor
        // driver. Phase 18: instead of busy-polling f alone, it enqueues f as
        // a task and drives the WHOLE executor (round-robin over f plus any
        // spawned tasks, sleeping in the reactor when everyone is Pending)
        // until f completes, then returns f's result. Registered under both
        // the mangled name and the bare `block_on` alias for back-compat;
        // getOrEmitBlockOn emits the same shape for other result types.
        {
            llvm::Function* fn = emitBlockOnBody("block_on__i64", makeInt());
            declaredFns_["block_on__i64"] = fn;
            declaredFns_["block_on"] = fn;
        }

        // i64 poll_count(): read the global poll counter (observability).
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "poll_count",
                module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(b.CreateLoad(i64Ty, kdPollCount_, "pc"));
            declaredFns_["poll_count"] = fn;
        }
    }

    // ====================================================================
    // Phase 19: OS threads + Mutex on pthread.
    // ====================================================================
    //
    // Threads. A kardashev fn VALUE is the fat pointer `{ i8* fn, i8* env }`
    // (Phase 10b); calling it is `fn(env)` (the env-calling convention with
    // zero further args, since `thread_spawn` takes `fn() -> i64`). We can't
    // hand that pair straight to pthread_create (its start routine is the C
    // ABI `void* (*)(void*)`), so we generate ONE trampoline that adapts it:
    //
    //   void* __kd_thread_trampoline(void* arg):
    //       blk = (kd.thread_blk*)arg
    //       r   = ((i64 (*)(i8*))blk->fn)(blk->env)   // run the closure
    //       blk->result = r                            // stash the i64 result
    //       return null
    //
    // `thread_spawn({fn,env})` heap-allocates the control block
    // `{ i64 tid, i8* fn, i8* env, i64 result }`, stores fn+env, then
    // pthread_create(&blk->tid, null, trampoline, blk) and returns (i64)blk as
    // an opaque handle. `thread_join(h)` casts h back, pthread_join(blk->tid,
    // null), reads blk->result, frees the block, and returns the result. The
    // i64 result therefore flows: closure return -> trampoline -> block slot ->
    // join return. The control block leaks only if a thread is spawned but
    // never joined (documented; never a double-free / UAF).
    //
    // Mutex<i64> (handle-based). `mutex_new(v)` heap-allocates
    // `{ [64 x i8] mtx, i64 value }` (64 bytes covers pthread_mutex_t on glibc
    // (40) and macOS (64)), pthread_mutex_init(&mtx, null), stores v, and
    // returns (i64)blk. The handle is a plain i64 — therefore Copy — so it can
    // be captured BY VALUE into each thread's closure, giving every thread the
    // same underlying lock + cell (the sharing mechanism). mutex_lock/unlock
    // call pthread_mutex_lock/unlock(&mtx); mutex_get/mutex_set read/write the
    // value cell (the caller is expected to hold the lock). Two threads each
    // doing { lock; set(get()+1); unlock } N times therefore land on exactly
    // 2N with mutual exclusion (an unsynchronized get/set races and loses
    // updates -> < 2N).
    //
    // Declared LAZILY (idempotent guard) on the first use of any thread/mutex
    // builtin — emitCall calls ensureThreadRuntime() before routing to the
    // declaredFns_ entry. This keeps a thread-free program's IR unchanged (no
    // pthread externs, no @free-bearing thread_join body).
    bool threadRuntimeDeclared_ = false;
    void ensureThreadRuntime() {
        if (threadRuntimeDeclared_) return;
        threadRuntimeDeclared_ = true;
        declareThreadRuntime();
    }
    void declareThreadRuntime() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* voidTy = llvm::Type::getVoidTy(ctx);

        // --- pthread externs (declared here, lazily). POSIX, so NO platform
        // guard. Pointers are opaque `i8*`; `pthread_t` is an 8-byte handle on
        // both glibc (unsigned long) and macOS (a pointer), modeled as i64 by
        // value for pthread_join and `&tid` (i64*) for pthread_create.
        //   int pthread_create(pthread_t*, const attr*, void*(*)(void*), void*)
        pthreadCreateFn_ = llvm::Function::Create(
            llvm::FunctionType::get(
                i32Ty, {i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy}, false),
            llvm::Function::ExternalLinkage, "pthread_create", module_.get());
        //   int pthread_join(pthread_t, void** retval)
        pthreadJoinFn_ = llvm::Function::Create(
            llvm::FunctionType::get(i32Ty, {i64Ty, i8PtrTy}, false),
            llvm::Function::ExternalLinkage, "pthread_join", module_.get());
        //   int pthread_mutex_init(pthread_mutex_t*, const attr*)
        pthreadMutexInitFn_ = llvm::Function::Create(
            llvm::FunctionType::get(i32Ty, {i8PtrTy, i8PtrTy}, false),
            llvm::Function::ExternalLinkage, "pthread_mutex_init",
            module_.get());
        //   int pthread_mutex_lock/unlock(pthread_mutex_t*)
        auto* pmLockTy = llvm::FunctionType::get(i32Ty, {i8PtrTy}, false);
        pthreadMutexLockFn_ = llvm::Function::Create(
            pmLockTy, llvm::Function::ExternalLinkage, "pthread_mutex_lock",
            module_.get());
        pthreadMutexUnlockFn_ = llvm::Function::Create(
            pmLockTy, llvm::Function::ExternalLinkage, "pthread_mutex_unlock",
            module_.get());

        // Control-block types.
        threadBlkTy_ = llvm::StructType::create(
            ctx, {i64Ty, i8PtrTy, i8PtrTy, i64Ty}, "kd.thread_blk");
        auto* mtxStorageTy = llvm::ArrayType::get(
            llvm::Type::getInt8Ty(ctx), 64);
        mutexBlkTy_ = llvm::StructType::create(
            ctx, {mtxStorageTy, i64Ty}, "kd.mutex_blk");
        // The closure's fn pointer has the env-calling-convention type for a
        // `fn() -> i64` value: i64(i8* env).
        auto* closureCallTy =
            llvm::FunctionType::get(i64Ty, {i8PtrTy}, /*isVarArg=*/false);

        // void* __kd_thread_trampoline(void* arg)
        {
            auto* fnTy =
                llvm::FunctionType::get(i8PtrTy, {i8PtrTy}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage,
                "__kd_thread_trampoline", module_.get());
            fn->getArg(0)->setName("arg");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* blk = fn->getArg(0);
            auto* fnP = b.CreateLoad(
                i8PtrTy, b.CreateStructGEP(threadBlkTy_, blk, 1, "fn_ptr"),
                "fn");
            auto* envP = b.CreateLoad(
                i8PtrTy, b.CreateStructGEP(threadBlkTy_, blk, 2, "env_ptr"),
                "env");
            auto* r = b.CreateCall(closureCallTy, fnP, {envP}, "thread_ret");
            b.CreateStore(
                r, b.CreateStructGEP(threadBlkTy_, blk, 3, "result_ptr"));
            b.CreateRet(llvm::ConstantPointerNull::get(i8PtrTy));
            threadTrampolineFn_ = fn;
        }

        // i64 thread_spawn({ i8* fn, i8* env })
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {fnValTy_}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "thread_spawn",
                module_.get());
            fn->getArg(0)->setName("f");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* fnPtr = b.CreateExtractValue(fn->getArg(0), {0}, "f.fn");
            auto* envPtr = b.CreateExtractValue(fn->getArg(0), {1}, "f.env");
            uint64_t sz =
                module_->getDataLayout().getTypeAllocSize(threadBlkTy_);
            auto* blk = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "thread_blk");
            b.CreateStore(
                fnPtr, b.CreateStructGEP(threadBlkTy_, blk, 1, "fn_slot"));
            b.CreateStore(
                envPtr, b.CreateStructGEP(threadBlkTy_, blk, 2, "env_slot"));
            auto* tidPtr =
                b.CreateStructGEP(threadBlkTy_, blk, 0, "tid_ptr");
            b.CreateCall(pthreadCreateFn_,
                         {tidPtr,
                          llvm::ConstantPointerNull::get(i8PtrTy),
                          threadTrampolineFn_, blk});
            b.CreateRet(b.CreatePtrToInt(blk, i64Ty, "handle"));
            declaredFns_["thread_spawn"] = fn;
        }

        // i64 thread_join(i64 handle)
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i64Ty}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "thread_join",
                module_.get());
            fn->getArg(0)->setName("handle");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* blk = b.CreateIntToPtr(fn->getArg(0), i8PtrTy, "blk");
            auto* tid = b.CreateLoad(
                i64Ty, b.CreateStructGEP(threadBlkTy_, blk, 0, "tid_ptr"),
                "tid");
            b.CreateCall(pthreadJoinFn_,
                         {tid, llvm::ConstantPointerNull::get(i8PtrTy)});
            auto* result = b.CreateLoad(
                i64Ty, b.CreateStructGEP(threadBlkTy_, blk, 3, "result_ptr"),
                "result");
            b.CreateCall(freeFn_, {blk});
            b.CreateRet(result);
            declaredFns_["thread_join"] = fn;
        }

        // i64 mutex_new(i64 v)
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i64Ty}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "mutex_new",
                module_.get());
            fn->getArg(0)->setName("v");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            uint64_t sz =
                module_->getDataLayout().getTypeAllocSize(mutexBlkTy_);
            auto* blk = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "mutex_blk");
            auto* mtxPtr =
                b.CreateStructGEP(mutexBlkTy_, blk, 0, "mtx_ptr");
            b.CreateCall(pthreadMutexInitFn_,
                         {mtxPtr, llvm::ConstantPointerNull::get(i8PtrTy)});
            b.CreateStore(
                fn->getArg(0),
                b.CreateStructGEP(mutexBlkTy_, blk, 1, "value_slot"));
            b.CreateRet(b.CreatePtrToInt(blk, i64Ty, "handle"));
            declaredFns_["mutex_new"] = fn;
        }

        // i64 mutex_lock(i64 h) / i64 mutex_unlock(i64 h) — lock/unlock and
        // return 0 (an i64 so it composes in expression position).
        auto emitMutexLockOp = [&](const char* name, llvm::Function* op) {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i64Ty}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, name, module_.get());
            fn->getArg(0)->setName("h");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* blk = b.CreateIntToPtr(fn->getArg(0), i8PtrTy, "blk");
            auto* mtxPtr = b.CreateStructGEP(mutexBlkTy_, blk, 0, "mtx_ptr");
            b.CreateCall(op, {mtxPtr});
            b.CreateRet(llvm::ConstantInt::get(i64Ty, 0));
            declaredFns_[name] = fn;
        };
        emitMutexLockOp("mutex_lock", pthreadMutexLockFn_);
        emitMutexLockOp("mutex_unlock", pthreadMutexUnlockFn_);

        // i64 mutex_get(i64 h): read the guarded cell.
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i64Ty}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "mutex_get",
                module_.get());
            fn->getArg(0)->setName("h");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* blk = b.CreateIntToPtr(fn->getArg(0), i8PtrTy, "blk");
            auto* v = b.CreateLoad(
                i64Ty, b.CreateStructGEP(mutexBlkTy_, blk, 1, "value_ptr"),
                "value");
            b.CreateRet(v);
            declaredFns_["mutex_get"] = fn;
        }

        // i64 mutex_set(i64 h, i64 v): write the guarded cell, return v.
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {i64Ty, i64Ty}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "mutex_set",
                module_.get());
            fn->getArg(0)->setName("h");
            fn->getArg(1)->setName("v");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* blk = b.CreateIntToPtr(fn->getArg(0), i8PtrTy, "blk");
            b.CreateStore(
                fn->getArg(1),
                b.CreateStructGEP(mutexBlkTy_, blk, 1, "value_ptr"));
            (void)voidTy;
            b.CreateRet(fn->getArg(1));
            declaredFns_["mutex_set"] = fn;
        }
    }

    // Phase 17b: the per-result-type `Poll<T> = { i1 ready, T value }`. Built
    // lazily and cached by mangled T. `Poll<i64>` aliases the canonical
    // `pollTy_`. Used to size + GEP the value slot at every block_on / await /
    // async-fn-finish site for a future whose result is T.
    llvm::StructType* pollTypeFor(const TypePtr& T) {
        std::string m = mangleType(T);
        // i64 and unit both use the canonical `Poll<i64>` value slot: i64 is
        // the common case, and a unit-bodied async fn reports Ready(0) into an
        // i64 slot (void can't be a struct field). Everything else gets a
        // dedicated `{ i1, T }` shape sized via DataLayout at use sites.
        if (m == "i64" || m == "unit") return pollTy_;
        auto it = pollTypes_.find(m);
        if (it != pollTypes_.end()) return it->second;
        auto& ctx = *ctx_;
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* valTy = mapKardashevType(T);
        auto* pt = llvm::StructType::create(ctx, {i1Ty, valTy}, "kd.poll." + m);
        pollTypes_[m] = pt;
        return pt;
    }

    // Phase 17b/18: lazily emit a per-result-type specialization of `block_on`.
    // Mangled `block_on__<T>` so emitCall's generic-callee branch routes to it
    // (mirrors getOrEmitVecOp). Phase 18 drives the whole executor (not just
    // this future) until it completes, so spawned tasks make progress and the
    // reactor sleeps when everyone is Pending. `block_on__i64` is emitted
    // eagerly in declareAsyncRuntime and aliased to the bare `block_on` name.
    llvm::Function* getOrEmitBlockOn(const TypePtr& T) {
        std::string mangled = "block_on__" + mangleType(T);
        auto it = declaredFns_.find(mangled);
        if (it != declaredFns_.end()) return it->second;
        llvm::Function* fn = emitBlockOnBody(mangled, T);
        declaredFns_[mangled] = fn;
        return fn;
    }

    // ===================================================================
    // Phase 18: real async I/O + multitask executor
    // ===================================================================
    //
    // The runtime model upgrades the Phase 12 single-task busy-poll into a
    // cooperative scheduler with a real timer/fd reactor:
    //
    //   * A process-global executor (`__kd_exec`) owns a growable array of
    //     tasks. A task is a type-erased Future `{poll,frame}` plus a
    //     heap `poll_slot` (a `Poll<T>` the SPAWNER sized — the executor only
    //     ever reads field 0, the `ready` flag at offset 0, so it stays
    //     T-agnostic; block_on/join read the value through their own Poll<T>).
    //   * `spawn(f)` enqueues a task, returns an i64 handle (= task index).
    //   * `block_on(f)` / `join(h)` enqueue (block_on) then DRIVE the whole
    //     executor round-robin until the target task is done, returning its T.
    //     Driving everyone is what makes spawned tasks interleave.
    //   * The reactor: each round resets `has_dl`; a timer future that returns
    //     Pending registers its deadline (min) via __kd_exec_set_deadline. If
    //     a full round makes no task ready and a deadline is registered, the
    //     executor sleeps (nanosleep, or epoll_wait on Linux when fds are
    //     registered) until the nearest deadline rather than hot-spinning.

    // Field indices into the global executor struct `__kd_exec`.
    enum {
        EXEC_TASKS = 0,   // i8* — malloc'd array of Task
        EXEC_COUNT = 1,   // i64 — live task count
        EXEC_CAP = 2,     // i64 — array capacity (in Tasks)
        EXEC_DL_SEC = 3,  // i64 — nearest pending deadline, seconds
        EXEC_DL_NSEC = 4, // i64 — nearest pending deadline, nanoseconds
        EXEC_HAS_DL = 5,  // i64 — 1 iff a deadline was registered this round
        EXEC_EPFD = 6,    // i32 — epoll fd (Linux), -1 if none
        EXEC_NFDS = 7     // i32 — count of fds registered with epoll
    };
    // Field indices into a Task entry.
    enum {
        TASK_POLL = 0,  // i8* — the future's poll fn
        TASK_FRAME = 1, // i8* — the future's heap frame
        TASK_DONE = 2,   // i64 — 1 once Ready
        TASK_SLOT = 3,   // i8* — the spawner-sized Poll<T> result slot
        TASK_TYPE = 4    // i64 — compile-time type tag for join<T>
    };

    // Build the executor types + global + scheduler/reactor functions. Called
    // once from declareAsyncRuntime. All functions are InternalLinkage IR; no
    // C++ runtime file is required (mirrors how yield_now/print are emitted).
    void declareExecutor() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* voidTy = llvm::Type::getVoidTy(ctx);

        taskTy_ = llvm::StructType::create(
            ctx, {i8PtrTy, i8PtrTy, i64Ty, i8PtrTy, i64Ty}, "kd.task");
        execTy_ = llvm::StructType::create(
            ctx,
            {i8PtrTy, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty, i32Ty, i32Ty},
            "kd.exec");
        // The singleton, zero-initialized: count=cap=0, has_dl=0. epfd starts 0
        // here and is set to -1 lazily by __kd_exec_ensure (0 is a valid fd, so
        // we can't use it as the "no epoll yet" sentinel from the zeroinit).
        execGlobal_ = new llvm::GlobalVariable(
            *module_, execTy_, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantAggregateZero::get(execTy_), "__kd_exec");

        auto* execPtrTy = i8PtrTy; // opaque ptr to __kd_exec / Task array

        // ---- void __kd_exec_ensure(): lazily mark the executor initialized.
        // Sets epfd to -1 the first time (cap is the init sentinel: while it's
        // 0 we treat tasks as absent; epfd's -1 means "no epoll created yet").
        {
            auto* fnTy = llvm::FunctionType::get(voidTy, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage, "__kd_exec_ensure",
                module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* initBB = llvm::BasicBlock::Create(ctx, "init", fn);
            auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
            llvm::IRBuilder<> b(entry);
            auto* epfdP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_EPFD,
                                            "epfd_ptr");
            auto* epfd = b.CreateLoad(i32Ty, epfdP, "epfd");
            // epfd==0 means "never initialized" (the zeroinit value); set -1.
            auto* uninit = b.CreateICmpEQ(
                epfd, llvm::ConstantInt::get(i32Ty, 0), "uninit");
            b.CreateCondBr(uninit, initBB, doneBB);
            b.SetInsertPoint(initBB);
            b.CreateStore(llvm::ConstantInt::get(i32Ty, -1), epfdP);
            b.CreateBr(doneBB);
            b.SetInsertPoint(doneBB);
            b.CreateRetVoid();
            execEnsureFn_ = fn;
        }

        // ---- i8* __kd_exec_task_slot(i64 idx): return a pointer to Task[idx].
        {
            auto* fnTy =
                llvm::FunctionType::get(i8PtrTy, {i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage, "__kd_exec_task_slot",
                module_.get());
            fn->getArg(0)->setName("idx");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* tasksP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_TASKS,
                                             "tasks_ptr");
            auto* tasks = b.CreateLoad(i8PtrTy, tasksP, "tasks");
            auto* slot = b.CreateGEP(taskTy_, tasks, {fn->getArg(0)},
                                     "task_slot");
            b.CreateRet(slot);
            execSlotFn_ = fn;
        }

        // ---- i64 __kd_exec_push(i8* poll, i8* frame, i8* poll_slot):
        // append a task; grow the array if needed; return its index (handle).
        {
            auto* fnTy = llvm::FunctionType::get(
                i64Ty, {i8PtrTy, i8PtrTy, i8PtrTy, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage, "__kd_exec_push",
                module_.get());
            fn->getArg(0)->setName("poll");
            fn->getArg(1)->setName("frame");
            fn->getArg(2)->setName("slot");
            fn->getArg(3)->setName("type_tag");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* growBB = llvm::BasicBlock::Create(ctx, "grow", fn);
            auto* storeBB = llvm::BasicBlock::Create(ctx, "store", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateCall(execEnsureFn_, {});
            auto* countP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_COUNT,
                                             "count_ptr");
            auto* count = b.CreateLoad(i64Ty, countP, "count");
            auto* capP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_CAP,
                                           "cap_ptr");
            auto* cap = b.CreateLoad(i64Ty, capP, "cap");
            auto* full = b.CreateICmpEQ(count, cap, "full");
            b.CreateCondBr(full, growBB, storeBB);
            // grow: newcap = cap==0 ? 4 : cap*2; realloc(tasks, newcap*sizeof).
            b.SetInsertPoint(growBB);
            auto* capZero = b.CreateICmpEQ(
                cap, llvm::ConstantInt::get(i64Ty, 0), "cap_zero");
            auto* dbl = b.CreateMul(cap, llvm::ConstantInt::get(i64Ty, 2),
                                    "cap_dbl");
            auto* newCap = b.CreateSelect(
                capZero, llvm::ConstantInt::get(i64Ty, 4), dbl, "new_cap");
            uint64_t taskSz =
                module_->getDataLayout().getTypeAllocSize(taskTy_);
            auto* bytes = b.CreateMul(
                newCap, llvm::ConstantInt::get(i64Ty, taskSz), "new_bytes");
            auto* tasksP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_TASKS,
                                             "tasks_ptr");
            auto* oldTasks = b.CreateLoad(i8PtrTy, tasksP, "old_tasks");
            auto* newTasks =
                b.CreateCall(reallocFn_, {oldTasks, bytes}, "new_tasks");
            b.CreateStore(newTasks, tasksP);
            b.CreateStore(newCap, capP);
            b.CreateBr(storeBB);
            // store: Task[count] = { poll, frame, done=0, slot }; count++.
            b.SetInsertPoint(storeBB);
            auto* slot = b.CreateCall(execSlotFn_, {count}, "slot");
            b.CreateStore(fn->getArg(0),
                          b.CreateStructGEP(taskTy_, slot, TASK_POLL, "p"));
            b.CreateStore(fn->getArg(1),
                          b.CreateStructGEP(taskTy_, slot, TASK_FRAME, "f"));
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0),
                          b.CreateStructGEP(taskTy_, slot, TASK_DONE, "d"));
            b.CreateStore(fn->getArg(2),
                          b.CreateStructGEP(taskTy_, slot, TASK_SLOT, "s"));
            b.CreateStore(fn->getArg(3),
                          b.CreateStructGEP(taskTy_, slot, TASK_TYPE, "t"));
            b.CreateStore(
                b.CreateAdd(count, llvm::ConstantInt::get(i64Ty, 1)), countP);
            b.CreateRet(count);
            execPushFn_ = fn;
        }

        // ---- i64 __kd_exec_step(): poll every not-done task once
        // (round-robin). Returns the number of tasks polled that returned
        // Pending this round (0 => nothing left pending, so the driver can
        // stop or doesn't need to sleep). Clears has_dl before the round so a
        // timer that stays Pending re-registers the freshest deadline.
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage, "__kd_exec_step",
                module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* loopBB = llvm::BasicBlock::Create(ctx, "loop", fn);
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "body", fn);
            auto* pollBB = llvm::BasicBlock::Create(ctx, "poll", fn);
            auto* readyBB = llvm::BasicBlock::Create(ctx, "ready", fn);
            auto* pendBB = llvm::BasicBlock::Create(ctx, "pending", fn);
            auto* contBB = llvm::BasicBlock::Create(ctx, "cont", fn);
            auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
            llvm::IRBuilder<> b(entry);
            // pending counter + reset has_dl.
            auto* pendP = b.CreateAlloca(i64Ty, nullptr, "pending");
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), pendP);
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0),
                          b.CreateStructGEP(execTy_, execGlobal_, EXEC_HAS_DL,
                                            "has_dl_ptr"));
            auto* iP = b.CreateAlloca(i64Ty, nullptr, "i");
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), iP);
            b.CreateBr(loopBB);
            b.SetInsertPoint(loopBB);
            auto* i = b.CreateLoad(i64Ty, iP, "i");
            auto* count = b.CreateLoad(
                i64Ty,
                b.CreateStructGEP(execTy_, execGlobal_, EXEC_COUNT, "cnt_ptr"),
                "count");
            auto* more = b.CreateICmpULT(i, count, "more");
            b.CreateCondBr(more, bodyBB, doneBB);
            // body: skip done tasks, else poll.
            b.SetInsertPoint(bodyBB);
            auto* slot = b.CreateCall(execSlotFn_, {i}, "slot");
            auto* doneP = b.CreateStructGEP(taskTy_, slot, TASK_DONE, "done_p");
            auto* done = b.CreateLoad(i64Ty, doneP, "done");
            auto* isDone = b.CreateICmpNE(
                done, llvm::ConstantInt::get(i64Ty, 0), "is_done");
            b.CreateCondBr(isDone, contBB, pollBB);
            // poll this task into its own poll_slot.
            b.SetInsertPoint(pollBB);
            auto* pollFn = b.CreateLoad(
                i8PtrTy, b.CreateStructGEP(taskTy_, slot, TASK_POLL, "pp"),
                "poll");
            auto* frame = b.CreateLoad(
                i8PtrTy, b.CreateStructGEP(taskTy_, slot, TASK_FRAME, "fp"),
                "frame");
            auto* pslot = b.CreateLoad(
                i8PtrTy, b.CreateStructGEP(taskTy_, slot, TASK_SLOT, "sp"),
                "pslot");
            b.CreateCall(pollFnTy_, pollFn, {frame, pslot});
            // ready flag is field 0 of Poll<T> — offset 0 for every T.
            auto* rdy = b.CreateLoad(
                i1Ty, b.CreateStructGEP(pollTy_, pslot, 0, "rdy_p"), "rdy");
            b.CreateCondBr(rdy, readyBB, pendBB);
            // ready: mark the task done (drops it from future rotation).
            // Re-fetch the slot: the poll may have spawned a new task and
            // grown (realloc'd) the array, invalidating the pre-poll `slot`.
            b.SetInsertPoint(readyBB);
            auto* slot2 = b.CreateCall(execSlotFn_, {i}, "slot2");
            b.CreateStore(
                llvm::ConstantInt::get(i64Ty, 1),
                b.CreateStructGEP(taskTy_, slot2, TASK_DONE, "done_p2"));
            b.CreateBr(contBB);
            // pending: count it (the driver uses this to decide to sleep/loop).
            b.SetInsertPoint(pendBB);
            auto* pend = b.CreateLoad(i64Ty, pendP, "pend");
            b.CreateStore(
                b.CreateAdd(pend, llvm::ConstantInt::get(i64Ty, 1)), pendP);
            b.CreateBr(contBB);
            b.SetInsertPoint(contBB);
            b.CreateStore(
                b.CreateAdd(i, llvm::ConstantInt::get(i64Ty, 1)), iP);
            b.CreateBr(loopBB);
            b.SetInsertPoint(doneBB);
            b.CreateRet(b.CreateLoad(i64Ty, pendP, "pending_final"));
            execStepFn_ = fn;
        }

        declareExecSetDeadline();
        declareExecWait();
        declareExecDrive(execPtrTy);
    }

    // ---- void __kd_exec_set_deadline(i64 sec, i64 nsec): register a timer
    // deadline; keeps the EARLIEST of any deadlines registered this round so
    // the reactor sleeps only until the nearest one.
    void declareExecSetDeadline() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* fnTy = llvm::FunctionType::get(voidTy, {i64Ty, i64Ty}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage, "__kd_exec_set_deadline",
            module_.get());
        fn->getArg(0)->setName("sec");
        fn->getArg(1)->setName("nsec");
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* setBB = llvm::BasicBlock::Create(ctx, "set", fn);
        auto* cmpBB = llvm::BasicBlock::Create(ctx, "cmp", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        llvm::IRBuilder<> b(entry);
        auto* hasP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_HAS_DL,
                                       "has_dl_ptr");
        auto* secP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_DL_SEC,
                                       "dl_sec_ptr");
        auto* nsecP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_DL_NSEC,
                                        "dl_nsec_ptr");
        auto* has = b.CreateLoad(i64Ty, hasP, "has");
        auto* none = b.CreateICmpEQ(
            has, llvm::ConstantInt::get(i64Ty, 0), "none_yet");
        b.CreateCondBr(none, setBB, cmpBB);
        b.SetInsertPoint(setBB);
        b.CreateStore(llvm::ConstantInt::get(i64Ty, 1), hasP);
        b.CreateStore(fn->getArg(0), secP);
        b.CreateStore(fn->getArg(1), nsecP);
        b.CreateBr(doneBB);
        // cmp: keep min(existing, new) as nanoseconds-since-epoch comparison.
        b.SetInsertPoint(cmpBB);
        auto* curSec = b.CreateLoad(i64Ty, secP, "cur_sec");
        auto* curNsec = b.CreateLoad(i64Ty, nsecP, "cur_nsec");
        auto* newEarlierSec = b.CreateICmpSLT(fn->getArg(0), curSec, "ne_sec");
        auto* eqSec = b.CreateICmpEQ(fn->getArg(0), curSec, "eq_sec");
        auto* newEarlierNsec =
            b.CreateICmpSLT(fn->getArg(1), curNsec, "ne_nsec");
        auto* earlier = b.CreateOr(
            newEarlierSec, b.CreateAnd(eqSec, newEarlierNsec), "earlier");
        auto* updBB = llvm::BasicBlock::Create(ctx, "upd", fn);
        b.CreateCondBr(earlier, updBB, doneBB);
        b.SetInsertPoint(updBB);
        b.CreateStore(fn->getArg(0), secP);
        b.CreateStore(fn->getArg(1), nsecP);
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();
        // not cached in a member; looked up by name where needed.
        declaredFns_["__kd_exec_set_deadline"] = fn;
    }

    // ---- void __kd_exec_wait(): the reactor's "sleep, don't spin". Called by
    // the driver only when a full round made NO task ready but at least one
    // task is still pending on a timer (has_dl) and/or an fd (Linux epoll).
    //   * timer only: nanosleep the remaining `deadline - now`.
    //   * fds only (Linux): block in epoll_wait with timeout = -1 (forever)
    //     until a registered fd is readable.
    //   * timer + fds (Linux): epoll_wait with timeout = remaining ms (whoever
    //     fires first wakes us).
    //   * non-Linux with no timer: return immediately (defensive; the driver
    //     only reaches here with has_dl set on macOS, since fds aren't a
    //     thing there).
    void declareExecWait() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* fnTy = llvm::FunctionType::get(voidTy, {}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage, "__kd_exec_wait",
            module_.get());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* retBB = llvm::BasicBlock::Create(ctx, "ret", fn);
        llvm::IRBuilder<> b(entry);
        auto* hasDl = b.CreateLoad(
            i64Ty,
            b.CreateStructGEP(execTy_, execGlobal_, EXEC_HAS_DL, "hdp"),
            "has_dl");
        auto* haveDl = b.CreateICmpNE(
            hasDl, llvm::ConstantInt::get(i64Ty, 0), "have_dl");

        // Compute remaining = deadline - now (only meaningful when haveDl);
        // also a "past" flag so an already-elapsed timer doesn't sleep.
        auto* nowTs = b.CreateAlloca(timespecTy_, nullptr, "now_ts");
        b.CreateCall(clockGettimeFn_,
                     {llvm::ConstantInt::get(i32Ty, kMonotonicClockId), nowTs});
        auto* nowSec = b.CreateLoad(
            i64Ty, b.CreateStructGEP(timespecTy_, nowTs, 0, "now_sec_p"),
            "now_sec");
        auto* nowNsec = b.CreateLoad(
            i64Ty, b.CreateStructGEP(timespecTy_, nowTs, 1, "now_nsec_p"),
            "now_nsec");
        auto* dlSec = b.CreateLoad(
            i64Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_DL_SEC, "dlsp"),
            "dl_sec");
        auto* dlNsec = b.CreateLoad(
            i64Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_DL_NSEC, "dlnp"),
            "dl_nsec");
        auto* rsec = b.CreateSub(dlSec, nowSec, "rsec");
        auto* rnsec = b.CreateSub(dlNsec, nowNsec, "rnsec");
        auto* neg = b.CreateICmpSLT(rnsec, llvm::ConstantInt::get(i64Ty, 0),
                                    "nsec_neg");
        auto* rnsecB = b.CreateSelect(
            neg, b.CreateAdd(rnsec, llvm::ConstantInt::get(i64Ty, 1000000000)),
            rnsec, "rnsec_b");
        auto* rsecB = b.CreateSelect(
            neg, b.CreateSub(rsec, llvm::ConstantInt::get(i64Ty, 1)), rsec,
            "rsec_b");
        auto* past = b.CreateICmpSLT(rsecB, llvm::ConstantInt::get(i64Ty, 0),
                                     "past");
        // timer-elapsed-already => return without sleeping (re-poll now).
        auto* timerPast = b.CreateAnd(haveDl, past, "timer_past");
#if defined(__linux__)
        // Linux: branch on whether any fds are registered.
        auto* nfds = b.CreateLoad(
            i32Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_NFDS, "nfdsp"),
            "nfds");
        auto* haveFds = b.CreateICmpSGT(
            nfds, llvm::ConstantInt::get(i32Ty, 0), "have_fds");
        auto* epollBB = llvm::BasicBlock::Create(ctx, "epoll", fn);
        auto* nanoChkBB = llvm::BasicBlock::Create(ctx, "nano_chk", fn);
        // If a timer already elapsed AND no fds, just return (re-poll). If fds
        // exist we still want to drain readiness, so go to epoll even if the
        // timer is past (epoll with timeout 0 = non-blocking poll).
        auto* skip = b.CreateAnd(timerPast, b.CreateNot(haveFds), "skip");
        auto* doWaitBB = llvm::BasicBlock::Create(ctx, "do_wait", fn);
        b.CreateCondBr(skip, retBB, doWaitBB);
        b.SetInsertPoint(doWaitBB);
        b.CreateCondBr(haveFds, epollBB, nanoChkBB);
        // epoll path: timeout = haveDl ? max(remaining_ms, 0) : -1 (forever).
        b.SetInsertPoint(epollBB);
        auto* epfd = b.CreateLoad(
            i32Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_EPFD, "epfdp2"),
            "epfd");
        auto* msSec = b.CreateMul(rsecB, llvm::ConstantInt::get(i64Ty, 1000),
                                  "ms_sec");
        auto* msNsec = b.CreateSDiv(
            rnsecB, llvm::ConstantInt::get(i64Ty, 1000000), "ms_nsec");
        auto* msTot = b.CreateAdd(msSec, msNsec, "ms_tot");
        // clamp negative remaining to 0 (non-blocking drain).
        auto* msClamped = b.CreateSelect(
            b.CreateICmpSLT(msTot, llvm::ConstantInt::get(i64Ty, 0)),
            llvm::ConstantInt::get(i64Ty, 0), msTot, "ms_clamped");
        auto* msI32 = b.CreateTrunc(msClamped, i32Ty, "ms_i32");
        auto* timeout = b.CreateSelect(
            haveDl, msI32, llvm::ConstantInt::get(i32Ty, -1), "timeout");
        auto* evbuf = b.CreateAlloca(
            epollEventTy_, llvm::ConstantInt::get(i32Ty, 8), "evbuf");
        b.CreateCall(epollWaitFn_,
                     {epfd, evbuf, llvm::ConstantInt::get(i32Ty, 8), timeout});
        b.CreateBr(retBB);
        // nano_chk: no fds. Only nanosleep if we actually have a (future)
        // deadline; otherwise return (defensive — driver shouldn't call us).
        b.SetInsertPoint(nanoChkBB);
        auto* doNano = b.CreateAnd(haveDl, b.CreateNot(past), "do_nano");
        auto* nanoBB = llvm::BasicBlock::Create(ctx, "nano", fn);
        b.CreateCondBr(doNano, nanoBB, retBB);
        b.SetInsertPoint(nanoBB);
        auto* req = b.CreateAlloca(timespecTy_, nullptr, "req");
        b.CreateStore(rsecB, b.CreateStructGEP(timespecTy_, req, 0, "rqs"));
        b.CreateStore(rnsecB, b.CreateStructGEP(timespecTy_, req, 1, "rqn"));
        b.CreateCall(nanosleepFn_,
                     {req, llvm::ConstantPointerNull::get(
                               llvm::cast<llvm::PointerType>(i8PtrTy))});
        b.CreateBr(retBB);
#else
        // Non-Linux (macOS): nanosleep the remaining duration when a future
        // deadline exists. kqueue fd-readiness is documented as deferred, so
        // there are never fds to wait on here.
        auto* doNano = b.CreateAnd(haveDl, b.CreateNot(past), "do_nano");
        auto* nanoBB = llvm::BasicBlock::Create(ctx, "nano", fn);
        b.CreateCondBr(doNano, nanoBB, retBB);
        b.SetInsertPoint(nanoBB);
        auto* req = b.CreateAlloca(timespecTy_, nullptr, "req");
        b.CreateStore(rsecB, b.CreateStructGEP(timespecTy_, req, 0, "rqs"));
        b.CreateStore(rnsecB, b.CreateStructGEP(timespecTy_, req, 1, "rqn"));
        b.CreateCall(nanosleepFn_,
                     {req, llvm::ConstantPointerNull::get(
                               llvm::cast<llvm::PointerType>(i8PtrTy))});
        b.CreateBr(retBB);
        (void)timerPast;
#endif
        b.SetInsertPoint(retBB);
        b.CreateRetVoid();
        execWaitFn_ = fn;
    }

    // ---- void __kd_exec_drive_until(i64 idx): the scheduler heart. Loop:
    // step() the whole queue (round-robin one poll per live task); if the
    // target task is done, stop; else if the round made no task ready and a
    // timer deadline is registered, sleep in the reactor until it; else keep
    // looping (busy — only when a non-timer leaf like yield_now is the only
    // thing pending, preserving Phase 12 behavior). A task that returns Ready
    // is removed from rotation (its `done` flag short-circuits future polls).
    void declareExecDrive(llvm::Type* /*execPtrTy*/) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* voidTy = llvm::Type::getVoidTy(ctx);
        auto* fnTy = llvm::FunctionType::get(voidTy, {i64Ty}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage, "__kd_exec_drive_until",
            module_.get());
        fn->getArg(0)->setName("idx");
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* loopBB = llvm::BasicBlock::Create(ctx, "loop", fn);
        auto* chkBB = llvm::BasicBlock::Create(ctx, "check_target", fn);
        auto* waitChkBB = llvm::BasicBlock::Create(ctx, "wait_check", fn);
        auto* waitBB = llvm::BasicBlock::Create(ctx, "wait", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        llvm::IRBuilder<> b(entry);
        b.CreateCall(execEnsureFn_, {});
        b.CreateBr(loopBB);
        b.SetInsertPoint(loopBB);
        auto* pending = b.CreateCall(execStepFn_, {}, "pending");
        b.CreateBr(chkBB);
        // target done? read Task[idx].done.
        b.SetInsertPoint(chkBB);
        auto* slot = b.CreateCall(execSlotFn_, {fn->getArg(0)}, "tslot");
        auto* done = b.CreateLoad(
            i64Ty, b.CreateStructGEP(taskTy_, slot, TASK_DONE, "tdone_p"),
            "tdone");
        auto* targetDone = b.CreateICmpNE(
            done, llvm::ConstantInt::get(i64Ty, 0), "target_done");
        b.CreateCondBr(targetDone, doneBB, waitChkBB);
        // not done. If nothing is pending at all, also stop (defensive: target
        // somehow not pending and not done — avoid an infinite spin).
        b.SetInsertPoint(waitChkBB);
        auto* nonePending = b.CreateICmpEQ(
            pending, llvm::ConstantInt::get(i64Ty, 0), "none_pending");
        auto* loop2BB = llvm::BasicBlock::Create(ctx, "decide_wait", fn);
        b.CreateCondBr(nonePending, doneBB, loop2BB);
        b.SetInsertPoint(loop2BB);
        // Some task is pending. If a timer deadline is registered (or, on
        // Linux, an fd is registered with epoll), sleep in the reactor until
        // it fires (don't spin). Otherwise loop immediately (a yield_now-style
        // leaf that's Pending-then-Ready needs an immediate re-poll — this
        // preserves the Phase 12 busy-progress behavior for non-timer leaves).
        auto* hasDl = b.CreateLoad(
            i64Ty,
            b.CreateStructGEP(execTy_, execGlobal_, EXEC_HAS_DL, "has_dl_p"),
            "has_dl");
        llvm::Value* shouldWait = b.CreateICmpNE(
            hasDl, llvm::ConstantInt::get(i64Ty, 0), "has_dl_nz");
#if defined(__linux__)
        auto* nfds = b.CreateLoad(
            llvm::Type::getInt32Ty(ctx),
            b.CreateStructGEP(execTy_, execGlobal_, EXEC_NFDS, "nfds_p"),
            "nfds");
        auto* haveFds = b.CreateICmpSGT(
            nfds, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0),
            "have_fds");
        shouldWait = b.CreateOr(shouldWait, haveFds, "should_wait");
#endif
        b.CreateCondBr(shouldWait, waitBB, loopBB);
        b.SetInsertPoint(waitBB);
        b.CreateCall(execWaitFn_, {});
        b.CreateBr(loopBB);
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();
        execDriveFn_ = fn;
    }

    // Phase 18: emit a `block_on`/spawn-driven executor body for result type T.
    // Shared by block_on__i64 (eager) and getOrEmitBlockOn (lazy per-T). Body:
    //   slot = malloc(sizeof Poll<T>)
    //   h = __kd_exec_push(f.poll, f.frame, slot)
    //   __kd_exec_drive_until(h)
    //   return ((Poll<T>*)slot)->value
    llvm::Function* emitBlockOnBody(const std::string& mangled,
                                    const TypePtr& T) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* futureTy = structTypes_["Future"];
        auto* pollT = pollTypeFor(T);
        auto* valTy = mapKardashevType(T);
        auto* fnTy = llvm::FunctionType::get(valTy, {futureTy}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, mangled, module_.get());
        fn->getArg(0)->setName("f");
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> b(entry);
        auto* fut = fn->getArg(0);
        auto* pollPtr = b.CreateExtractValue(fut, {0}, "f.poll");
        auto* framePtr = b.CreateExtractValue(fut, {1}, "f.frame");
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(pollT);
        auto* slot = b.CreateCall(
            mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "poll_slot");
        auto* typeTag = llvm::ConstantInt::get(i64Ty, std::hash<std::string>{}(mangleType(T)));
        auto* h = b.CreateCall(execPushFn_, {pollPtr, framePtr, slot, typeTag}, "handle");
        b.CreateCall(execDriveFn_, {h});
        // result = Poll<T>.value from the task's slot.
        auto* valP = b.CreateStructGEP(pollT, slot, 1, "val_ptr");
        auto* val = b.CreateLoad(valTy, valP, "result");
        (void)i8PtrTy;
        b.CreateRet(val);
        return fn;
    }

    // Phase 18: `sleep_ms(n: i64) -> Future<i64>` — a real wall-clock timer
    // leaf future. Frame: { i64 armed, i64 dl_sec, i64 dl_nsec, i64 ms }.
    // First poll computes deadline = now + ms (CLOCK_MONOTONIC) and arms;
    // every poll compares now vs deadline: past => Ready(ms); else register
    // the deadline with the executor and return Pending. Registering lets the
    // reactor sleep until the nearest deadline rather than busy-spin.
    void declareSleepMs() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* futureTy = structTypes_["Future"];
        // frame { armed, dl_sec, dl_nsec, ms }.
        auto* frameTy = llvm::StructType::create(
            ctx, {i64Ty, i64Ty, i64Ty, i64Ty}, "kd.sleep_frame");
        auto* setDl = declaredFns_["__kd_exec_set_deadline"];

        // void __kd_sleep_poll(i8* frame, kd.poll* out).
        llvm::Function* pollFn;
        {
            pollFn = llvm::Function::Create(
                pollFnTy_, llvm::Function::InternalLinkage, "__kd_sleep_poll",
                module_.get());
            pollFn->getArg(0)->setName("frame");
            pollFn->getArg(1)->setName("out");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", pollFn);
            auto* armBB = llvm::BasicBlock::Create(ctx, "arm", pollFn);
            auto* chkBB = llvm::BasicBlock::Create(ctx, "check", pollFn);
            auto* readyBB = llvm::BasicBlock::Create(ctx, "ready", pollFn);
            auto* pendBB = llvm::BasicBlock::Create(ctx, "pending", pollFn);
            llvm::IRBuilder<> b(entry);
            // Observe the poll (shared global counter, like yield_now).
            auto* pc = b.CreateLoad(i64Ty, kdPollCount_, "pc");
            b.CreateStore(
                b.CreateAdd(pc, llvm::ConstantInt::get(i64Ty, 1)),
                kdPollCount_);
            auto* frame = pollFn->getArg(0);
            auto* armedP = b.CreateStructGEP(frameTy, frame, 0, "armed_p");
            auto* dlSecP = b.CreateStructGEP(frameTy, frame, 1, "dlsec_p");
            auto* dlNsecP = b.CreateStructGEP(frameTy, frame, 2, "dlnsec_p");
            auto* msP = b.CreateStructGEP(frameTy, frame, 3, "ms_p");
            auto* armed = b.CreateLoad(i64Ty, armedP, "armed");
            auto* needArm = b.CreateICmpEQ(
                armed, llvm::ConstantInt::get(i64Ty, 0), "need_arm");
            b.CreateCondBr(needArm, armBB, chkBB);
            // arm: deadline = now + ms.
            b.SetInsertPoint(armBB);
            auto* nowTs = b.CreateAlloca(timespecTy_, nullptr, "now_ts");
            b.CreateCall(
                clockGettimeFn_,
                {llvm::ConstantInt::get(i32Ty, kMonotonicClockId), nowTs});
            auto* nowSec = b.CreateLoad(
                i64Ty, b.CreateStructGEP(timespecTy_, nowTs, 0, "ns_p"),
                "now_sec");
            auto* nowNsec = b.CreateLoad(
                i64Ty, b.CreateStructGEP(timespecTy_, nowTs, 1, "nn_p"),
                "now_nsec");
            auto* ms = b.CreateLoad(i64Ty, msP, "ms");
            // total_nsec = now_nsec + (ms % 1000)*1e6 ; sec = now_sec + ms/1000
            auto* addSec = b.CreateSDiv(ms, llvm::ConstantInt::get(i64Ty, 1000),
                                        "add_sec");
            auto* remMs = b.CreateSRem(ms, llvm::ConstantInt::get(i64Ty, 1000),
                                       "rem_ms");
            auto* addNsec = b.CreateMul(
                remMs, llvm::ConstantInt::get(i64Ty, 1000000), "add_nsec");
            auto* sumNsec = b.CreateAdd(nowNsec, addNsec, "sum_nsec");
            // carry if sumNsec >= 1e9.
            auto* carry = b.CreateICmpSGE(
                sumNsec, llvm::ConstantInt::get(i64Ty, 1000000000), "carry");
            auto* nsecFinal = b.CreateSelect(
                carry,
                b.CreateSub(sumNsec, llvm::ConstantInt::get(i64Ty, 1000000000)),
                sumNsec, "nsec_final");
            auto* secCarry = b.CreateSelect(
                carry, llvm::ConstantInt::get(i64Ty, 1),
                llvm::ConstantInt::get(i64Ty, 0), "sec_carry");
            auto* secFinal = b.CreateAdd(
                b.CreateAdd(nowSec, addSec), secCarry, "sec_final");
            b.CreateStore(secFinal, dlSecP);
            b.CreateStore(nsecFinal, dlNsecP);
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 1), armedP);
            b.CreateBr(chkBB);
            // check: now >= deadline ?
            b.SetInsertPoint(chkBB);
            auto* nowTs2 = b.CreateAlloca(timespecTy_, nullptr, "now_ts2");
            b.CreateCall(
                clockGettimeFn_,
                {llvm::ConstantInt::get(i32Ty, kMonotonicClockId), nowTs2});
            auto* nSec = b.CreateLoad(
                i64Ty, b.CreateStructGEP(timespecTy_, nowTs2, 0, "ns2_p"),
                "n_sec");
            auto* nNsec = b.CreateLoad(
                i64Ty, b.CreateStructGEP(timespecTy_, nowTs2, 1, "nn2_p"),
                "n_nsec");
            auto* dSec = b.CreateLoad(i64Ty, dlSecP, "d_sec");
            auto* dNsec = b.CreateLoad(i64Ty, dlNsecP, "d_nsec");
            // reached = now_sec > d_sec || (now_sec==d_sec && now_nsec>=d_nsec)
            auto* secGt = b.CreateICmpSGT(nSec, dSec, "sec_gt");
            auto* secEq = b.CreateICmpEQ(nSec, dSec, "sec_eq");
            auto* nsecGe = b.CreateICmpSGE(nNsec, dNsec, "nsec_ge");
            auto* reached =
                b.CreateOr(secGt, b.CreateAnd(secEq, nsecGe), "reached");
            b.CreateCondBr(reached, readyBB, pendBB);
            // ready: Ready(ms).
            b.SetInsertPoint(readyBB);
            auto* out = pollFn->getArg(1);
            b.CreateStore(llvm::ConstantInt::getTrue(ctx),
                          b.CreateStructGEP(pollTy_, out, 0, "rdy_p"));
            auto* ms2 = b.CreateLoad(i64Ty, msP, "ms2");
            b.CreateStore(ms2,
                          b.CreateStructGEP(pollTy_, out, 1, "outval_p"));
            b.CreateRetVoid();
            // pending: register deadline with the executor, return Pending.
            b.SetInsertPoint(pendBB);
            b.CreateCall(setDl, {dSec, dNsec});
            b.CreateStore(llvm::ConstantInt::getFalse(ctx),
                          b.CreateStructGEP(pollTy_, out, 0, "rdy_p2"));
            b.CreateRetVoid();
        }

        // Future sleep_ms(i64 ms): malloc the frame { armed=0, 0, 0, ms } and
        // return Future { __kd_sleep_poll, frame }.
        {
            auto* fnTy = llvm::FunctionType::get(futureTy, {i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "sleep_ms",
                module_.get());
            fn->getArg(0)->setName("ms");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(frameTy);
            auto* frame = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "sleep_frame");
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0),
                          b.CreateStructGEP(frameTy, frame, 0, "armed0"));
            b.CreateStore(fn->getArg(0),
                          b.CreateStructGEP(frameTy, frame, 3, "ms0"));
            llvm::Value* futv = llvm::UndefValue::get(futureTy);
            futv = b.CreateInsertValue(futv, pollFn, {0}, "fut.poll");
            futv = b.CreateInsertValue(futv, frame, {1}, "fut.frame");
            b.CreateRet(futv);
            declaredFns_["sleep_ms"] = fn;
            (void)i1Ty;
            (void)i8PtrTy;
        }
    }

    // Phase 18: `spawn<T>(f: Future<T>) -> i64` — register f as a concurrent
    // task and return its handle. Per-T only so the result slot is sized for T
    // (read back by join<T>). Mangled `spawn__<T>` so emitCall routes here.
    llvm::Function* getOrEmitSpawn(const TypePtr& T) {
        std::string mangled = "spawn__" + mangleType(T);
        auto it = declaredFns_.find(mangled);
        if (it != declaredFns_.end()) return it->second;
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* futureTy = structTypes_["Future"];
        auto* pollT = pollTypeFor(T);
        auto* fnTy = llvm::FunctionType::get(i64Ty, {futureTy}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, mangled, module_.get());
        fn->getArg(0)->setName("f");
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> b(entry);
        auto* fut = fn->getArg(0);
        auto* pollPtr = b.CreateExtractValue(fut, {0}, "f.poll");
        auto* framePtr = b.CreateExtractValue(fut, {1}, "f.frame");
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(pollT);
        auto* slot = b.CreateCall(
            mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "poll_slot");
        auto* typeTag = llvm::ConstantInt::get(i64Ty, std::hash<std::string>{}(mangleType(T)));
        auto* h = b.CreateCall(execPushFn_, {pollPtr, framePtr, slot, typeTag}, "handle");
        b.CreateRet(h);
        declaredFns_[mangled] = fn;
        return fn;
    }

    // Phase 18: `join<T>(handle: i64) -> T` — drive the executor until the
    // handle's task completes, then return its result read as T from the
    // task's poll_slot. Per-T so the value is read at its natural width.
    llvm::Function* getOrEmitJoin(const TypePtr& T) {
        std::string mangled = "join__" + mangleType(T);
        auto it = declaredFns_.find(mangled);
        if (it != declaredFns_.end()) return it->second;
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* pollT = pollTypeFor(T);
        auto* valTy = mapKardashevType(T);
        auto* fnTy = llvm::FunctionType::get(valTy, {i64Ty}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, mangled, module_.get());
        fn->getArg(0)->setName("handle");
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> b(entry);
        auto* h = fn->getArg(0);
        auto* countP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_COUNT, "count_ptr");
        auto* count = b.CreateLoad(i64Ty, countP, "count");
        auto* inRange = b.CreateICmpULT(h, count, "in_range");
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        auto* badBB = llvm::BasicBlock::Create(ctx, "bad", fn);
        b.CreateCondBr(inRange, okBB, badBB);
        b.SetInsertPoint(badBB);
        b.CreateRet(llvm::Constant::getNullValue(valTy));
        b.SetInsertPoint(okBB);
        b.CreateCall(execDriveFn_, {h});
        auto* slot = b.CreateCall(execSlotFn_, {h}, "tslot");
        auto* expected = llvm::ConstantInt::get(i64Ty, std::hash<std::string>{}(mangleType(T)));
        auto* gotType = b.CreateLoad(i64Ty, b.CreateStructGEP(taskTy_, slot, TASK_TYPE, "tp"), "got_type");
        auto* typeOk = b.CreateICmpEQ(expected, gotType, "type_ok");
        auto* typeBadBB = llvm::BasicBlock::Create(ctx, "type_bad", fn);
        auto* readBB = llvm::BasicBlock::Create(ctx, "read", fn);
        b.CreateCondBr(typeOk, readBB, typeBadBB);
        b.SetInsertPoint(typeBadBB);
        b.CreateRet(llvm::Constant::getNullValue(valTy));
        b.SetInsertPoint(readBB);
        auto* pslot = b.CreateLoad(
            i8PtrTy, b.CreateStructGEP(taskTy_, slot, TASK_SLOT, "sp"),
            "pslot");
        auto* valP = b.CreateStructGEP(pollT, pslot, 1, "val_ptr");
        auto* val = b.CreateLoad(valTy, valP, "result");
        b.CreateRet(val);
        declaredFns_[mangled] = fn;
        return fn;
    }

#if defined(__linux__)
    // Phase 18 stretch (Linux/epoll): the fd-readiness primitives. A pipe
    // handle packs `(write_fd << 32) | read_fd`; the read end is set
    // O_NONBLOCK so `read_pipe`'s poll can probe without blocking. `read_pipe`
    // is a leaf future whose frame is { i64 fd, i64 registered }: first poll
    // registers the fd with the executor's epoll set (EPOLLIN) and returns
    // Pending; the reactor then blocks in epoll_wait (see __kd_exec_wait)
    // until the fd is readable, after which a re-poll's non-blocking read
    // succeeds and the future reports Ready(byte). EOF reports Ready(0).
    void declareFdReactor() {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* futureTy = structTypes_["Future"];

        // i64 pipe_make(): pipe(fds); set fds[0] nonblocking; pack hi/lo.
        {
            auto* fnTy = llvm::FunctionType::get(i64Ty, {}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "pipe_make",
                module_.get());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateCall(execEnsureFn_, {}); // ensure executor exists
            // ensure the epoll fd exists (lazily create on first pipe).
            ensureEpoll(b);
            auto* fds = b.CreateAlloca(i32Ty, llvm::ConstantInt::get(i32Ty, 2),
                                       "fds");
            b.CreateCall(pipeFn_, {fds});
            auto* rfdP = b.CreateGEP(i32Ty, fds,
                                     {llvm::ConstantInt::get(i32Ty, 0)}, "rfdp");
            auto* rfd = b.CreateLoad(i32Ty, rfdP, "rfd");
            auto* wfdP = b.CreateGEP(i32Ty, fds,
                                     {llvm::ConstantInt::get(i32Ty, 1)}, "wfdp");
            auto* wfd = b.CreateLoad(i32Ty, wfdP, "wfd");
            // fcntl(rfd, F_SETFL, O_NONBLOCK). F_SETFL=4, O_NONBLOCK=04000.
            b.CreateCall(fcntlFn_,
                         {rfd, llvm::ConstantInt::get(i32Ty, 4 /*F_SETFL*/),
                          llvm::ConstantInt::get(i32Ty, 04000 /*O_NONBLOCK*/)});
            // handle = 0x4b44500000000000 | (wfd << 32) | (rfd & 0xffffffff).
            auto* rfd64 = b.CreateZExt(rfd, i64Ty, "rfd64");
            auto* wfd64 = b.CreateZExt(wfd, i64Ty, "wfd64");
            auto* hi = b.CreateShl(wfd64, llvm::ConstantInt::get(i64Ty, 32),
                                   "hi");
            auto* raw = b.CreateOr(hi, rfd64, "raw");
            auto* handle = b.CreateOr(raw, llvm::ConstantInt::get(i64Ty, 0x4b44500000000000ULL), "handle");
            b.CreateRet(handle);
            declaredFns_["pipe_make"] = fn;
        }

        // i64 pipe_send(i64 handle, i64 byte): write one byte to the write end.
        {
            auto* fnTy =
                llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "pipe_send",
                module_.get());
            fn->getArg(0)->setName("handle");
            fn->getArg(1)->setName("byte");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* magic = b.CreateAnd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 0xfffff00000000000ULL));
            auto* ok = b.CreateICmpEQ(magic, llvm::ConstantInt::get(i64Ty, 0x4b44500000000000ULL));
            auto* badBB = llvm::BasicBlock::Create(ctx, "bad", fn);
            auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
            b.CreateCondBr(ok, okBB, badBB);
            b.SetInsertPoint(badBB);
            b.CreateRet(llvm::ConstantInt::getSigned(i64Ty, -1));
            b.SetInsertPoint(okBB);
            auto* masked = b.CreateAnd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 0x000000ffffffffffULL));
            auto* wfd64 = b.CreateLShr(
                masked, llvm::ConstantInt::get(i64Ty, 32), "wfd64");
            auto* wfd = b.CreateTrunc(wfd64, i32Ty, "wfd");
            auto* buf = b.CreateAlloca(i8Ty, nullptr, "buf");
            b.CreateStore(b.CreateTrunc(fn->getArg(1), i8Ty, "b8"), buf);
            auto* n = b.CreateCall(
                writeFn_, {wfd, buf, llvm::ConstantInt::get(i64Ty, 1)}, "n");
            b.CreateRet(n);
            declaredFns_["pipe_send"] = fn;
        }

        // read_pipe's frame { i64 rfd, i64 registered }.
        auto* frameTy = llvm::StructType::create(ctx, {i64Ty, i64Ty},
                                                 "kd.readpipe_frame");

        // void __kd_readpipe_poll(i8* frame, kd.poll* out).
        {
            auto* fn = llvm::Function::Create(
                pollFnTy_, llvm::Function::InternalLinkage,
                "__kd_readpipe_poll", module_.get());
            fn->getArg(0)->setName("frame");
            fn->getArg(1)->setName("out");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* gotBB = llvm::BasicBlock::Create(ctx, "got", fn);
            auto* pendBB = llvm::BasicBlock::Create(ctx, "pending", fn);
            auto* regBB = llvm::BasicBlock::Create(ctx, "register", fn);
            auto* afterRegBB = llvm::BasicBlock::Create(ctx, "after_reg", fn);
            llvm::IRBuilder<> b(entry);
            auto* pc = b.CreateLoad(i64Ty, kdPollCount_, "pc");
            b.CreateStore(
                b.CreateAdd(pc, llvm::ConstantInt::get(i64Ty, 1)),
                kdPollCount_);
            auto* frame = fn->getArg(0);
            auto* fdP = b.CreateStructGEP(frameTy, frame, 0, "fd_p");
            auto* fd64 = b.CreateLoad(i64Ty, fdP, "fd64");
            auto* fd = b.CreateTrunc(fd64, i32Ty, "fd");
            // Try a non-blocking read of 1 byte.
            auto* buf = b.CreateAlloca(i8Ty, nullptr, "buf");
            auto* n = b.CreateCall(
                readFn_, {fd, buf, llvm::ConstantInt::get(i64Ty, 1)}, "n");
            // n >= 1 => Ready(byte); n == 0 => EOF Ready(0); n < 0 => would
            // block => Pending (register with epoll first time).
            auto* got = b.CreateICmpSGE(
                n, llvm::ConstantInt::get(i64Ty, 1), "got_byte");
            b.CreateCondBr(got, gotBB, pendBB);
            // got: Ready(byte) and deregister the fd from epoll.
            b.SetInsertPoint(gotBB);
            auto* byte = b.CreateZExt(b.CreateLoad(i8Ty, buf, "byte8"), i64Ty,
                                      "byte");
            deregisterFd(b, fd);
            auto* out = fn->getArg(1);
            b.CreateStore(llvm::ConstantInt::getTrue(ctx),
                          b.CreateStructGEP(pollTy_, out, 0, "rdy_p"));
            b.CreateStore(byte,
                          b.CreateStructGEP(pollTy_, out, 1, "outval_p"));
            b.CreateRetVoid();
            // pending: register the fd with epoll if not yet, then Pending.
            b.SetInsertPoint(pendBB);
            auto* regP = b.CreateStructGEP(frameTy, frame, 1, "reg_p");
            auto* reg = b.CreateLoad(i64Ty, regP, "reg");
            auto* notReg = b.CreateICmpEQ(
                reg, llvm::ConstantInt::get(i64Ty, 0), "not_reg");
            b.CreateCondBr(notReg, regBB, afterRegBB);
            b.SetInsertPoint(regBB);
            registerFd(b, fd);
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 1), regP);
            b.CreateBr(afterRegBB);
            b.SetInsertPoint(afterRegBB);
            b.CreateStore(llvm::ConstantInt::getFalse(ctx),
                          b.CreateStructGEP(pollTy_, fn->getArg(1), 0, "rdy_p2"));
            b.CreateRetVoid();
            readPipePollFn_ = fn;
        }

        // Future read_pipe(i64 handle): frame { rfd = handle & 0xffffffff,
        // registered = 0 }, future { __kd_readpipe_poll, frame }.
        {
            auto* fnTy = llvm::FunctionType::get(futureTy, {i64Ty}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage, "read_pipe",
                module_.get());
            fn->getArg(0)->setName("handle");
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            llvm::IRBuilder<> b(entry);
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(frameTy);
            auto* frame = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "rp_frame");
            auto* magic = b.CreateAnd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 0xfffff00000000000ULL));
            auto* ok = b.CreateICmpEQ(magic, llvm::ConstantInt::get(i64Ty, 0x4b44500000000000ULL));
            auto* goodBB = llvm::BasicBlock::Create(ctx, "good", fn);
            auto* badBB = llvm::BasicBlock::Create(ctx, "bad", fn);
            b.CreateCondBr(ok, goodBB, badBB);
            b.SetInsertPoint(badBB);
            b.CreateStore(llvm::ConstantInt::get(i64Ty, -1), b.CreateStructGEP(frameTy, frame, 0, "fd_bad"));
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), b.CreateStructGEP(frameTy, frame, 1, "reg_bad"));
            llvm::Value* badf = llvm::UndefValue::get(futureTy);
            badf = b.CreateInsertValue(badf, readPipePollFn_, {0}, "fut.poll.bad");
            badf = b.CreateInsertValue(badf, frame, {1}, "fut.frame.bad");
            b.CreateRet(badf);
            b.SetInsertPoint(goodBB);
            auto* masked = b.CreateAnd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 0x000000ffffffffffULL));
            auto* rfd = b.CreateAnd(
                masked, llvm::ConstantInt::get(i64Ty, 0xffffffff),
                "rfd");
            b.CreateStore(rfd, b.CreateStructGEP(frameTy, frame, 0, "fd0"));
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0),
                          b.CreateStructGEP(frameTy, frame, 1, "reg0"));
            llvm::Value* futv = llvm::UndefValue::get(futureTy);
            futv = b.CreateInsertValue(futv, readPipePollFn_, {0}, "fut.poll");
            futv = b.CreateInsertValue(futv, frame, {1}, "fut.frame");
            b.CreateRet(futv);
            declaredFns_["read_pipe"] = fn;
            (void)i1Ty;
            (void)i8PtrTy;
        }
    }

    // Lazily create the executor's epoll instance (epoll_create1(0)) if epfd is
    // still -1, storing the new fd back into the executor global. Emitted into
    // the IRBuilder's current block.
    void ensureEpoll(llvm::IRBuilder<>& b) {
        auto& ctx = *ctx_;
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* epfdP = b.CreateStructGEP(execTy_, execGlobal_, EXEC_EPFD,
                                        "epfd_p");
        auto* epfd = b.CreateLoad(i32Ty, epfdP, "epfd");
        auto* needCreate = b.CreateICmpSLT(
            epfd, llvm::ConstantInt::get(i32Ty, 0), "need_epoll");
        auto* fn = b.GetInsertBlock()->getParent();
        auto* createBB = llvm::BasicBlock::Create(ctx, "mk_epoll", fn);
        auto* contBB = llvm::BasicBlock::Create(ctx, "ep_cont", fn);
        b.CreateCondBr(needCreate, createBB, contBB);
        b.SetInsertPoint(createBB);
        auto* newFd = b.CreateCall(
            epollCreate1Fn_, {llvm::ConstantInt::get(i32Ty, 0)}, "new_epfd");
        b.CreateStore(newFd, epfdP);
        b.CreateBr(contBB);
        b.SetInsertPoint(contBB);
    }

    // epoll_ctl(epfd, EPOLL_CTL_ADD=1, fd, &ev{ EPOLLIN, fd }); nfds++.
    void registerFd(llvm::IRBuilder<>& b, llvm::Value* fd) {
        auto& ctx = *ctx_;
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* epfd = b.CreateLoad(
            i32Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_EPFD, "epp"),
            "epfd");
        auto* ev = b.CreateAlloca(epollEventTy_, nullptr, "ev");
        // EPOLLIN = 0x001.
        b.CreateStore(llvm::ConstantInt::get(i32Ty, 0x001),
                      b.CreateStructGEP(epollEventTy_, ev, 0, "ev_events"));
        b.CreateStore(b.CreateZExt(fd, i64Ty, "fd64"),
                      b.CreateStructGEP(epollEventTy_, ev, 1, "ev_data"));
        b.CreateCall(epollCtlFn_,
                     {epfd, llvm::ConstantInt::get(i32Ty, 1 /*ADD*/), fd, ev});
        auto* nfdsP =
            b.CreateStructGEP(execTy_, execGlobal_, EXEC_NFDS, "nfds_p");
        auto* nfds = b.CreateLoad(i32Ty, nfdsP, "nfds");
        b.CreateStore(b.CreateAdd(nfds, llvm::ConstantInt::get(i32Ty, 1)),
                      nfdsP);
    }

    // epoll_ctl(epfd, EPOLL_CTL_DEL=2, fd, null); nfds--.
    void deregisterFd(llvm::IRBuilder<>& b, llvm::Value* fd) {
        auto& ctx = *ctx_;
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* epfd = b.CreateLoad(
            i32Ty, b.CreateStructGEP(execTy_, execGlobal_, EXEC_EPFD, "epp"),
            "epfd");
        b.CreateCall(epollCtlFn_,
                     {epfd, llvm::ConstantInt::get(i32Ty, 2 /*DEL*/), fd,
                      llvm::ConstantPointerNull::get(
                          llvm::cast<llvm::PointerType>(i8PtrTy))});
        auto* nfdsP =
            b.CreateStructGEP(execTy_, execGlobal_, EXEC_NFDS, "nfds_p");
        auto* nfds = b.CreateLoad(i32Ty, nfdsP, "nfds");
        b.CreateStore(b.CreateSub(nfds, llvm::ConstantInt::get(i32Ty, 1)),
                      nfdsP);
    }
#endif // __linux__

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
                // Built-in single-layout generics: `Vec<T>`, `String`,
                // `Slice`, `HashMap<V>` and `Future<T>` always lower to one
                // hand-built struct regardless of their type arg(s). Vec keeps
                // a single `{i8*,i64,i64}` layout (element size computed
                // separately); HashMap is `{i8*,i64,i64}` (per-V bucket entry
                // sized separately); Future is `{i8* poll, i8* frame}` (the
                // result T only affects the per-T Poll/await/block_on, never
                // the Future value itself). So `Vec<i64>` / `Future<bool>` /
                // `HashMap<P>` must NOT get a distinct `%T__arg` instance —
                // that would mismatch the value the constructor produces.
                if (r->structName == "Vec" || r->structName == "String" ||
                    r->structName == "Slice" || r->structName == "HashMap" ||
                    r->structName == "Future") {
                    auto bit = structTypes_.find(r->structName);
                    if (bit != structTypes_.end()) return bit->second;
                }
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
        // Phase 13b: slice type `&[T]`. Handle before the ref-peel (the `&`
        // is part of the slice spelling, not an extra Ref wrapper).
        if (tr.isSlice) {
            TypePtr elem = tr.typeArgs.empty()
                               ? makeInt()
                               : astTypeRefToConcrete(tr.typeArgs[0]);
            return makeSlice(elem);
        }
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
        // Phase 16: a fn with no `-> T` annotation returns unit (parser
        // synthesizes a "unit" TypeRef). Mirrors typecheck's resolveTypeRef.
        if (tr.name == "unit") return makeUnit();
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
        auto mangleTypeArgs = [&](const std::vector<TypePtr>& args) {
            if (args.empty()) return std::string{};
            std::string out = "__";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0) out += "_";
                out += mangleType(args[i]);
            }
            return out;
        };
        switch (r->kind) {
        case TypeKind::Int:    return "i64";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Unit:   return "unit";
        case TypeKind::Struct: return r->structName + mangleTypeArgs(r->typeArgs);
        case TypeKind::Enum:   return r->enumName + mangleTypeArgs(r->typeArgs);
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
            // Phase 12: async fn — declare two LLVM functions. The
            // user-visible `name` is a CONSTRUCTOR returning `Future`: it
            // heap-allocates the state-machine frame, seeds state=0 + the
            // params, and returns `{ __async_poll_<name>, frame }`. The
            // sibling `__async_poll_<name>` is the resumable poll function
            // `void(i8* frame, kd.poll* out)` that runs the body across
            // suspension points (emitted in emitFunctionAs). Nothing runs
            // eagerly: calling `name` only builds the future.
            auto* futureTy = structTypes_["Future"];
            std::vector<llvm::Type*> argTs;
            argTs.reserve(fn.params.size());
            for (const auto& p : fn.params) argTs.push_back(mapTypeRef(p.type));
            auto* ctorFty = llvm::FunctionType::get(
                futureTy, argTs, /*isVarArg=*/false);
            auto* ctor = llvm::Function::Create(
                ctorFty, llvm::Function::ExternalLinkage, name,
                module_.get());
            unsigned i = 0;
            for (auto& arg : ctor->args()) {
                if (i < fn.params.size()) arg.setName(fn.params[i].name);
                ++i;
            }
            declaredFns_[name] = ctor;

            auto* pollFn = llvm::Function::Create(
                pollFnTy_, llvm::Function::InternalLinkage,
                "__async_poll_" + name, module_.get());
            pollFn->getArg(0)->setName("frame");
            pollFn->getArg(1)->setName("out");
            declaredFns_["__async_poll_" + name] = pollFn;
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
        // Phase 12: async fns lower to a state-machine poll function + a
        // future-constructing entry fn (no eager body execution).
        if (fn.isAsync) {
            emitAsyncFn(fn, baseName);
            return;
        }
        std::string mangled = mangleInstance(baseName, typeArgs);
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
        // Phase 16: reset Drop scope state and open the function-body scope
        // (params live here; the body block opens a nested scope inside).
        dropScopes_.clear();
        loopDropDepth_.clear();
        pushDropScope();
        // Phase 14a: attach a DISubprogram for this function (line-table
        // scope + DWARF entry). Done before body emission so DILocations can
        // reference it. No-op when debug is off.
        beginDebugSubprogram(fn);
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
            // Phase 14a: describe the parameter to the debugger (bonus).
            declareDebugParam(fn, i, name, alloca);
            // Phase 10a: record fn-typed params' Kardashev type so an
            // indirect call `f(args)` through the parameter can rebuild the
            // concrete LLVM FunctionType (same machinery as let-bound fn
            // values). Effects on the type are irrelevant to lowering.
            TypePtr paramTy = astTypeRefToConcrete(fn.params[i].type);
            if (resolve(paramTy)->kind == TypeKind::Function) {
                localTypes_[name] = paramTy;
            }
            // Phase 16: a by-value param owns its argument and is dropped at
            // function exit unless moved/returned. (`&self`/`&mut self` and
            // other reference params are borrows — not owning — so isDroppable
            // is false for them.)
            registerDroppableLocal(name, alloca, paramTy);
            ++i;
        }

        llvm::Value* bodyVal = emitBlock(*fn.body);

        // If we fell through to the end of the body without a terminator,
        // emit a `ret` using the body's tail value. Phase 16: drop the
        // function-body scope (the params) first — but if the body's tail is
        // the returned value (a bare droppable binding), it has moved out, so
        // its flag was already cleared by emitBlock's tail handling.
        if (!currentBlockTerminated()) {
            if (fn.body->tail) clearDropFlagIfMoved(*fn.body->tail);
            emitScopeDrops(dropScopes_.back());
        }
        dropScopes_.pop_back();
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
        endDebugSubprogram();
    }

    // Collect every `let`-bound local in an async fn body (source order, first
    // binding wins on shadowing) paired with its resolved Kardashev type, read
    // from the typechecker's recorded RHS types. Used to lay out the frame and
    // to map promoted-local LLVM field types.
    std::vector<std::pair<std::string, TypePtr>> asyncLocalTypes(
        const ast::FnDecl& fn) {
        std::vector<std::pair<std::string, TypePtr>> out;
        std::unordered_set<std::string> seen;
        collectAsyncLocalTypes(*fn.body, out, seen);
        return out;
    }
    void collectAsyncLocalTypes(
        const ast::Expr& e,
        std::vector<std::pair<std::string, TypePtr>>& out,
        std::unordered_set<std::string>& seen) {
        if (auto* be = dynamic_cast<const ast::BlockExpr*>(&e)) {
            for (const auto& s : be->stmts) {
                if (auto* let = dynamic_cast<const ast::LetStmt*>(s.get())) {
                    collectAsyncLocalTypes(*let->value, out, seen);
                    if (seen.insert(let->name).second) {
                        auto it = tc_.exprTypes.find(let->value.get());
                        TypePtr ty = it != tc_.exprTypes.end() ? it->second
                                                               : makeInt();
                        out.emplace_back(let->name, ty);
                    }
                } else if (auto* es =
                               dynamic_cast<const ast::ExprStmt*>(s.get())) {
                    collectAsyncLocalTypes(*es->expr, out, seen);
                } else if (auto* rs =
                               dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                    if (rs->value) collectAsyncLocalTypes(*rs->value, out, seen);
                } else if (auto* as =
                               dynamic_cast<const ast::AssignStmt*>(s.get())) {
                    collectAsyncLocalTypes(*as->value, out, seen);
                }
            }
            if (be->tail) collectAsyncLocalTypes(*be->tail, out, seen);
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            collectAsyncLocalTypes(*ie->cond, out, seen);
            collectAsyncLocalTypes(*ie->thenBranch, out, seen);
            if (ie->elseBranch) collectAsyncLocalTypes(*ie->elseBranch, out, seen);
            return;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            collectAsyncLocalTypes(*we->cond, out, seen);
            collectAsyncLocalTypes(*we->body, out, seen);
            return;
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            collectAsyncLocalTypes(*le->body, out, seen);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            collectAsyncLocalTypes(*ae->operand, out, seen);
            return;
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            collectAsyncLocalTypes(*bin->lhs, out, seen);
            collectAsyncLocalTypes(*bin->rhs, out, seen);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args) collectAsyncLocalTypes(*a, out, seen);
            return;
        }
        // Phase 17b: descend into the remaining composite expressions so a
        // `let` nested inside any of them (e.g. a struct-field block, a match
        // arm) is still discovered and given a frame slot. Mirrors the wider
        // set asyncLivenessWalk now traverses.
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& f : sl->fields)
                collectAsyncLocalTypes(*f.second, out, seen);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            collectAsyncLocalTypes(*fe->object, out, seen);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            collectAsyncLocalTypes(*re->operand, out, seen);
            return;
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            collectAsyncLocalTypes(*ue->operand, out, seen);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            collectAsyncLocalTypes(*mc->receiver, out, seen);
            for (const auto& a : mc->args) collectAsyncLocalTypes(*a, out, seen);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            collectAsyncLocalTypes(*me->scrutinee, out, seen);
            for (const auto& arm : me->arms)
                collectAsyncLocalTypes(*arm.body, out, seen);
            return;
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            collectAsyncLocalTypes(*se->operand, out, seen);
            collectAsyncLocalTypes(*se->start, out, seen);
            collectAsyncLocalTypes(*se->end, out, seen);
            return;
        }
        if (auto* range = dynamic_cast<const ast::RangeExpr*>(&e)) {
            collectAsyncLocalTypes(*range->start, out, seen);
            collectAsyncLocalTypes(*range->end, out, seen);
            return;
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            collectAsyncLocalTypes(*bn->value, out, seen);
            return;
        }
        // Remaining kinds (literals, idents, closures) bind no locals reachable
        // in the async surface; their sub-expressions can't introduce `let`s.
    }

    // Phase 12: compute the set of locals (params + `let` bindings) that are
    // LIVE ACROSS an await — i.e. bound before some await and read at/after
    // that await — and therefore must live in the heap frame to survive
    // suspension. Locals used only within a single await-free straight-line
    // segment stay as ordinary stack allocas.
    //
    // Analysis: a single walk in evaluation order maintains an `awaitCounter`
    // (incremented at each await) and `boundAt[v]` (the counter value when v
    // was bound). A read of v promotes v iff `awaitCounter > boundAt[v]`.
    // Params are bound at counter 0. Control flow is handled conservatively
    // (never under-approximating, which would miscompile): if/match branches
    // are walked in sequence; loop bodies are walked TWICE so a value defined
    // after an await in the body and read at the top on the next iteration is
    // caught. Over-approximation only widens the frame; it never drops a
    // genuinely-live local.
    std::unordered_set<std::string> computeAsyncFrameLocals(
        const ast::FnDecl& fn) {
        std::unordered_set<std::string> promoted;
        std::unordered_map<std::string, int> boundAt;
        int awaitCounter = 0;
        for (const auto& p : fn.params) boundAt[p.name] = 0;
        asyncLivenessWalk(*fn.body, boundAt, awaitCounter, promoted);
        return promoted;
    }
    void asyncLivenessWalk(const ast::Expr& e,
                           std::unordered_map<std::string, int>& boundAt,
                           int& awaitCounter,
                           std::unordered_set<std::string>& promoted) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            auto it = boundAt.find(id->name);
            if (it != boundAt.end() && awaitCounter > it->second)
                promoted.insert(id->name);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            asyncLivenessWalk(*ae->operand, boundAt, awaitCounter, promoted);
            ++awaitCounter; // the suspension happens after the operand evaluates
            return;
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            asyncLivenessWalk(*bin->lhs, boundAt, awaitCounter, promoted);
            asyncLivenessWalk(*bin->rhs, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args)
                asyncLivenessWalk(*a, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            asyncLivenessWalk(*fe->object, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            asyncLivenessWalk(*re->operand, boundAt, awaitCounter, promoted);
            return;
        }
        // Phase 17b: a struct literal `P { x: a, y: b }` reads each field
        // expression — so a local used only inside one (and bound before an
        // await it crosses) must still be promoted (e.g. an async fn returning
        // a struct built from awaited values). This traversal was missing,
        // which left such locals on a non-dominating alloca.
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& f : sl->fields)
                asyncLivenessWalk(*f.second, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            asyncLivenessWalk(*ue->operand, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            asyncLivenessWalk(*mc->receiver, boundAt, awaitCounter, promoted);
            for (const auto& a : mc->args)
                asyncLivenessWalk(*a, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            asyncLivenessWalk(*me->scrutinee, boundAt, awaitCounter, promoted);
            for (const auto& arm : me->arms)
                asyncLivenessWalk(*arm.body, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            asyncLivenessWalk(*se->operand, boundAt, awaitCounter, promoted);
            asyncLivenessWalk(*se->start, boundAt, awaitCounter, promoted);
            asyncLivenessWalk(*se->end, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* range = dynamic_cast<const ast::RangeExpr*>(&e)) {
            asyncLivenessWalk(*range->start, boundAt, awaitCounter, promoted);
            asyncLivenessWalk(*range->end, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            asyncLivenessWalk(*bn->value, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            asyncLivenessWalk(*ie->cond, boundAt, awaitCounter, promoted);
            asyncLivenessWalk(*ie->thenBranch, boundAt, awaitCounter, promoted);
            if (ie->elseBranch)
                asyncLivenessWalk(*ie->elseBranch, boundAt, awaitCounter,
                                  promoted);
            return;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            // Walk twice so loop-carried liveness across an in-body await is
            // captured on the conceptual back-edge.
            for (int rep = 0; rep < 2; ++rep) {
                asyncLivenessWalk(*we->cond, boundAt, awaitCounter, promoted);
                asyncLivenessWalk(*we->body, boundAt, awaitCounter, promoted);
            }
            return;
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            for (int rep = 0; rep < 2; ++rep)
                asyncLivenessWalk(*le->body, boundAt, awaitCounter, promoted);
            return;
        }
        if (auto* be = dynamic_cast<const ast::BlockExpr*>(&e)) {
            for (const auto& s : be->stmts) {
                if (auto* let = dynamic_cast<const ast::LetStmt*>(s.get())) {
                    asyncLivenessWalk(*let->value, boundAt, awaitCounter,
                                      promoted);
                    boundAt[let->name] = awaitCounter; // bound after RHS
                } else if (auto* es =
                               dynamic_cast<const ast::ExprStmt*>(s.get())) {
                    asyncLivenessWalk(*es->expr, boundAt, awaitCounter, promoted);
                } else if (auto* rs =
                               dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                    if (rs->value)
                        asyncLivenessWalk(*rs->value, boundAt, awaitCounter,
                                          promoted);
                } else if (auto* as =
                               dynamic_cast<const ast::AssignStmt*>(s.get())) {
                    // RHS reads count; the target write keeps the existing
                    // boundAt (conservative: a reassigned var stays promoted
                    // if it ever crosses an await).
                    asyncLivenessWalk(*as->value, boundAt, awaitCounter,
                                      promoted);
                }
            }
            if (be->tail)
                asyncLivenessWalk(*be->tail, boundAt, awaitCounter, promoted);
            return;
        }
        // IntLit / StringLit / Break / Continue / closures etc.: no locals of
        // ours to read (closures capture by value at definition; their bodies
        // run later and don't extend our frame liveness).
    }

    // ---------------------------------------------------------------------
    // Phase 12: lower an `async fn` to a real cooperative-future state
    // machine. Two functions result:
    //   * the user-visible `baseName` CONSTRUCTOR — heap-allocates the frame,
    //     stores state=0 and the params, returns Future{ poll, frame }.
    //   * `__async_poll_<baseName>` — the resumable POLL function. It reloads
    //     params + frame-promoted locals, `switch`es on the saved state to
    //     resume at the last suspension point, and runs the body. Each
    //     `.await` polls its sub-future: Pending => spill live locals + save
    //     state + return Pending (genuine suspension); Ready(x) => bind x and
    //     fall through. The final value is written to the Poll out-param as
    //     Ready.
    //
    // Frame layout: { i64 state, <params...>, <promoted locals...>,
    // Future cur_subfut }. "Promoted locals" are exactly those LIVE ACROSS an
    // await (computed by computeAsyncFrameLocals); locals not live across any
    // await stay as ordinary stack allocas re-created on the state-0 path.
    void emitAsyncFn(const ast::FnDecl& fn, const std::string& baseName) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* futureTy = structTypes_["Future"];

        auto ctorIt = declaredFns_.find(baseName);
        auto pollIt = declaredFns_.find("__async_poll_" + baseName);
        if (ctorIt == declaredFns_.end() || pollIt == declaredFns_.end()) {
            errors_.push_back("codegen: async fn not declared: " + baseName);
            return;
        }
        llvm::Function* ctor = ctorIt->second;
        llvm::Function* pollFn = pollIt->second;

        // --- Decide which locals must survive suspension. ---
        std::unordered_set<std::string> promoted =
            computeAsyncFrameLocals(fn);

        // --- Build the frame struct type. ---
        // Field order: [0] state (i64); params; promoted locals; cur_subfut.
        std::vector<llvm::Type*> frameFields;
        std::unordered_map<std::string, unsigned> frameIndex;
        std::vector<std::string> promotedOrder; // deterministic spill order
        frameFields.push_back(i64Ty); // state @ 0
        unsigned stateIdx = 0;
        for (const auto& p : fn.params) {
            frameIndex[p.name] = static_cast<unsigned>(frameFields.size());
            frameFields.push_back(mapTypeRef(p.type));
            if (promoted.count(p.name)) promotedOrder.push_back(p.name);
        }
        for (const auto& [name, ty] : asyncLocalTypes(fn)) {
            if (!promoted.count(name)) continue;
            if (frameIndex.count(name)) continue; // a promoted param already
            frameIndex[name] = static_cast<unsigned>(frameFields.size());
            frameFields.push_back(mapKardashevType(ty));
            promotedOrder.push_back(name);
        }
        unsigned subfutIdx = static_cast<unsigned>(frameFields.size());
        frameFields.push_back(futureTy); // cur_subfut
        std::string frameName = "kd.async_frame." + baseName + "." +
                                std::to_string(nextAsyncId_++);
        auto* frameTy = llvm::StructType::create(ctx, frameFields, frameName);

        // --- Emit the constructor (user-visible name). ---
        {
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", ctor);
            llvm::IRBuilder<> b(entry);
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(frameTy);
            auto* frame = b.CreateCall(
                mallocFn_, {llvm::ConstantInt::get(i64Ty, sz)}, "async_frame");
            // state = 0
            auto* stateP = b.CreateStructGEP(frameTy, frame, stateIdx,
                                             "state_ptr");
            b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), stateP);
            // store params into their frame slots
            unsigned ai = 0;
            for (auto& arg : ctor->args()) {
                const std::string& pn = fn.params[ai].name;
                auto* slot = b.CreateStructGEP(frameTy, frame,
                                               frameIndex[pn], pn + "_slot");
                b.CreateStore(&arg, slot);
                ++ai;
            }
            llvm::Value* fut = llvm::UndefValue::get(futureTy);
            fut = b.CreateInsertValue(fut, pollFn, {0}, "fut.poll");
            fut = b.CreateInsertValue(fut, frame, {1}, "fut.frame");
            b.CreateRet(fut);
        }

        // --- Emit the poll function body. ---
        // Save the outer emission context (we may be nested via a generic
        // instance worklist; same save/restore discipline as emitClosure).
        auto* savedFn = currentFn_;
        auto savedLocals = std::move(locals_);
        auto savedLocalTypes = std::move(localTypes_);
        auto savedLoopFrames = std::move(loopFrames_);
        TypePtr savedRetTy = currentFnReturnType_;
        bool savedInAsync = inAsyncFn_;
        auto savedFrameTy = asyncFrameTy_;
        auto savedFramePtr = asyncFramePtr_;
        auto savedPollOut = asyncPollOut_;
        auto savedPollTy = asyncPollTy_;
        auto savedSwitch = asyncSwitch_;
        auto savedFrameIndex = std::move(asyncFrameIndex_);
        auto savedPromoted = std::move(asyncPromotedLocals_);
        auto savedStateIdx = asyncStateIdx_;
        auto savedSubfutIdx = asyncSubfutIdx_;
        auto savedNextState = asyncNextState_;

        locals_.clear();
        localTypes_.clear();
        loopFrames_.clear();
        currentFn_ = pollFn;
        currentFnReturnType_ = astTypeRefToConcrete(fn.returnType);
        inAsyncFn_ = true;
        asyncFrameTy_ = frameTy;
        asyncFramePtr_ = pollFn->getArg(0);
        asyncPollOut_ = pollFn->getArg(1);
        // Phase 17b: this async fn's result type drives the Poll<T> shape its
        // poll fn writes through. currentFnReturnType_ is the INNER type T
        // (the body's value type), not Future<T>.
        asyncPollTy_ = pollTypeFor(currentFnReturnType_);
        asyncFrameIndex_ = std::move(frameIndex);
        asyncPromotedLocals_ = std::move(promotedOrder);
        asyncStateIdx_ = stateIdx;
        asyncSubfutIdx_ = subfutIdx;
        asyncNextState_ = 1;

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", pollFn);
        builder_->SetInsertPoint(entry);
        // Reload params + promoted locals from the frame into fresh stack
        // slots. On state 0 the promoted slots are uninitialised, but the
        // state-0 path writes each before any await reads it; on a resume the
        // slots hold the values spilled at the last suspension.
        for (const auto& p : fn.params) {
            llvm::Type* lt = mapTypeRef(p.type);
            auto* slot = asyncFrameSlot(p.name);
            auto* val = builder_->CreateLoad(lt, slot, p.name + ".reload");
            auto* a = builder_->CreateAlloca(lt, nullptr, p.name);
            builder_->CreateStore(val, a);
            locals_[p.name] = a;
            TypePtr pty = astTypeRefToConcrete(p.type);
            if (resolve(pty)->kind == TypeKind::Function) localTypes_[p.name] = pty;
        }
        for (const auto& [name, ty] : asyncLocalTypes(fn)) {
            if (!asyncFrameIndex_.count(name)) continue;
            if (locals_.count(name)) continue; // promoted param already loaded
            llvm::Type* lt = mapKardashevType(ty);
            auto* slot = asyncFrameSlot(name);
            auto* val = builder_->CreateLoad(lt, slot, name + ".reload");
            auto* a = builder_->CreateAlloca(lt, nullptr, name);
            builder_->CreateStore(val, a);
            locals_[name] = a;
            if (resolve(ty)->kind == TypeKind::Function) localTypes_[name] = ty;
        }
        // switch (state) { 0 -> body0; N -> (added per await) }
        auto* stateSlot = asyncFrameSlot("");  // state @ stateIdx
        auto* stateVal = builder_->CreateLoad(i64Ty, stateSlot, "state");
        auto* body0 = llvm::BasicBlock::Create(ctx, "async.body", pollFn);
        asyncSwitch_ = builder_->CreateSwitch(stateVal, body0, 4);
        builder_->SetInsertPoint(body0);

        // Emit the body. `.await` inside drives the state machine; `return`s
        // and the tail value finish the future via finishAsyncReady.
        llvm::Value* bodyVal = emitBlock(*fn.body);
        if (!currentBlockTerminated()) {
            finishAsyncReady(bodyVal);
        }

        // Restore outer context.
        currentFn_ = savedFn;
        locals_ = std::move(savedLocals);
        localTypes_ = std::move(savedLocalTypes);
        loopFrames_ = std::move(savedLoopFrames);
        currentFnReturnType_ = savedRetTy;
        inAsyncFn_ = savedInAsync;
        asyncFrameTy_ = savedFrameTy;
        asyncFramePtr_ = savedFramePtr;
        asyncPollOut_ = savedPollOut;
        asyncPollTy_ = savedPollTy;
        asyncSwitch_ = savedSwitch;
        asyncFrameIndex_ = std::move(savedFrameIndex);
        asyncPromotedLocals_ = std::move(savedPromoted);
        asyncStateIdx_ = savedStateIdx;
        asyncSubfutIdx_ = savedSubfutIdx;
        asyncNextState_ = savedNextState;
        (void)i1Ty;
        (void)i8PtrTy;
    }

    // GEP a field of the current async frame. The empty name addresses the
    // state word (field `asyncStateIdx_`); otherwise the named param/local's
    // promoted slot.
    llvm::Value* asyncFrameSlot(const std::string& name) {
        unsigned idx = name.empty() ? asyncStateIdx_ : asyncFrameIndex_.at(name);
        return builder_->CreateStructGEP(asyncFrameTy_, asyncFramePtr_, idx,
                                         (name.empty() ? "state" : name) +
                                             ".frameslot");
    }

    // Spill every promoted local from its stack slot into the frame, so its
    // value survives a suspension. Called right before saving state + polling
    // a sub-future at an await point.
    void spillAsyncLocals() {
        for (const auto& name : asyncPromotedLocals_) {
            auto lit = locals_.find(name);
            if (lit == locals_.end()) continue;
            auto* val = builder_->CreateLoad(
                lit->second->getAllocatedType(), lit->second, name + ".spill");
            builder_->CreateStore(val, asyncFrameSlot(name));
        }
    }

    // Finish the current async fn: write Ready(value) into the Poll out-param
    // and return from the poll function. `value` may be null for a unit body
    // (we report Ready(0) into the i64-shaped Poll). Phase 17b: the out-param
    // is shaped as `asyncPollTy_` = Poll<T> for this fn's result type T, so a
    // bool / struct result is stored at its natural width.
    void finishAsyncReady(llvm::Value* value) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::StructType* pollT = asyncPollTy_ ? asyncPollTy_ : pollTy_;
        if (!value) {
            // Unit body: report Ready(0) into the i64 slot (pollTypeFor(unit)
            // aliases pollTy_, so pollT's value field is i64 here).
            value = llvm::ConstantInt::get(i64Ty, 0);
        }
        auto* rdyP =
            builder_->CreateStructGEP(pollT, asyncPollOut_, 0, "ready_ptr");
        builder_->CreateStore(llvm::ConstantInt::getTrue(ctx), rdyP);
        auto* valP =
            builder_->CreateStructGEP(pollT, asyncPollOut_, 1, "out_val_ptr");
        builder_->CreateStore(value, valP);
        builder_->CreateRetVoid();
    }

    // Phase 12: lower `<fut>.await` inside an async fn's poll function to a
    // genuine suspension point. We allocate a fresh resume-state id N, store
    // the awaited sub-future into the frame, and emit:
    //
    //     <eval operand into cur_subfut>     (first arrival only)
    //     store state = N; spill live locals  (first arrival only)
    //     br poll_N
    //   resume_N:  (switch target for state==N — reached on re-poll)
    //     br poll_N
    //   poll_N:
    //     subfut = load frame.cur_subfut
    //     call subfut.poll(subfut.frame, &local_poll)
    //     if !ready { copy local_poll into out; ret }   ; PENDING: suspend
    //     result = local_poll.value                       ; READY: continue
    //
    // Returning Pending hands control back to the caller (the executor or an
    // outer poll fn); the next poll re-enters via the entry switch at
    // resume_N, having reloaded the spilled locals, and re-polls the same
    // sub-future. This is the real cooperative suspend/resume.
    llvm::Value* emitAwait(const ast::AwaitExpr& ae) {
        if (!inAsyncFn_) {
            // Guarded by the typechecker (`.await` only in async fn); defensive.
            errors_.push_back("codegen: .await outside an async fn");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i1Ty = llvm::Type::getInt1Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* futureTy = structTypes_["Future"];

        // Phase 17b: the awaited sub-future's result type. Its operand types as
        // `Future<subT>`; we poll it through a `Poll<subT>` slot and read the
        // ready value as subT. Defensive fallback to i64 if the type is
        // missing (ill-typed input codegen shouldn't reach) or typeArgs-less.
        TypePtr subResultTy = makeInt();
        if (TypePtr opTy = lookupExprType(*ae.operand)) {
            TypePtr ro = resolveInInstance(opTy);
            if (ro->kind == TypeKind::Struct && ro->structName == "Future" &&
                !ro->typeArgs.empty()) {
                subResultTy = ro->typeArgs[0];
            }
        }
        auto* subPollTy = pollTypeFor(subResultTy);
        auto* subValTy = mapKardashevType(subResultTy);

        int state = asyncNextState_++;

        // Evaluate the sub-future operand and store it into the frame so it
        // survives suspension (only happens on the first arrival; on resume we
        // jump straight to poll_N). The frame GEP is recomputed at each use
        // site so it dominates both the body0 store and the (switch-reachable)
        // pollBB load.
        llvm::Value* subfut = emitExpr(*ae.operand);
        builder_->CreateStore(
            subfut,
            builder_->CreateStructGEP(asyncFrameTy_, asyncFramePtr_,
                                      asyncSubfutIdx_, "cur_subfut_ptr"));
        // Save resume state + spill locals live across this suspension.
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, state),
                              asyncFrameSlot(""));
        spillAsyncLocals();

        auto* resumeBB = llvm::BasicBlock::Create(
            ctx, "async.resume" + std::to_string(state), currentFn_);
        auto* pollBB = llvm::BasicBlock::Create(
            ctx, "async.poll" + std::to_string(state), currentFn_);
        auto* pendingBB = llvm::BasicBlock::Create(
            ctx, "async.pending" + std::to_string(state), currentFn_);
        auto* readyBB = llvm::BasicBlock::Create(
            ctx, "async.ready" + std::to_string(state), currentFn_);

        // Register the resume target on the entry switch.
        asyncSwitch_->addCase(llvm::ConstantInt::get(i64Ty, state), resumeBB);

        builder_->CreateBr(pollBB);
        builder_->SetInsertPoint(resumeBB);
        builder_->CreateBr(pollBB);

        builder_->SetInsertPoint(pollBB);
        auto* subfutSlot = builder_->CreateStructGEP(
            asyncFrameTy_, asyncFramePtr_, asyncSubfutIdx_, "cur_subfut_ptr");
        auto* fut = builder_->CreateLoad(futureTy, subfutSlot, "subfut");
        auto* subPoll = builder_->CreateExtractValue(fut, {0}, "subfut.poll");
        auto* subFrame = builder_->CreateExtractValue(fut, {1}, "subfut.frame");
        auto* pollSlot = builder_->CreateAlloca(subPollTy, nullptr, "subpoll");
        builder_->CreateCall(pollFnTy_, subPoll, {subFrame, pollSlot});
        auto* rdyP = builder_->CreateStructGEP(subPollTy, pollSlot, 0, "ready_ptr");
        auto* rdy = builder_->CreateLoad(i1Ty, rdyP, "ready");
        builder_->CreateCondBr(rdy, readyBB, pendingBB);

        // PENDING: propagate Pending to our caller and return (suspend). Only
        // the `ready` flag (field 0) is written; the value is undefined while
        // Pending. Use this fn's own Poll<T> shape for the out-param GEP.
        builder_->SetInsertPoint(pendingBB);
        llvm::StructType* outPollTy = asyncPollTy_ ? asyncPollTy_ : pollTy_;
        auto* outRdyP =
            builder_->CreateStructGEP(outPollTy, asyncPollOut_, 0,
                                      "out_ready_ptr");
        builder_->CreateStore(llvm::ConstantInt::getFalse(ctx), outRdyP);
        builder_->CreateRetVoid();

        // READY: extract the value (as the sub-future's result type) and
        // continue with it as the await result.
        builder_->SetInsertPoint(readyBB);
        auto* valP = builder_->CreateStructGEP(subPollTy, pollSlot, 1, "val_ptr");
        (void)i8PtrTy;
        (void)i64Ty;
        return builder_->CreateLoad(subValTy, valP, "await_result");
    }

    // ===================================================================
    // Phase 16: deterministic Drop (RAII)
    // ===================================================================

    // Allocate a stack slot in the CURRENT function's entry block. Drop-flag
    // and scratch allocas MUST live in the entry block (not inline in a loop
    // body) so they're created once per call rather than once per iteration —
    // otherwise a loop that drops a fresh value each turn would grow the stack
    // unboundedly. (LLVM's mem2reg promotes these away post-opt, but we keep
    // the IR correct pre-opt too, and the unit tests assert on flag behavior.)
    llvm::AllocaInst* entryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::BasicBlock& entry = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb(&entry, entry.getFirstInsertionPt());
        return eb.CreateAlloca(ty, nullptr, name);
    }

    // Is a value of type `t` something whose scope-exit needs drop glue?
    // True for the heap-owning built-ins (Vec/String/HashMap/Box), any type
    // with a user `impl Drop`, and any struct/enum that transitively owns one.
    // `seen` guards against cyclic type definitions.
    bool isDroppable(const TypePtr& t) {
        std::unordered_set<std::string> seen;
        return isDroppableImpl(t, seen);
    }
    bool isDroppableImpl(const TypePtr& t,
                         std::unordered_set<std::string>& seen) {
        if (!t) return false;
        TypePtr r = resolveInInstance(t);
        switch (r->kind) {
        case TypeKind::Box:
            return true; // always owns a heap allocation
        case TypeKind::Struct: {
            // Heap-owning built-ins.
            if (r->structName == "Vec" || r->structName == "String" ||
                r->structName == "HashMap")
                return true;
            // Slice / Range / fn-value / future structs own no heap (Slice is
            // a borrow; Range/Future are plain scalars in the MVP).
            if (r->structName == "Slice" || r->structName == "Range" ||
                r->structName == "Future")
                return false;
            std::string key = mangleStructInstance(r->structName, r->typeArgs);
            if (!seen.insert("S:" + key).second) return false;
            if (dropImpls_.count(r->structName)) return true;
            for (const auto& [_n, fty] : r->structFields) {
                if (isDroppableImpl(fty, seen)) return true;
            }
            return false;
        }
        case TypeKind::Enum: {
            std::string key = mangleStructInstance(r->enumName, r->typeArgs);
            if (!seen.insert("E:" + key).second) return false;
            if (dropImpls_.count(r->enumName)) return true;
            for (const auto& v : r->enumVariants) {
                for (const auto& pt : v.payloadTypes) {
                    if (isDroppableImpl(pt, seen)) return true;
                }
            }
            return false;
        }
        // Scalars, references, fn values, dyn objects: never owning.
        default:
            return false;
        }
    }

    // Emit the full drop of a value whose storage lives at `valuePtr` (a
    // pointer to a value of type `t`): run the user `Drop::drop` (if any), then
    // recurse into fields / payloads / heap buffers, freeing as we go. This is
    // the order Rust uses: user destructor first, then field drops, then the
    // value's own backing storage. UNCONDITIONAL — callers gate it on a drop
    // flag (or static knowledge that the value is still owned).
    void emitDropGlue(llvm::Value* valuePtr, const TypePtr& t) {
        TypePtr r = resolveInInstance(t);
        auto& ctx = *ctx_;
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);

        if (r->kind == TypeKind::Box) {
            // Box<T>: storage holds a heap pointer to T. Drop the boxed value
            // (through the loaded pointer), then free the box itself. A
            // Box<dyn Trait> is a fat pointer we don't statically know how to
            // drop the pointee of — free only the data slot.
            llvm::Value* boxPtr =
                builder_->CreateLoad(i8PtrTy, valuePtr, "box.ptr");
            TypePtr inner = resolveInInstance(r->refInner);
            if (inner->kind != TypeKind::Dyn && isDroppable(inner)) {
                // boxPtr points directly at the boxed T.
                emitDropGlue(boxPtr, inner);
            }
            builder_->CreateCall(freeFn_, {boxPtr});
            return;
        }

        if (r->kind == TypeKind::Struct) {
            // User destructor first (if this concrete type has `impl Drop`).
            emitUserDrop(valuePtr, r);
            llvm::Type* llvmTy = mapKardashevType(r);
            if (r->structName == "Vec" || r->structName == "String") {
                // { i8* data, i64 len, i64 cap }. `cap == 0` is the "not
                // heap-owned" marker: an empty Vec (data == null) or a
                // borrowed String literal (data points at a read-only global).
                // Freeing in that case would be a no-op (null) or, worse, undo
                // a non-malloc pointer, so we guard the free on `cap != 0`.
                // When the buffer IS heap-owned and the element type is itself
                // droppable, drop each live element before freeing.
                if (r->structName == "Vec" && !r->typeArgs.empty() &&
                    isDroppable(r->typeArgs[0])) {
                    emitDropVecElements(valuePtr, r->typeArgs[0]);
                }
                emitFreeBufferIfOwned(valuePtr, llvmTy);
                return;
            }
            if (r->structName == "HashMap") {
                // { i8* buckets, i64 len, i64 cap }. MVP keys/values are i64
                // (non-droppable), so just free the bucket array (guarded on
                // cap != 0, i.e. an actually-allocated table).
                emitFreeBufferIfOwned(valuePtr, llvmTy);
                return;
            }
            // Plain user struct: recurse into each droppable field in
            // declaration order (after the user destructor above).
            for (unsigned i = 0; i < r->structFields.size(); ++i) {
                const TypePtr& fty = r->structFields[i].second;
                if (!isDroppable(fty)) continue;
                auto* fldP = builder_->CreateStructGEP(
                    llvmTy, valuePtr, i, "drop.fld");
                emitDropGlue(fldP, fty);
            }
            return;
        }

        if (r->kind == TypeKind::Enum) {
            emitUserDrop(valuePtr, r);
            emitDropEnum(valuePtr, r);
            return;
        }
        // Scalars / refs / fn values: nothing to free.
    }

    // Invoke a user `impl Drop for T`'s `drop(&mut self)` on the value at
    // `valuePtr`, if this concrete type has one. The method's LLVM signature
    // takes the self pointer directly (it's a `&mut self` method).
    void emitUserDrop(llvm::Value* valuePtr, const TypePtr& r) {
        const std::string& name =
            r->kind == TypeKind::Struct ? r->structName : r->enumName;
        auto it = dropImpls_.find(name);
        if (it == dropImpls_.end()) return;
        auto fit = declaredFns_.find(it->second);
        if (fit == declaredFns_.end()) return; // not emitted (shouldn't happen)
        builder_->CreateCall(fit->second, {valuePtr});
    }

    // Free the heap buffer (field 0) of a `{ data, len, cap }` value (Vec /
    // String / HashMap) only when `cap != 0` — the marker that the buffer is
    // genuinely heap-owned (cap == 0 is an empty/borrowed value whose `data`
    // is null or a read-only global, which must NOT be passed to free).
    void emitFreeBufferIfOwned(llvm::Value* valuePtr, llvm::Type* llvmTy) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* capP = builder_->CreateStructGEP(llvmTy, valuePtr, 2, "buf.cap.p");
        auto* cap = builder_->CreateLoad(i64Ty, capP, "buf.cap");
        auto* owned = builder_->CreateICmpNE(
            cap, llvm::ConstantInt::get(i64Ty, 0), "buf.owned");
        auto* freeBB = llvm::BasicBlock::Create(ctx, "buf.free", currentFn_);
        auto* afterBB =
            llvm::BasicBlock::Create(ctx, "buf.free.after", currentFn_);
        builder_->CreateCondBr(owned, freeBB, afterBB);
        builder_->SetInsertPoint(freeBB);
        auto* dataP = builder_->CreateStructGEP(llvmTy, valuePtr, 0, "buf.ptr.p");
        auto* data = builder_->CreateLoad(i8PtrTy, dataP, "buf.ptr");
        builder_->CreateCall(freeFn_, {data});
        builder_->CreateBr(afterBB);
        builder_->SetInsertPoint(afterBB);
    }

    // Drop each live element of a Vec<T> whose element type T is droppable.
    // Loads len, loops i in [0,len), GEPs element i, drops it. `data` may be
    // null only when len==0, so the loop body never dereferences null.
    void emitDropVecElements(llvm::Value* vecPtr, const TypePtr& elemTy) {
        auto& ctx = *ctx_;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* vecTy = structTypes_["Vec"];
        llvm::Type* elemLlvm = mapKardashevType(elemTy);
        auto* lenP = builder_->CreateStructGEP(vecTy, vecPtr, 1, "vec.len.p");
        auto* len = builder_->CreateLoad(i64Ty, lenP, "vec.len");
        auto* dataP = builder_->CreateStructGEP(vecTy, vecPtr, 0, "vec.data.p");
        auto* data = builder_->CreateLoad(i8PtrTy, dataP, "vec.data");
        auto* iSlot = entryAlloca(i64Ty, "drop.i");
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), iSlot);
        auto* hdr = llvm::BasicBlock::Create(ctx, "drop.elem.hdr", currentFn_);
        auto* body = llvm::BasicBlock::Create(ctx, "drop.elem.body", currentFn_);
        auto* done = llvm::BasicBlock::Create(ctx, "drop.elem.done", currentFn_);
        builder_->CreateBr(hdr);
        builder_->SetInsertPoint(hdr);
        auto* i = builder_->CreateLoad(i64Ty, iSlot, "i");
        auto* cmp = builder_->CreateICmpSLT(i, len, "more");
        builder_->CreateCondBr(cmp, body, done);
        builder_->SetInsertPoint(body);
        auto* elemPtr = builder_->CreateGEP(elemLlvm, data, i, "elem.ptr");
        emitDropGlue(elemPtr, elemTy);
        auto* iNext = builder_->CreateAdd(
            i, llvm::ConstantInt::get(i64Ty, 1), "i.next");
        builder_->CreateStore(iNext, iSlot);
        builder_->CreateBr(hdr);
        builder_->SetInsertPoint(done);
    }

    // Drop an enum value: switch on the tag and drop the active variant's
    // droppable payload slots. Variants with no droppable payload share the
    // no-op default. The enum's own storage is inline (no heap), so there's
    // nothing to free beyond the payloads.
    void emitDropEnum(llvm::Value* enumPtr, const TypePtr& r) {
        // Collect variants that actually own something droppable.
        std::vector<unsigned> dropVariants;
        for (unsigned vi = 0; vi < r->enumVariants.size(); ++vi) {
            for (const auto& pt : r->enumVariants[vi].payloadTypes) {
                if (isDroppable(pt)) { dropVariants.push_back(vi); break; }
            }
        }
        if (dropVariants.empty()) return; // nothing to do
        auto& ctx = *ctx_;
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* llvmTy = mapKardashevType(r);
        std::string mangled = mangleStructInstance(r->enumName, r->typeArgs);
        const auto& slots = enumPayloadIndices_[mangled];
        auto* tagP = builder_->CreateStructGEP(llvmTy, enumPtr, 0, "drop.tag.p");
        auto* tag = builder_->CreateLoad(i32Ty, tagP, "drop.tag");
        auto* contBB = llvm::BasicBlock::Create(ctx, "drop.enum.cont",
                                                currentFn_);
        auto* sw = builder_->CreateSwitch(tag, contBB, dropVariants.size());
        for (unsigned vi : dropVariants) {
            auto* caseBB = llvm::BasicBlock::Create(
                ctx, "drop.enum.v" + std::to_string(vi), currentFn_);
            sw->addCase(llvm::ConstantInt::get(i32Ty, vi), caseBB);
            builder_->SetInsertPoint(caseBB);
            const auto& vty = r->enumVariants[vi].payloadTypes;
            for (unsigned pi = 0; pi < vty.size(); ++pi) {
                if (!isDroppable(vty[pi])) continue;
                auto* pp = builder_->CreateStructGEP(
                    llvmTy, enumPtr, slots[vi][pi], "drop.pld");
                emitDropGlue(pp, vty[pi]);
            }
            builder_->CreateBr(contBB);
        }
        builder_->SetInsertPoint(contBB);
    }

    // --- scope bookkeeping ---

    void pushDropScope() { dropScopes_.push_back({}); }

    // Register `name` (just `let`-bound or a by-value param) as an owning local
    // in the innermost scope, with a drop flag initialized to `live`. No-op for
    // non-droppable types and inside async fns (whose frame-promoted locals
    // outlive the poll fn and would risk a double-free — async values leak, a
    // documented MVP limitation).
    void registerDroppableLocal(const std::string& name,
                                llvm::AllocaInst* storage, const TypePtr& ty) {
        if (inAsyncFn_) return;
        if (dropScopes_.empty()) return;
        if (!ty || !isDroppable(ty)) return;
        auto* flag = entryAlloca(llvm::Type::getInt1Ty(*ctx_),
                                 name + ".droplive");
        // Set live at the point of binding (re-runs each loop iteration for a
        // local declared inside a loop body, which is exactly what we want:
        // the previous iteration's value was already dropped at block exit).
        builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), flag);
        dropScopes_.back().push_back({name, storage, flag,
                                      resolveInInstance(ty)});
    }

    // Emit drops for one scope's locals in reverse declaration order, each
    // guarded by its drop flag. After dropping, the flag is cleared so a value
    // can never be dropped twice (defensive — each storage is dropped on one
    // path). Does NOT pop the scope vector; callers manage the stack.
    void emitScopeDrops(const std::vector<DropLocal>& scope) {
        if (currentBlockTerminated()) return;
        auto& ctx = *ctx_;
        for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
            const DropLocal& d = *it;
            auto* live = builder_->CreateLoad(
                llvm::Type::getInt1Ty(ctx), d.flag, d.name + ".live");
            auto* dropBB = llvm::BasicBlock::Create(
                ctx, "drop." + d.name, currentFn_);
            auto* afterBB = llvm::BasicBlock::Create(
                ctx, "drop." + d.name + ".after", currentFn_);
            builder_->CreateCondBr(live, dropBB, afterBB);
            builder_->SetInsertPoint(dropBB);
            builder_->CreateStore(llvm::ConstantInt::getFalse(ctx), d.flag);
            emitDropGlue(d.storage, d.type);
            if (!currentBlockTerminated()) builder_->CreateBr(afterBB);
            builder_->SetInsertPoint(afterBB);
        }
    }

    // Pop the innermost scope and drop its locals (normal block fall-through).
    void popDropScopeWithDrops() {
        if (dropScopes_.empty()) return;
        emitScopeDrops(dropScopes_.back());
        dropScopes_.pop_back();
    }

    // A `return` exits every enclosing scope of the function: drop them all,
    // innermost first, in reverse declaration order within each. Does NOT pop
    // the scope vectors — the normal structural pop still happens as emission
    // unwinds, but those pops see a terminated block and emit nothing.
    void emitReturnDrops() {
        for (auto sit = dropScopes_.rbegin(); sit != dropScopes_.rend(); ++sit) {
            emitScopeDrops(*sit);
            if (currentBlockTerminated()) break;
        }
    }

    // `break`/`continue` exits the scopes opened INSIDE the current loop body
    // (everything at depth >= the loop's recorded entry depth), innermost
    // first. Outer scopes survive (the loop's enclosing scope keeps living).
    void emitLoopExitDrops() {
        if (loopDropDepth_.empty()) return;
        std::size_t floor = loopDropDepth_.back();
        for (std::size_t i = dropScopes_.size(); i-- > floor;) {
            emitScopeDrops(dropScopes_[i]);
            if (currentBlockTerminated()) break;
        }
    }

    // Mark a droppable local as moved-out (clear its drop flag) when an
    // IdentExpr naming it appears in a consuming position. Mirrors the borrow
    // checker's "whole use": the new owner (callee, field, returned value, ...)
    // becomes responsible, so the current scope must not also drop it. Searches
    // all live scopes (a value declared in an outer block can be moved from an
    // inner one). Walks innermost-out so shadowing resolves to the nearest.
    void clearDropFlagIfMoved(const ast::Expr& e) {
        if (inAsyncFn_) return;
        const auto* id = dynamic_cast<const ast::IdentExpr*>(&e);
        if (!id) return;
        for (auto sit = dropScopes_.rbegin(); sit != dropScopes_.rend(); ++sit) {
            for (auto it = sit->rbegin(); it != sit->rend(); ++it) {
                if (it->name == id->name) {
                    builder_->CreateStore(
                        llvm::ConstantInt::getFalse(*ctx_), it->flag);
                    return;
                }
            }
        }
    }

    // Emit an expression that is being CONSUMED by value into the surrounding
    // context (call/ctor arg, struct-lit field, `let` RHS, returned value,
    // match scrutinee, by-value method receiver, `Box::new` operand, break
    // value, `?` operand). If it's a bare droppable binding, that binding has
    // been moved away, so clear its drop flag.
    llvm::Value* emitConsume(const ast::Expr& e) {
        llvm::Value* v = emitExpr(e);
        clearDropFlagIfMoved(e);
        return v;
    }

    llvm::Value* emitBlock(const ast::BlockExpr& block) {
        // Phase 16: a block opens a lexical scope; its `let`-bound owning
        // locals are dropped (reverse order) when control leaves the block.
        pushDropScope();
        llvm::Value* v = emitBlockInner(block);
        // The tail value, when it is a bare droppable binding, is moved OUT of
        // the block to the enclosing context (it's the block's value) — so it
        // must survive this scope's drops.
        if (block.tail) clearDropFlagIfMoved(*block.tail);
        popDropScopeWithDrops();
        return v;
    }

    llvm::Value* emitBlockInner(const ast::BlockExpr& block) {
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
            // Phase 16: the RHS is a consuming context — if it's a bare
            // droppable binding, ownership moves into the new binding, so the
            // source's drop flag is cleared (after the value bits are read).
            llvm::Value* v = emitExpr(*let->value);
            clearDropFlagIfMoved(*let->value);
            // Phase 12: if this local is frame-promoted (live across an await),
            // its single stack slot was already alloca'd in the poll fn's entry
            // block (so the resume path's reload and the let's store target the
            // SAME slot). Reuse it; otherwise create a fresh alloca as usual.
            llvm::AllocaInst* alloca = nullptr;
            if (inAsyncFn_ && asyncFrameIndex_.count(let->name)) {
                auto existing = locals_.find(let->name);
                if (existing != locals_.end()) alloca = existing->second;
            }
            if (!alloca) {
                alloca = builder_->CreateAlloca(v->getType(), nullptr,
                                                let->name);
            }
            builder_->CreateStore(v, alloca);
            locals_[let->name] = alloca;
            // Phase 14a: describe the local to the debugger (bonus). Skip in
            // async fns, where locals live in a promoted frame slot rather
            // than a plain entry-block alloca.
            if (!inAsyncFn_) {
                declareDebugLocal(let->name, let->line, let->column, alloca);
            }
            // Phase 4.3: remember the kardashev type so emitCall can
            // do indirect dispatch through fn-pointer locals.
            auto tyIt = tc_.exprTypes.find(let->value.get());
            if (tyIt != tc_.exprTypes.end()) {
                localTypes_[let->name] = tyIt->second;
            }
            // Phase 16: track this binding for scope-exit drop if it owns a
            // droppable value.
            registerDroppableLocal(let->name, alloca, lookupExprType(*let->value));
            return;
        }
        if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            llvm::Value* v = ret->value ? emitExpr(*ret->value) : nullptr;
            // Phase 16: a returned bare binding is moved OUT to the caller, so
            // clear its drop flag before running this fn's scope-exit drops —
            // the caller now owns it. Then drop every still-live local in all
            // enclosing scopes (innermost first) before the actual `ret`.
            if (ret->value) clearDropFlagIfMoved(*ret->value);
            if (inAsyncFn_) {
                // Phase 12: `return x` from an async fn finishes the future
                // with Ready(x) (the poll fn itself returns void).
                finishAsyncReady(v);
            } else {
                emitReturnDrops();
                if (v) {
                    builder_->CreateRet(v);
                } else {
                    builder_->CreateRetVoid();
                }
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
        // Phase 16: the RHS moves into the place (clear its source flag); and
        // when the target is a still-live droppable binding, its OLD value is
        // overwritten, so drop it first (guarded by its flag), then store the
        // new value and keep the binding live. This frees, e.g., the previous
        // buffer when a `let mut v: Vec` is reassigned — without it the old
        // allocation would leak.
        clearDropFlagIfMoved(*as.value);
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(as.target.get())) {
            DropLocal* d = findDropLocal(id->name);
            if (d) {
                emitDropLocalGuarded(*d);
                builder_->CreateStore(v, slot);
                builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_),
                                      d->flag);
                return;
            }
        }
        builder_->CreateStore(v, slot);
    }

    // Find the tracked owning local named `name` in any live scope (innermost
    // first), or null. Used by assignment-overwrite drops.
    DropLocal* findDropLocal(const std::string& name) {
        for (auto sit = dropScopes_.rbegin(); sit != dropScopes_.rend(); ++sit) {
            for (auto it = sit->rbegin(); it != sit->rend(); ++it) {
                if (it->name == name) return &*it;
            }
        }
        return nullptr;
    }

    // Drop a single tracked local if its flag is live (then leave the flag
    // cleared on the dropped path). Reuses the conditional pattern of
    // emitScopeDrops for one local.
    void emitDropLocalGuarded(DropLocal& d) {
        if (currentBlockTerminated()) return;
        auto& ctx = *ctx_;
        auto* live = builder_->CreateLoad(
            llvm::Type::getInt1Ty(ctx), d.flag, d.name + ".live");
        auto* dropBB =
            llvm::BasicBlock::Create(ctx, "drop." + d.name + ".re", currentFn_);
        auto* afterBB = llvm::BasicBlock::Create(
            ctx, "drop." + d.name + ".re.after", currentFn_);
        builder_->CreateCondBr(live, dropBB, afterBB);
        builder_->SetInsertPoint(dropBB);
        builder_->CreateStore(llvm::ConstantInt::getFalse(ctx), d.flag);
        emitDropGlue(d.storage, d.type);
        if (!currentBlockTerminated()) builder_->CreateBr(afterBB);
        builder_->SetInsertPoint(afterBB);
    }

    // Compute an address (pointer) for an assignable place. Returns null
    // if the target shape isn't supported.
    llvm::Value* emitPlaceAddr(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            // Phase 17a: a by-ref capture's slot address IS the env pointer
            // into the enclosing variable's storage; storing through it makes
            // the mutation visible after the closure call.
            if (auto rit = refLocals_.find(id->name); rit != refLocals_.end())
                return rit->second.first;
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
            // Phase 17a: a by-ref capture roots a field chain at the env
            // pointer (it addresses the enclosing variable's value directly).
            if (auto rit = refLocals_.find(id->name); rit != refLocals_.end()) {
                outTy = lookupExprType(e);
                return rit->second.first;
            }
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
        // Phase 14a: attach this node's source position to every instruction
        // the raw emit produces, building the DWARF line table. Set before
        // emit (so child instructions inherit it) and restore the parent's
        // location after, so sibling expressions get their own line. No-op
        // when debug is off (debugLocFor returns an empty DebugLoc, but we
        // guard the set/restore to avoid disturbing the historic path).
        if (emitDebugInfo_ && diCurrentSP_) {
            llvm::DebugLoc saved = builder_->getCurrentDebugLocation();
            builder_->SetCurrentDebugLocation(debugLocFor(e.line, e.column));
            llvm::Value* v = emitExprWithCoercion(e);
            builder_->SetCurrentDebugLocation(saved);
            return v;
        }
        return emitExprWithCoercion(e);
    }

    llvm::Value* emitExprWithCoercion(const ast::Expr& e) {
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
        // Phase 15: boolean literal -> i1 constant (1/0).
        if (auto* bl = dynamic_cast<const ast::BoolLitExpr*>(&e)) {
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx_),
                                          bl->value ? 1 : 0);
        }
        // Phase 15: prefix unary operators.
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            return emitUnary(*un);
        }
        if (auto* sl = dynamic_cast<const ast::StringLitExpr*>(&e)) {
            return emitStringLit(*sl);
        }
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            // Phase 17a: a by-ref capture reads through the env pointer into
            // the enclosing variable's storage.
            if (auto rit = refLocals_.find(id->name); rit != refLocals_.end()) {
                return builder_->CreateLoad(rit->second.second,
                                            rit->second.first, id->name);
            }
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
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            return emitCallValue(*cv);
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
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            return emitSlice(*se);
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            return emitAwait(*ae);
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
        // cap = 0 marks a read-only literal view (not heap-owned), so a
        // later string_push_str copies before growing rather than
        // realloc'ing the global.
        v = builder_->CreateInsertValue(
            v, llvm::ConstantInt::get(i64Ty, 0), {2}, "str_cap");
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
            // Phase 16: a field initializer consumes its value into the struct.
            values[name] = emitConsume(*expr);
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
        // Phase 17a: `&x` / `&mut x` of a by-ref capture is the env pointer —
        // it already addresses the enclosing variable's storage.
        if (auto rit = refLocals_.find(id->name); rit != refLocals_.end())
            return rit->second.first;
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

    // Phase 13b: `&v[a..b]` — build the slice fat pointer { ptr, len } where
    // ptr = vec.data + start*stride and len = end - start. The element stride
    // comes from the Vec's element type (the slice's typeArgs[0]); the MVP
    // exercises i64. The operand may be a `Vec<T>` value or a `&Vec<T>`.
    llvm::Value* emitSlice(const ast::SliceExpr& se) {
        auto& ctx = *ctx_;
        auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
        auto* vecTy = structTypes_["Vec"];
        auto* sliceTy = structTypes_["Slice"];

        // Resolve the operand's data pointer. For a `&Vec` (pointer) operand
        // we GEP+load the data field; for a `Vec` value we ExtractValue.
        TypePtr opTy = lookupExprType(*se.operand);
        if (opTy) opTy = resolveInInstance(opTy);
        llvm::Value* dataPtr = nullptr;
        // Element type for the stride.
        TypePtr elemTy = makeInt();
        TypePtr sliceTyK = lookupExprType(se);
        if (sliceTyK) {
            sliceTyK = resolveInInstance(sliceTyK);
            if (!sliceTyK->typeArgs.empty()) elemTy = sliceTyK->typeArgs[0];
        }
        llvm::Type* elemLlvm = mapKardashevType(elemTy);

        if (opTy && opTy->kind == TypeKind::Ref) {
            // &Vec: emitExpr gives a pointer to the Vec; load its data field.
            llvm::Value* vecPtr = emitExpr(*se.operand);
            auto* dp = builder_->CreateStructGEP(vecTy, vecPtr, 0, "vec_data_p");
            dataPtr = builder_->CreateLoad(i8PtrTy, dp, "vec_data");
        } else {
            // Vec value: extract the data field directly.
            llvm::Value* vecVal = emitExpr(*se.operand);
            dataPtr = builder_->CreateExtractValue(vecVal, {0}, "vec_data");
        }

        llvm::Value* start = emitExpr(*se.start);
        llvm::Value* end = emitExpr(*se.end);
        // ptr = data + start (GEP in element units handles the stride).
        llvm::Value* basePtr =
            builder_->CreateGEP(elemLlvm, dataPtr, start, "slice_ptr");
        llvm::Value* len = builder_->CreateSub(end, start, "slice_len");

        llvm::Value* agg = llvm::UndefValue::get(sliceTy);
        agg = builder_->CreateInsertValue(agg, basePtr, {0}, "slice.ptr");
        agg = builder_->CreateInsertValue(agg, len, {1}, "slice.len");
        return agg;
    }

    // Phase 11: `Box::new(v)` — malloc sizeof(T), move v into the heap slot,
    // return the (opaque) heap pointer. That pointer is the `Box<T>` value,
    // and is exactly what a `&dyn`/`Box<dyn>` coercion uses as the data slot.
    llvm::Value* emitBoxNew(const ast::BoxNewExpr& bn) {
        llvm::Value* inner = emitExpr(*bn.value);
        clearDropFlagIfMoved(*bn.value); // Phase 16: value moves into the box
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

    // Phase 15: lower a prefix unary operator. `-x` is integer negation
    // (`0 - x` via CreateNeg); `!x` is logical not on an i1 (CreateNot ==
    // `xor i1 x, true`). The typechecker has already constrained the operand
    // to i64 / bool respectively.
    llvm::Value* emitUnary(const ast::UnaryExpr& un) {
        llvm::Value* v = emitExpr(*un.operand);
        switch (un.op) {
        case ast::UnaryOp::Neg:
            return builder_->CreateNeg(v, "neg");
        case ast::UnaryOp::Not:
            return builder_->CreateNot(v, "not");
        }
        return v; // unreachable
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
        // Phase 19: lazily declare the OS-thread + Mutex runtime (pthread
        // externs + trampoline + thread_*/mutex_* bodies) the first time any
        // of those builtins is called, then fall through to the normal
        // declaredFns_ dispatch below. Keeps thread-free programs' IR pristine.
        if (call.callee == "thread_spawn" || call.callee == "thread_join" ||
            call.callee == "mutex_new" || call.callee == "mutex_lock" ||
            call.callee == "mutex_unlock" || call.callee == "mutex_get" ||
            call.callee == "mutex_set") {
            ensureThreadRuntime();
        }
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
                        args.push_back(emitConsume(*a)); // Phase 16: by-value move
                    }
                    // A void-returning call must NOT be named (LLVM rejects a
                    // name on a value-less instruction — e.g. a unit-returning
                    // FnMut closure like `inc()`).
                    const char* nm = llvmFnTy->getReturnType()->isVoidTy()
                                         ? ""
                                         : "indir_call";
                    return builder_->CreateCall(llvmFnTy, fnPtr, args, nm);
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
                for (const auto& a : call.args) args.push_back(emitConsume(*a));
                return builder_->CreateCall(fn, args, "call_" + call.callee);
            }
            // Phase 17b: `block_on<T>` is a generic built-in with no AST body
            // — synthesize a per-result-type executor (mirrors vec_*). T is
            // the call-site type arg (the future's result type).
            if (call.callee == "block_on" && !concreteTypeArgs.empty()) {
                llvm::Function* fn = getOrEmitBlockOn(concreteTypeArgs[0]);
                std::vector<llvm::Value*> args;
                args.reserve(call.args.size());
                for (const auto& a : call.args) args.push_back(emitConsume(*a));
                return builder_->CreateCall(fn, args, "call_block_on");
            }
            // Phase 18: `spawn<T>(Future<T>) -> i64` and `join<T>(i64) -> T`
            // — generic executor built-ins with no AST body; synthesize per T.
            if (call.callee == "spawn" && !concreteTypeArgs.empty()) {
                llvm::Function* fn = getOrEmitSpawn(concreteTypeArgs[0]);
                std::vector<llvm::Value*> args;
                args.reserve(call.args.size());
                for (const auto& a : call.args) args.push_back(emitConsume(*a));
                return builder_->CreateCall(fn, args, "call_spawn");
            }
            if (call.callee == "join" && !concreteTypeArgs.empty()) {
                // `join<T>`'s T appears only in return position, so when the
                // result is discarded (e.g. `join(h);` purely to drive a task
                // to completion) inference can leave it an unsolved Var. The
                // executor stores results uniformly, so an unconstrained join
                // just reads an i64-shaped slot — default the Var to i64 so it
                // codegens (the value is unused anyway).
                TypePtr jt = resolve(concreteTypeArgs[0]);
                if (jt->kind == TypeKind::Var) jt = makeInt();
                llvm::Function* fn = getOrEmitJoin(jt);
                std::vector<llvm::Value*> args;
                args.reserve(call.args.size());
                for (const auto& a : call.args) args.push_back(emitConsume(*a));
                return builder_->CreateCall(fn, args, "call_join");
            }
            // Phase 17b: `hashmap_*<V>` are generic built-ins with no AST body
            // — synthesize a per-value-type specialization (key fixed i64).
            if ((call.callee == "hashmap_new" ||
                 call.callee == "hashmap_insert" ||
                 call.callee == "hashmap_get" ||
                 call.callee == "hashmap_len") &&
                !concreteTypeArgs.empty()) {
                llvm::Function* fn =
                    getOrEmitHashMapOp(call.callee, concreteTypeArgs[0]);
                if (!fn) {
                    errors_.push_back(
                        "codegen: cannot specialize " + call.callee);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*ctx_), 0);
                }
                std::vector<llvm::Value*> args;
                args.reserve(call.args.size());
                for (const auto& a : call.args) args.push_back(emitConsume(*a));
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
                args.push_back(emitConsume(*a));
            }
            return builder_->CreateCall(fn, args, "call_" + call.callee);
        }

        if (auto it = declaredFns_.find(call.callee);
            it != declaredFns_.end()) {
            llvm::Function* fn = it->second;
            std::vector<llvm::Value*> args;
            args.reserve(call.args.size());
            for (const auto& a : call.args) {
                args.push_back(emitConsume(*a));
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

    // Phase 17a: call a fn VALUE produced by an arbitrary expression
    // (`(s.f)(x)`, `(getCallback())(args)`). We evaluate the callee to its
    // fat pointer `{ fn, env }` (emitExpr already lowers a field-held fn
    // value, a closure, or a bare fn name to this representation), then
    // dispatch exactly like the Phase 10b indirect-call path: extract fn +
    // env, and call `fn(env, args...)` through the env-calling convention
    // type rebuilt from the callee's Kardashev Function type.
    llvm::Value* emitCallValue(const ast::CallValueExpr& cv) {
        TypePtr fnTy = lookupExprType(*cv.callee);
        if (fnTy) fnTy = resolve(fnTy);
        if (!fnTy || fnTy->kind != TypeKind::Function) {
            errors_.push_back(
                "codegen: called value has no Function type at call site");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        auto* llvmFnTy = envCalleeType(fnTy);
        llvm::Value* fatVal = emitExpr(*cv.callee); // the `{ fn, env }` value
        auto* fnPtr = builder_->CreateExtractValue(fatVal, {0}, "callee.fn");
        auto* envPtr = builder_->CreateExtractValue(fatVal, {1}, "callee.env");
        std::vector<llvm::Value*> args;
        args.reserve(cv.args.size() + 1);
        args.push_back(envPtr);
        for (const auto& a : cv.args) {
            args.push_back(emitConsume(*a)); // Phase 16: by-value move
        }
        // A void-returning call must not be named (LLVM rejects a name on a
        // value-less instruction).
        const char* nm =
            llvmFnTy->getReturnType()->isVoidTy() ? "" : "indir_call";
        return builder_->CreateCall(llvmFnTy, fnPtr, args, nm);
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
            // Phase 16: a constructor argument is moved into the payload.
            llvm::Value* v = emitConsume(*call.args[i]);
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
        // Phase 16: `?` consumes (destructures) its operand — a bare binding is
        // moved here, so it must not also be dropped by the enclosing scope.
        clearDropFlagIfMoved(*te.operand);
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
            // Phase 16: the `?` early-return path must drop the function's
            // still-live locals (the new owner is the caller, via the Err).
            emitReturnDrops();
            if (!currentBlockTerminated()) builder_->CreateRet(propAgg);
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
        // Phase 16: a by-value `self` consumes the receiver (moves it into the
        // method, which drops it at its own exit); a `&self`/`&mut self` borrow
        // does not. Clear the receiver binding's drop flag in the by-value case.
        if (!selfByRef) clearDropFlagIfMoved(*mc.receiver);
        args.push_back(recv);
        // Method arguments are moved into the method by value.
        for (const auto& a : mc.args) {
            llvm::Value* av = emitExpr(*a);
            clearDropFlagIfMoved(*a);
            args.push_back(av);
        }
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
        // Phase 16: matching consumes (destructures) the scrutinee — a bare
        // binding is moved into the match, so the enclosing scope must not drop
        // it. (Payload bindings introduced by patterns are not yet tracked for
        // drop; matching on droppable-payload enums is a documented limitation.)
        clearDropFlagIfMoved(*me.scrutinee);
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
        loopDropDepth_.push_back(dropScopes_.size()); // Phase 16
        emitExpr(*we.body);
        loopDropDepth_.pop_back();
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
        loopDropDepth_.push_back(dropScopes_.size()); // Phase 16
        emitExpr(*le.body);
        loopDropDepth_.pop_back();
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
        // Phase 13a: general `for` over an arbitrary Iterator. The
        // typechecker marked this loop and synthesized a `__it.next()` call;
        // lower to `{ let mut __it = <iter>; loop { match __it.next() {
        // Some(x) => body, None => break } } }`. Ranges (iteratorDesugar ==
        // false) keep the Phase 9 direct induction-variable lowering below.
        if (fe.iteratorDesugar && fe.nextCall) {
            return emitForIterator(fe);
        }
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
        loopDropDepth_.push_back(dropScopes_.size()); // Phase 16
        emitExpr(*fe.body);
        loopDropDepth_.pop_back();
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

    // Phase 13a: lower a `for x in <iter>` over an arbitrary Iterator to the
    // Iterator::next desugar. The iterator value is spilled into a stack slot
    // so the synthetic `__it.next()` call (a `&mut self` method) can mutate it
    // in place across iterations (the receiver IdentExpr resolves to this
    // alloca via `locals_`). Each iteration: call next(), test the returned
    // Option's tag; Some -> bind payload to the loop var + run body + loop,
    // None -> exit.
    llvm::Value* emitForIterator(const ast::ForExpr& fe) {
        auto* i32Ty = llvm::Type::getInt32Ty(*ctx_);

        // Spill the iterator into an addressable slot and register the
        // synthetic slot name so the next() receiver resolves to it.
        TypePtr iterTy = lookupExprType(*fe.iter);
        llvm::Value* iterVal = emitExpr(*fe.iter);
        llvm::Type* iterLlvm = iterVal->getType();
        auto* iterSlot =
            builder_->CreateAlloca(iterLlvm, nullptr, fe.iterSlotName);
        builder_->CreateStore(iterVal, iterSlot);
        llvm::AllocaInst* savedLocal = nullptr;
        bool hadLocal = locals_.count(fe.iterSlotName) > 0;
        if (hadLocal) savedLocal = locals_[fe.iterSlotName];
        locals_[fe.iterSlotName] = iterSlot;

        // Option<i64> layout for the next() result.
        TypePtr optTy = lookupExprType(*fe.nextCall);
        if (!optTy || resolveInInstance(optTy)->kind != TypeKind::Enum) {
            errors_.push_back("codegen: for-Iterator next() has no Option type");
            return nullptr;
        }
        optTy = resolveInInstance(optTy);
        mapKardashevType(optTy); // ensure LLVM struct + payload indices exist
        const std::string optMangled =
            mangleStructInstance(optTy->enumName, optTy->typeArgs);
        unsigned someIdx = variantIndexInEnum(optTy, "Some");
        const auto& optPayloadSlots = enumPayloadIndices_[optMangled];

        auto* headerBB =
            llvm::BasicBlock::Create(*ctx_, "foriter.header", currentFn_);
        auto* bodyBB =
            llvm::BasicBlock::Create(*ctx_, "foriter.body", currentFn_);
        auto* exitBB =
            llvm::BasicBlock::Create(*ctx_, "foriter.exit", currentFn_);

        builder_->CreateBr(headerBB);
        builder_->SetInsertPoint(headerBB);
        // Drive the iterator: __it.next() (the &mut self call mutates the
        // slot in place).
        llvm::Value* optVal = emitMethodCall(*fe.nextCall);
        llvm::Value* tag =
            builder_->CreateExtractValue(optVal, {0}, "foriter.tag");
        llvm::Value* isSome = builder_->CreateICmpEQ(
            tag, llvm::ConstantInt::get(i32Ty, someIdx), "foriter.issome");
        builder_->CreateCondBr(isSome, bodyBB, exitBB);

        builder_->SetInsertPoint(bodyBB);
        // Bind the loop variable to Some's payload.
        if (auto* vp = dynamic_cast<const ast::VarPat*>(fe.pattern.get())) {
            llvm::Value* payload = builder_->CreateExtractValue(
                optVal, {optPayloadSlots[someIdx][0]}, "foriter.elem");
            auto* vAlloca = builder_->CreateAlloca(
                payload->getType(), nullptr, vp->name);
            builder_->CreateStore(payload, vAlloca);
            locals_[vp->name] = vAlloca;
        }
        // continue -> header (re-calls next()); break -> exit.
        loopFrames_.push_back({headerBB, exitBB, nullptr});
        loopDropDepth_.push_back(dropScopes_.size()); // Phase 16
        emitExpr(*fe.body);
        loopDropDepth_.pop_back();
        loopFrames_.pop_back();
        if (!currentBlockTerminated()) builder_->CreateBr(headerBB);

        builder_->SetInsertPoint(exitBB);
        // Restore any prior binding of the slot name (defensive; the name is
        // freshly generated per loop so collisions shouldn't occur).
        if (hadLocal) locals_[fe.iterSlotName] = savedLocal;
        else locals_.erase(fe.iterSlotName);
        (void)iterTy;
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
            clearDropFlagIfMoved(*be.value); // Phase 16: break value moves out
            if (frame.breakValueAlloca) {
                builder_->CreateStore(v, frame.breakValueAlloca);
            }
        }
        // Phase 16: leaving the loop drops the locals of every scope opened
        // inside the loop body before branching to the exit.
        emitLoopExitDrops();
        if (!currentBlockTerminated()) builder_->CreateBr(frame.breakBB);
        return nullptr;
    }

    // Phase 9: `continue`. Branches to the innermost loop's continue
    // target (header for while/loop, step block for for).
    llvm::Value* emitContinue(const ast::ContinueExpr& ce) {
        if (loopFrames_.empty()) {
            errors_.push_back("codegen: `continue` outside loop");
            return nullptr;
        }
        // Phase 16: `continue` also leaves the loop-body scopes (this
        // iteration's locals are dropped before jumping to the loop header /
        // step block).
        emitLoopExitDrops();
        if (!currentBlockTerminated())
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
        // Phase 17a: a by-VALUE capture stores the value (its mapped LLVM
        // type); a by-REFERENCE (FnMut) capture stores a POINTER to the
        // enclosing variable's alloca (an opaque `ptr`), so the slot type is
        // i8* regardless of the captured value's type.
        std::vector<llvm::Type*> capLlvmTys;   // env slot type per capture
        std::vector<llvm::Type*> capValueTys;  // captured value's own type
        std::vector<TypePtr> capKdTys;
        capLlvmTys.reserve(cl.captures.size());
        capValueTys.reserve(cl.captures.size());
        capKdTys.reserve(cl.captures.size());
        for (const auto& cap : cl.captures) {
            TypePtr ct = resolveInInstance(cap.type);
            capKdTys.push_back(ct);
            llvm::Type* valTy = mapKardashevType(ct);
            capValueTys.push_back(valTy);
            capLlvmTys.push_back(cap.byRef ? static_cast<llvm::Type*>(i8PtrTy)
                                           : valTy);
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
                const std::string& name = cl.captures[i].name;
                auto lit = locals_.find(name);
                if (lit == locals_.end()) {
                    errors_.push_back(
                        "codegen: closure capture '" + name +
                        "' not a local in the enclosing scope");
                    continue;
                }
                auto* slot = builder_->CreateStructGEP(
                    envTy, envPtr, i, "cap_slot_" + name);
                if (cl.captures[i].byRef) {
                    // Phase 17a (FnMut): store a POINTER to the enclosing
                    // alloca. Reads/writes in the body go through it, so
                    // mutations persist after the call. The original binding
                    // keeps ownership (no move, no drop-flag clear) — it is
                    // borrowed for the closure's lifetime, which (MVP) does not
                    // outlive this scope.
                    builder_->CreateStore(lit->second, slot);
                    continue;
                }
                // By-value capture (Phase 10b): load + copy the value in.
                llvm::Value* loaded = builder_->CreateLoad(
                    lit->second->getAllocatedType(), lit->second,
                    "cap_" + name);
                builder_->CreateStore(loaded, slot);
                // Phase 16: a `move`-style capture takes the value by value
                // into the heap env. If the captured local is droppable, treat
                // the capture as a move so the enclosing scope does not also
                // free it (which would dangle the env's copy). The env itself
                // is not freed today — a documented leak, but never a UAF or
                // double-free.
                ast::IdentExpr capId;
                capId.name = name;
                clearDropFlagIfMoved(capId);
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
        auto savedRefLocals = std::move(refLocals_); // Phase 17a
        auto savedLoopFrames = std::move(loopFrames_);
        TypePtr savedRetTy = currentFnReturnType_;
        // Phase 16: the closure body is a sibling function with its own Drop
        // scope stack; save + reset (mirrors locals_/loopFrames_).
        auto savedDropScopes = std::move(dropScopes_);
        auto savedLoopDropDepth = std::move(loopDropDepth_);

        locals_.clear();
        localTypes_.clear();
        refLocals_.clear();
        loopFrames_.clear();
        dropScopes_.clear();
        loopDropDepth_.clear();
        currentFn_ = closureFn;
        currentFnReturnType_ = resolveInInstance(fnTy->ret);

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", closureFn);
        builder_->SetInsertPoint(entry);
        // Phase 16: open the closure-body function scope (its by-value params
        // are dropped at its exit, just like a top-level fn's).
        pushDropScope();

        // Prologue: reload captures from env into locals. We bitcast the
        // i8* env arg to the env struct pointer (opaque pointers make this a
        // no-op, but the GEPs use envTy). Captures are by-value copies living
        // in the (leaked) env; we do NOT drop them in the closure body, so
        // they aren't registered as owning locals.
        llvm::Value* envArg = closureFn->getArg(0);
        for (unsigned i = 0; i < cl.captures.size(); ++i) {
            const std::string& name = cl.captures[i].name;
            auto* slot = builder_->CreateStructGEP(
                envTy, envArg, i, "cap_slot_" + name);
            if (cl.captures[i].byRef) {
                // Phase 17a (FnMut): the env slot holds a POINTER to the
                // enclosing variable's storage. Load it and register the name
                // as a by-ref local; reads/writes go straight through the
                // pointer (no local copy), so the mutation is observed by the
                // enclosing scope across calls. The value type drives loads
                // (capValueTys[i], e.g. i64), not the i8* slot type.
                llvm::Value* ptr = builder_->CreateLoad(
                    capLlvmTys[i], slot, "capref_" + name);
                refLocals_[name] = {ptr, capValueTys[i]};
                continue;
            }
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
            registerDroppableLocal(cl.params[i].name, alloca, pt);
        }

        llvm::Value* bodyVal = emitExpr(*cl.body);
        // Drop the closure body scope (params) on fall-through; clear the flag
        // of a moved-out tail value first.
        if (!currentBlockTerminated()) {
            if (auto* be = dynamic_cast<const ast::BlockExpr*>(cl.body.get())) {
                if (be->tail) clearDropFlagIfMoved(*be->tail);
            } else {
                clearDropFlagIfMoved(*cl.body);
            }
            emitScopeDrops(dropScopes_.back());
        }
        dropScopes_.pop_back();
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
        refLocals_ = std::move(savedRefLocals); // Phase 17a
        loopFrames_ = std::move(savedLoopFrames);
        currentFnReturnType_ = savedRetTy;
        dropScopes_ = std::move(savedDropScopes);
        loopDropDepth_ = std::move(savedLoopDropDepth);
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
            // A unit-valued if where both branches end in a statement with no
            // tail (e.g. `if c { print(1); } else { print(0); }` used as a
            // statement) produces no SSA value — there is nothing to phi.
            // Guard so we don't deref a null branch value (would crash). Only
            // build the merge PHI when both branches actually yield a value;
            // the value-producing path is unchanged.
            if (!thenV || !elseV) return nullptr;
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
                       const TypeCheckResult& tc,
                       bool emitDebugInfo,
                       const std::string& sourceFile,
                       OptLevel optLevel) {
    Codegen cg(tc, emitDebugInfo, sourceFile, optLevel);
    cg.run(program);
    return cg.finish();
}

} // namespace kardashev
