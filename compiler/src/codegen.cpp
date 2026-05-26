#include "kardashev/codegen.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
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
        declareAllStructs();
        declareAllFunctions(program);
        for (const auto& fn : program.functions) {
            emitFunction(fn);
        }
    }

    CodegenResult finish() {
        std::string verifyErrs;
        llvm::raw_string_ostream os(verifyErrs);
        if (llvm::verifyModule(*module_, &os)) {
            errors_.push_back("module verification failed: " + verifyErrs);
        }
        return {std::move(ctx_), std::move(module_), std::move(errors_)};
    }

private:
    const TypeCheckResult& tc_;
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::vector<std::string> errors_;

    // Module-wide: fn name -> LLVM Function declaration.
    std::unordered_map<std::string, llvm::Function*> declaredFns_;

    // Module-wide: struct name -> LLVM struct type.
    std::unordered_map<std::string, llvm::StructType*> structTypes_;

    // Per-fn locals: variable name -> alloca pointer.
    std::unordered_map<std::string, llvm::AllocaInst*> locals_;
    llvm::Function* currentFn_ = nullptr;

    // Create one opaque llvm::StructType per kardashev struct, then set
    // bodies in a second pass so struct fields can reference any other
    // struct regardless of declaration order.
    void declareAllStructs() {
        for (const auto& [name, _ty] : tc_.structs) {
            structTypes_[name] = llvm::StructType::create(*ctx_, name);
        }
        for (const auto& [name, ty] : tc_.structs) {
            std::vector<llvm::Type*> elems;
            elems.reserve(ty->structFields.size());
            for (const auto& [_fname, fty] : ty->structFields) {
                elems.push_back(mapKardashevType(fty));
            }
            structTypes_[name]->setBody(elems);
        }
    }

    llvm::Type* mapKardashevType(const TypePtr& t) {
        auto r = resolve(t);
        switch (r->kind) {
            case TypeKind::Int:  return llvm::Type::getInt64Ty(*ctx_);
            case TypeKind::Bool: return llvm::Type::getInt1Ty(*ctx_);
            case TypeKind::Unit: return llvm::Type::getVoidTy(*ctx_);
            case TypeKind::Struct: {
                auto it = structTypes_.find(r->structName);
                if (it != structTypes_.end()) return it->second;
                errors_.push_back("codegen: unresolved struct " + r->structName);
                return llvm::Type::getInt64Ty(*ctx_);
            }
            default:
                errors_.push_back("codegen: unsupported type for codegen");
                return llvm::Type::getInt64Ty(*ctx_);
        }
    }

    llvm::Type* mapTypeRef(const ast::TypeRef& tr) {
        if (tr.name == "i64") return llvm::Type::getInt64Ty(*ctx_);
        if (tr.name == "bool") return llvm::Type::getInt1Ty(*ctx_);
        auto it = structTypes_.find(tr.name);
        if (it != structTypes_.end()) return it->second;
        errors_.push_back("codegen: unknown type " + tr.name);
        return llvm::Type::getInt64Ty(*ctx_);
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

    void declareAllFunctions(const ast::Program& program) {
        for (const auto& fn : program.functions) {
            llvm::FunctionType* fty = fnTypeFromDecl(fn);
            auto* f = llvm::Function::Create(
                fty, llvm::Function::ExternalLinkage, fn.name, module_.get());
            unsigned i = 0;
            for (auto& arg : f->args()) {
                if (i < fn.params.size()) arg.setName(fn.params[i].name);
                ++i;
            }
            declaredFns_[fn.name] = f;
        }
    }

    bool currentBlockTerminated() const {
        auto* bb = builder_->GetInsertBlock();
        return bb && bb->getTerminator() != nullptr;
    }

    void emitFunction(const ast::FnDecl& fn) {
        currentFn_ = declaredFns_[fn.name];
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
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            auto it = locals_.find(id->name);
            if (it != locals_.end()) {
                auto* alloca = it->second;
                return builder_->CreateLoad(alloca->getAllocatedType(),
                                            alloca, id->name);
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
        errors_.push_back("codegen: unknown expression kind");
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
    }

    llvm::Value* emitStructLit(const ast::StructLitExpr& sl) {
        auto stIt = structTypes_.find(sl.structName);
        auto layoutIt = tc_.structs.find(sl.structName);
        if (stIt == structTypes_.end() || layoutIt == tc_.structs.end()) {
            errors_.push_back("codegen: unknown struct " + sl.structName);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        llvm::StructType* st = stIt->second;

        std::unordered_map<std::string, llvm::Value*> values;
        values.reserve(sl.fields.size());
        for (const auto& [name, expr] : sl.fields) {
            values[name] = emitExpr(*expr);
        }

        const auto& fields = layoutIt->second->structFields;
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
        TypePtr objTy = resolve(tyIt->second);
        if (objTy->kind != TypeKind::Struct) {
            errors_.push_back("codegen: field access on non-struct value");
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        for (unsigned i = 0; i < objTy->structFields.size(); ++i) {
            if (objTy->structFields[i].first == fe.fieldName) {
                llvm::Value* obj = emitExpr(*fe.object);
                return builder_->CreateExtractValue(obj, {i},
                                                    "f_" + fe.fieldName);
            }
        }
        errors_.push_back("codegen: no field " + fe.fieldName + " on struct " +
                          objTy->structName);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
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
        auto it = declaredFns_.find(call.callee);
        if (it == declaredFns_.end()) {
            errors_.push_back("codegen: undefined function " + call.callee);
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0);
        }
        llvm::Function* fn = it->second;
        std::vector<llvm::Value*> args;
        args.reserve(call.args.size());
        for (const auto& a : call.args) {
            args.push_back(emitExpr(*a));
        }
        return builder_->CreateCall(fn, args, "call_" + call.callee);
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
