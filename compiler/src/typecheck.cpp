#include "kardashev/typecheck.hpp"

#include <utility>

namespace kardashev {
namespace {

class TypeChecker {
public:
    TypeCheckResult check(const ast::Program& program) {
        // Pass 1: register every fn signature so calls can see siblings
        // (and the function can recurse into itself).
        for (const auto& fn : program.functions) {
            std::vector<TypePtr> argTypes;
            argTypes.reserve(fn.params.size());
            for (const auto& p : fn.params) {
                argTypes.push_back(resolveTypeRef(p.type));
            }
            TypePtr ret = resolveTypeRef(fn.returnType);
            if (functions_.count(fn.name)) {
                error("function redefined: " + fn.name, fn.line, fn.column);
            }
            functions_[fn.name] = makeFunction(std::move(argTypes), ret);
        }
        // Pass 2: type-check each fn body.
        for (const auto& fn : program.functions) {
            checkFunction(fn);
        }
        return {std::move(errors_), std::move(exprTypes_)};
    }

private:
    using Scope = std::unordered_map<std::string, TypePtr>;

    std::unordered_map<std::string, TypePtr> functions_;
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes_;
    std::vector<TypeError> errors_;
    std::vector<Scope> scopes_;
    TypePtr currentReturnType_;

    void error(std::string msg, std::size_t line, std::size_t col) {
        errors_.push_back({std::move(msg), line, col});
    }

    TypePtr resolveTypeRef(const ast::TypeRef& tr) {
        if (tr.name == "i64") return makeInt();
        if (tr.name == "bool") return makeBool();
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

    void checkFunction(const ast::FnDecl& fn) {
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
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            if (auto t = lookupLocal(id->name)) return t;
            auto fnIt = functions_.find(id->name);
            if (fnIt != functions_.end()) return fnIt->second;
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
        error("unknown expression kind", e.line, e.column);
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
        auto fnIt = functions_.find(call.callee);
        if (fnIt == functions_.end()) {
            error("unknown function: " + call.callee, call.line, call.column);
            // Still type-check the args so secondary errors surface.
            for (const auto& a : call.args) checkExpr(*a);
            return makeInt();
        }
        const TypePtr& fnType = fnIt->second;
        if (fnType->args.size() != call.args.size()) {
            error("function '" + call.callee + "' expects " +
                      std::to_string(fnType->args.size()) +
                      " arg(s), got " + std::to_string(call.args.size()),
                  call.line, call.column);
        }
        const std::size_t n =
            std::min(fnType->args.size(), call.args.size());
        for (std::size_t i = 0; i < n; ++i) {
            TypePtr argType = checkExpr(*call.args[i]);
            if (!unify(argType, fnType->args[i])) {
                error("argument " + std::to_string(i + 1) + " to '" +
                          call.callee + "' has type " +
                          typeToString(argType) + ", expected " +
                          typeToString(fnType->args[i]),
                      call.args[i]->line, call.args[i]->column);
            }
        }
        // Type-check any extra args even though we already errored on arity.
        for (std::size_t i = n; i < call.args.size(); ++i) {
            checkExpr(*call.args[i]);
        }
        return fnType->ret;
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

TypeCheckResult typecheck(const ast::Program& program) {
    TypeChecker tc;
    return tc.check(program);
}

} // namespace kardashev
