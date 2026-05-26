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
        // Pass 1a: register every struct and enum decl. To allow free
        // cross-references (struct field of enum type, enum payload of
        // struct type), we do this in two phases:
        //   (i)  create opaque Type entries for every struct/enum just by
        //        name; the inner field/variant vectors stay empty.
        //   (ii) resolve the field types / variant payload types now that
        //        every name is bound. Mutate the entries in-place.
        //
        // Phase (i): opaque registration.
        for (const auto& sd : program.structs) {
            if (structs_.count(sd.name) || enums_.count(sd.name)) {
                error("struct redefined: " + sd.name, sd.line, sd.column);
                continue;
            }
            structs_[sd.name] = makeStruct(sd.name, {});
        }
        for (const auto& ed : program.enums) {
            if (structs_.count(ed.name) || enums_.count(ed.name)) {
                error("enum redefined: " + ed.name, ed.line, ed.column);
                continue;
            }
            enums_[ed.name] = makeEnum(ed.name, {});
        }

        // Phase (ii): resolve struct field types and enum variant payloads.
        for (const auto& sd : program.structs) {
            auto it = structs_.find(sd.name);
            if (it == structs_.end()) continue; // duplicate
            // Only resolve fields once: if the registered entry was already
            // populated by a previous decl with the same name, skip.
            if (!it->second->structFields.empty()) continue;
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
            it->second->structFields = std::move(resolvedFields);
        }

        for (const auto& ed : program.enums) {
            auto it = enums_.find(ed.name);
            if (it == enums_.end()) continue; // duplicate
            if (!it->second->enumVariants.empty()) continue;
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
            it->second->enumVariants = std::move(resolvedVariants);
        }

        // Pass 1b: register every fn signature so calls can see siblings
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
        TypeCheckResult result;
        result.errors = std::move(errors_);
        result.exprTypes = std::move(exprTypes_);
        result.structs = std::move(structs_);
        result.enums = std::move(enums_);
        result.variantIndex = std::move(variantIndex_);
        result.matchTrees = std::move(matchTrees_);
        return result;
    }

private:
    using Scope = std::unordered_map<std::string, TypePtr>;

    std::unordered_map<std::string, TypePtr> functions_;
    std::unordered_map<std::string, TypePtr> structs_;
    std::unordered_map<std::string, TypePtr> enums_;
    // Variant name -> {enumName, index within that enum}.
    std::unordered_map<std::string, std::pair<std::string, unsigned>>
        variantIndex_;
    std::unordered_map<const ast::Expr*, TypePtr> exprTypes_;
    std::unordered_map<const ast::MatchExpr*,
                       std::unique_ptr<pattern_match::DecisionTree>>
        matchTrees_;
    std::vector<TypeError> errors_;
    std::vector<Scope> scopes_;
    TypePtr currentReturnType_;

    void error(std::string msg, std::size_t line, std::size_t col) {
        errors_.push_back({std::move(msg), line, col});
    }

    TypePtr resolveTypeRef(const ast::TypeRef& tr) {
        if (tr.name == "i64") return makeInt();
        if (tr.name == "bool") return makeBool();
        auto sit = structs_.find(tr.name);
        if (sit != structs_.end()) return sit->second;
        auto eit = enums_.find(tr.name);
        if (eit != enums_.end()) return eit->second;
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

    // Look up a variant by name in the global variant table. Returns the
    // (enumType, variant) pair, or {nullptr, nullptr} if unknown.
    std::pair<TypePtr, const EnumVariantType*> lookupVariant(
        const std::string& name) {
        auto it = variantIndex_.find(name);
        if (it == variantIndex_.end()) return {nullptr, nullptr};
        auto enumIt = enums_.find(it->second.first);
        if (enumIt == enums_.end()) return {nullptr, nullptr};
        const auto& variants = enumIt->second->enumVariants;
        unsigned idx = it->second.second;
        if (idx >= variants.size()) return {nullptr, nullptr};
        return {enumIt->second, &variants[idx]};
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
            // Fall through to variant table: a bare Ident resolving to a
            // unit constructor is the value of that constructor.
            auto [enumType, variant] = lookupVariant(id->name);
            if (variant) {
                if (!variant->payloadTypes.empty()) {
                    error("constructor " + id->name + " requires " +
                              std::to_string(variant->payloadTypes.size()) +
                              " argument(s)",
                          e.line, e.column);
                    return makeInt();
                }
                return enumType;
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
        error("unknown expression kind", e.line, e.column);
        return makeInt();
    }

    TypePtr checkStructLit(const ast::StructLitExpr& sl) {
        auto it = structs_.find(sl.structName);
        if (it == structs_.end()) {
            error("unknown struct: " + sl.structName, sl.line, sl.column);
            for (const auto& f : sl.fields) checkExpr(*f.second);
            return makeInt();
        }
        const TypePtr& structType = it->second;
        std::unordered_map<std::string, TypePtr> declared;
        declared.reserve(structType->structFields.size());
        for (const auto& df : structType->structFields) {
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
        for (const auto& df : structType->structFields) {
            if (!initialised.count(df.first)) {
                error("missing field '" + df.first + "' in struct '" +
                          sl.structName + "' literal",
                      sl.line, sl.column);
            }
        }
        return structType;
    }

    TypePtr checkField(const ast::FieldExpr& fe) {
        TypePtr objT = checkExpr(*fe.object);
        TypePtr r = resolve(objT);
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
        auto fnIt = functions_.find(call.callee);
        if (fnIt != functions_.end()) {
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
            for (std::size_t i = n; i < call.args.size(); ++i) {
                checkExpr(*call.args[i]);
            }
            return fnType->ret;
        }
        // Not a fn — fall back to variant constructor.
        auto [enumType, variant] = lookupVariant(call.callee);
        if (variant) {
            if (variant->payloadTypes.size() != call.args.size()) {
                error("constructor " + call.callee + " expects " +
                          std::to_string(variant->payloadTypes.size()) +
                          " arg(s), got " +
                          std::to_string(call.args.size()),
                      call.line, call.column);
            }
            const std::size_t n =
                std::min(variant->payloadTypes.size(), call.args.size());
            for (std::size_t i = 0; i < n; ++i) {
                TypePtr argType = checkExpr(*call.args[i]);
                if (!unify(argType, variant->payloadTypes[i])) {
                    error("argument " + std::to_string(i + 1) +
                              " to constructor " + call.callee +
                              " has type " + typeToString(argType) +
                              ", expected " +
                              typeToString(variant->payloadTypes[i]),
                          call.args[i]->line, call.args[i]->column);
                }
            }
            for (std::size_t i = n; i < call.args.size(); ++i) {
                checkExpr(*call.args[i]);
            }
            return enumType;
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
        if (auto w = pattern_match::checkExhaustiveness(
                scrutT, me.arms, enums_, variantIndex_)) {
            error("non-exhaustive match: missing pattern `" + w->text + "`",
                  me.line, me.column);
        }
        const bool armsClean = (errors_.size() == errsBeforeArms);
        if (armsClean) {
            // Redundancy: report each arm unreachable given the arms before it.
            auto redundant = pattern_match::findRedundantArms(
                scrutT, me.arms, enums_, variantIndex_);
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
                scrutT, me.arms, enums_, variantIndex_);
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
            auto [enumType, variant] = lookupVariant(vp->name);
            if (variant) {
                if (!variant->payloadTypes.empty()) {
                    error("constructor " + vp->name + " requires " +
                              std::to_string(variant->payloadTypes.size()) +
                              " argument(s) in pattern",
                          pat.line, pat.column);
                    return;
                }
                if (!unify(expected, enumType)) {
                    error("pattern matches enum " + enumType->enumName +
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
            auto [enumType, variant] = lookupVariant(cp->ctorName);
            if (!variant) {
                error("unknown constructor " + cp->ctorName,
                      pat.line, pat.column);
                // Still walk subpatterns so nested errors / bindings surface
                // against fresh vars.
                for (const auto& sp : cp->subpatterns) {
                    checkPattern(*sp, makeFreshVar(), bindings);
                }
                return;
            }
            if (!unify(expected, enumType)) {
                error("pattern matches enum " + enumType->enumName +
                          ", scrutinee is " + typeToString(expected),
                      pat.line, pat.column);
            }
            if (cp->subpatterns.size() != variant->payloadTypes.size()) {
                error("constructor " + cp->ctorName + " expects " +
                          std::to_string(variant->payloadTypes.size()) +
                          " arg(s), got " +
                          std::to_string(cp->subpatterns.size()),
                      pat.line, pat.column);
            }
            const std::size_t n =
                std::min(cp->subpatterns.size(), variant->payloadTypes.size());
            for (std::size_t i = 0; i < n; ++i) {
                checkPattern(*cp->subpatterns[i], variant->payloadTypes[i],
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
