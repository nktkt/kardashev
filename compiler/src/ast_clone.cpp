// v25 Phase 135: AST deep-clone. See ast_clone.hpp.
#include "kardashev/ast_clone.hpp"

namespace kardashev::ast {

namespace {
// Clone an optional child (null stays null).
ExprPtr cloneOpt(const ExprPtr& p) { return p ? cloneExpr(*p) : nullptr; }
PatternPtr clonePatOpt(const PatternPtr& p) {
    return p ? clonePattern(*p) : nullptr;
}
// Stamp the source position onto a freshly-made node.
template <class T> T* pos(T* n, const Expr& src) {
    n->line = src.line;
    n->column = src.column;
    return n;
}
} // namespace

PatternPtr clonePattern(const Pattern& p) {
    PatternPtr out;
    if (auto* li = dynamic_cast<const LitIntPat*>(&p)) {
        auto n = std::make_unique<LitIntPat>();
        n->value = li->value;
        out = std::move(n);
    } else if (dynamic_cast<const WildPat*>(&p)) {
        out = std::make_unique<WildPat>();
    } else if (auto* v = dynamic_cast<const VarPat*>(&p)) {
        auto n = std::make_unique<VarPat>();
        n->name = v->name;
        out = std::move(n);
    } else if (auto* c = dynamic_cast<const CtorPat*>(&p)) {
        auto n = std::make_unique<CtorPat>();
        n->ctorName = c->ctorName;
        for (const auto& sp : c->subpatterns)
            n->subpatterns.push_back(clonePatOpt(sp));
        out = std::move(n);
    } else if (auto* t = dynamic_cast<const TuplePat*>(&p)) {
        auto n = std::make_unique<TuplePat>();
        for (const auto& el : t->elements)
            n->elements.push_back(clonePatOpt(el));
        out = std::move(n);
    } else if (auto* o = dynamic_cast<const OrPat*>(&p)) {
        auto n = std::make_unique<OrPat>();
        for (const auto& alt : o->alternatives)
            n->alternatives.push_back(clonePatOpt(alt));
        out = std::move(n);
    } else if (auto* sp = dynamic_cast<const SlicePat*>(&p)) {
        auto n = std::make_unique<SlicePat>();
        n->elements = sp->elements;
        n->hasRest = sp->hasRest;
        out = std::move(n);
    }
    if (out) {
        out->line = p.line;
        out->column = p.column;
    }
    return out;
}

std::unique_ptr<BlockExpr> cloneBlock(const BlockExpr& b) {
    auto n = std::make_unique<BlockExpr>();
    n->line = b.line;
    n->column = b.column;
    for (const auto& s : b.stmts) n->stmts.push_back(cloneStmt(*s));
    n->tail = cloneOpt(b.tail);
    return n;
}

StmtPtr cloneStmt(const Stmt& s) {
    if (auto* let = dynamic_cast<const LetStmt*>(&s)) {
        auto n = std::make_unique<LetStmt>();
        n->name = let->name;
        n->value = cloneOpt(let->value);
        n->tupleNames = let->tupleNames;
        n->isMut = let->isMut;
        n->annotation = let->annotation; // TypeRef is immutable post-parse
        n->line = let->line;
        n->column = let->column;
        return n;
    }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&s)) {
        auto n = std::make_unique<ReturnStmt>();
        n->value = cloneOpt(r->value);
        n->line = r->line;
        n->column = r->column;
        return n;
    }
    if (auto* es = dynamic_cast<const ExprStmt*>(&s)) {
        auto n = std::make_unique<ExprStmt>();
        n->expr = cloneOpt(es->expr);
        n->line = es->line;
        n->column = es->column;
        return n;
    }
    if (auto* as = dynamic_cast<const AssignStmt*>(&s)) {
        auto n = std::make_unique<AssignStmt>();
        n->target = cloneOpt(as->target);
        n->value = cloneOpt(as->value);
        n->line = as->line;
        n->column = as->column;
        return n;
    }
    return nullptr;
}

ExprPtr cloneExpr(const Expr& e) {
    ExprPtr out;
    if (auto* x = dynamic_cast<const IntLitExpr*>(&e)) {
        auto n = std::make_unique<IntLitExpr>();
        n->value = x->value;
        n->suffixWidth = x->suffixWidth;
        n->suffixSigned = x->suffixSigned;
        out = std::move(n);
    } else if (auto* f = dynamic_cast<const FloatLitExpr*>(&e)) {
        auto n = std::make_unique<FloatLitExpr>();
        n->lexeme = f->lexeme;
        n->value = f->value;
        n->suffixWidth = f->suffixWidth;
        out = std::move(n);
    } else if (auto* b = dynamic_cast<const BoolLitExpr*>(&e)) {
        auto n = std::make_unique<BoolLitExpr>();
        n->value = b->value;
        out = std::move(n);
    } else if (auto* s = dynamic_cast<const StringLitExpr*>(&e)) {
        auto n = std::make_unique<StringLitExpr>();
        n->value = s->value;
        out = std::move(n);
    } else if (auto* c = dynamic_cast<const CharLitExpr*>(&e)) {
        auto n = std::make_unique<CharLitExpr>();
        n->codepoint = c->codepoint;
        out = std::move(n);
    } else if (auto* id = dynamic_cast<const IdentExpr*>(&e)) {
        auto n = std::make_unique<IdentExpr>();
        n->name = id->name;
        out = std::move(n);
    } else if (auto* bin = dynamic_cast<const BinaryExpr*>(&e)) {
        auto n = std::make_unique<BinaryExpr>();
        n->op = bin->op;
        n->lhs = cloneOpt(bin->lhs);
        n->rhs = cloneOpt(bin->rhs);
        out = std::move(n);
    } else if (auto* un = dynamic_cast<const UnaryExpr*>(&e)) {
        auto n = std::make_unique<UnaryExpr>();
        n->op = un->op;
        n->operand = cloneOpt(un->operand);
        out = std::move(n);
    } else if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
        auto n = std::make_unique<CallExpr>();
        n->callee = c->callee;
        n->wasPath = c->wasPath;
        n->pathQualifier = c->pathQualifier;
        for (const auto& a : c->args) n->args.push_back(cloneOpt(a));
        out = std::move(n);
    } else if (auto* cv = dynamic_cast<const CallValueExpr*>(&e)) {
        auto n = std::make_unique<CallValueExpr>();
        n->callee = cloneOpt(cv->callee);
        for (const auto& a : cv->args) n->args.push_back(cloneOpt(a));
        out = std::move(n);
    } else if (auto* sl = dynamic_cast<const StructLitExpr*>(&e)) {
        auto n = std::make_unique<StructLitExpr>();
        n->structName = sl->structName;
        for (const auto& fld : sl->fields)
            n->fields.emplace_back(fld.first, cloneOpt(fld.second));
        out = std::move(n);
    } else if (auto* fe = dynamic_cast<const FieldExpr*>(&e)) {
        auto n = std::make_unique<FieldExpr>();
        n->object = cloneOpt(fe->object);
        n->fieldName = fe->fieldName;
        out = std::move(n);
    } else if (auto* iff = dynamic_cast<const IfExpr*>(&e)) {
        auto n = std::make_unique<IfExpr>();
        n->cond = cloneOpt(iff->cond);
        n->thenBranch = cloneOpt(iff->thenBranch);
        n->elseBranch = cloneOpt(iff->elseBranch);
        out = std::move(n);
    } else if (auto* blk = dynamic_cast<const BlockExpr*>(&e)) {
        out = cloneBlock(*blk);
    } else if (auto* m = dynamic_cast<const MatchExpr*>(&e)) {
        auto n = std::make_unique<MatchExpr>();
        n->scrutinee = cloneOpt(m->scrutinee);
        for (const auto& arm : m->arms) {
            MatchArm a;
            a.pattern = clonePatOpt(arm.pattern);
            a.body = cloneOpt(arm.body);
            a.line = arm.line;
            a.column = arm.column;
            n->arms.push_back(std::move(a));
        }
        out = std::move(n);
    } else if (auto* w = dynamic_cast<const WhileExpr*>(&e)) {
        auto n = std::make_unique<WhileExpr>();
        n->cond = cloneOpt(w->cond);
        n->body = cloneOpt(w->body);
        out = std::move(n);
    } else if (auto* lp = dynamic_cast<const LoopExpr*>(&e)) {
        auto n = std::make_unique<LoopExpr>();
        n->body = cloneOpt(lp->body);
        out = std::move(n);
    } else if (auto* rg = dynamic_cast<const RangeExpr*>(&e)) {
        auto n = std::make_unique<RangeExpr>();
        n->start = cloneOpt(rg->start);
        n->end = cloneOpt(rg->end);
        n->inclusive = rg->inclusive;
        out = std::move(n);
    } else if (auto* fo = dynamic_cast<const ForExpr*>(&e)) {
        auto n = std::make_unique<ForExpr>();
        n->pattern = clonePatOpt(fo->pattern);
        n->iter = cloneOpt(fo->iter);
        n->body = cloneOpt(fo->body);
        // desugar fields are typecheck-populated; leave them unset.
        out = std::move(n);
    } else if (auto* br = dynamic_cast<const BreakExpr*>(&e)) {
        auto n = std::make_unique<BreakExpr>();
        n->value = cloneOpt(br->value);
        out = std::move(n);
    } else if (dynamic_cast<const ContinueExpr*>(&e)) {
        out = std::make_unique<ContinueExpr>();
    } else if (auto* tr = dynamic_cast<const TryExpr*>(&e)) {
        auto n = std::make_unique<TryExpr>();
        n->operand = cloneOpt(tr->operand);
        out = std::move(n);
    } else if (auto* rf = dynamic_cast<const RefExpr*>(&e)) {
        auto n = std::make_unique<RefExpr>();
        n->operand = cloneOpt(rf->operand);
        n->isMut = rf->isMut;
        out = std::move(n);
    } else if (auto* sc = dynamic_cast<const SliceExpr*>(&e)) {
        auto n = std::make_unique<SliceExpr>();
        n->operand = cloneOpt(sc->operand);
        n->start = cloneOpt(sc->start);
        n->end = cloneOpt(sc->end);
        out = std::move(n);
    } else if (auto* ar = dynamic_cast<const ArrayLitExpr*>(&e)) {
        auto n = std::make_unique<ArrayLitExpr>();
        for (const auto& el : ar->elements) n->elements.push_back(cloneOpt(el));
        n->repeatCount = cloneOpt(ar->repeatCount);
        out = std::move(n);
    } else if (auto* ix = dynamic_cast<const IndexExpr*>(&e)) {
        auto n = std::make_unique<IndexExpr>();
        n->object = cloneOpt(ix->object);
        n->index = cloneOpt(ix->index);
        out = std::move(n);
    } else if (auto* tl = dynamic_cast<const TupleLitExpr*>(&e)) {
        auto n = std::make_unique<TupleLitExpr>();
        for (const auto& el : tl->elements) n->elements.push_back(cloneOpt(el));
        out = std::move(n);
    } else if (auto* tf = dynamic_cast<const TupleFieldExpr*>(&e)) {
        auto n = std::make_unique<TupleFieldExpr>();
        n->object = cloneOpt(tf->object);
        n->index = tf->index;
        out = std::move(n);
    } else if (auto* aw = dynamic_cast<const AwaitExpr*>(&e)) {
        auto n = std::make_unique<AwaitExpr>();
        n->operand = cloneOpt(aw->operand);
        out = std::move(n);
    } else if (auto* ca = dynamic_cast<const CastExpr*>(&e)) {
        auto n = std::make_unique<CastExpr>();
        n->operand = cloneOpt(ca->operand);
        n->targetType = ca->targetType; // TypeRef is value-copyable
        out = std::move(n);
    } else if (auto* cl = dynamic_cast<const ClosureExpr*>(&e)) {
        auto n = std::make_unique<ClosureExpr>();
        n->params = cl->params; // ClosureParam is value-copyable
        n->body = cloneOpt(cl->body);
        // captures are typecheck-populated; leave empty.
        out = std::move(n);
    } else if (auto* mc = dynamic_cast<const MethodCallExpr*>(&e)) {
        auto n = std::make_unique<MethodCallExpr>();
        n->receiver = cloneOpt(mc->receiver);
        n->methodName = mc->methodName;
        for (const auto& a : mc->args) n->args.push_back(cloneOpt(a));
        out = std::move(n);
    } else if (auto* bx = dynamic_cast<const BoxNewExpr*>(&e)) {
        auto n = std::make_unique<BoxNewExpr>();
        n->value = cloneOpt(bx->value);
        out = std::move(n);
    }
    if (out) pos(out.get(), e);
    return out;
}

} // namespace kardashev::ast
