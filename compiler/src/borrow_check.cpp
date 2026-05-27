#include "kardashev/borrow_check.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kardashev {
namespace {

enum class OwnState { Owned, Moved };

// Each binding declaration gets a unique `declPos` (assigned in source
// order). All per-binding state lives in `bindings_` keyed by declPos —
// scopes_ only map name -> declPos for lookup, so shadowing works.
struct Binding {
    std::string name;
    int declPos = -1;
    OwnState state = OwnState::Owned;
    bool isMoveTyped = true;
    int sharedLoans = 0;
    bool mutLoanActive = false;
    std::size_t moveLine = 0;
    std::size_t moveCol = 0;
    // Last position (in the source-order walk) at which any IdentExpr or
    // RefExpr-of-Ident refers to this binding. Filled by the pre-pass.
    int lastUsePos = -1;
};

struct Loan {
    int borrowedDeclPos;       // binding being borrowed
    int borrowerDeclPos = -1;  // owning let-binding's declPos (-1 = temp)
    bool isMut = false;
    // Position at which this loan expires. For named borrows: equals the
    // borrower's lastUsePos. For temporaries: equals the enclosing
    // statement's last position.
    int expirePos = -1;
    std::size_t line = 0;
    std::size_t column = 0;
};

bool isCopyType(const TypePtr& t) {
    if (!t) return true;
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Int:
    case TypeKind::Bool:
    case TypeKind::Unit:
        return true;
    case TypeKind::Ref:
        // `&T` is Copy; `&mut T` is not — copying a mut-ref would create
        // aliased mutable access.
        return !r->refIsMut;
    case TypeKind::Var:
    case TypeKind::Function:
    case TypeKind::Struct:
    case TypeKind::Enum:
        return false;
    }
    return false;
}

class BorrowChecker {
public:
    BorrowChecker(const ast::Program& program, const TypeCheckResult& tc)
        : program_(program), tc_(tc) {}

    BorrowCheckResult run() {
        for (const auto& fn : program_.functions) checkFn(fn);
        for (const auto& impl : program_.impls) {
            for (const auto& fn : impl.methods) checkFn(fn);
        }
        return std::move(result_);
    }

private:
    const ast::Program& program_;
    const TypeCheckResult& tc_;
    BorrowCheckResult result_;
    int pos_ = 0;
    // declPos -> Binding (per-fn, reset on each checkFn).
    std::unordered_map<int, Binding> bindings_;
    // Active loans tracked as a flat list; we expire them lazily by
    // checking expirePos at each tick.
    std::vector<Loan> activeLoans_;
    // Scope stack: each entry maps a name to its current declPos. On
    // scope exit we pop; loans hang on to their declPos independently.
    std::vector<std::unordered_map<std::string, int>> scopes_;

    void error(std::string msg, std::size_t line, std::size_t col) {
        result_.errors.push_back({std::move(msg), line, col});
    }

    int lookupDeclPos(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return -1;
    }

    Binding* lookupBinding(const std::string& name) {
        int dp = lookupDeclPos(name);
        if (dp < 0) return nullptr;
        auto it = bindings_.find(dp);
        if (it == bindings_.end()) return nullptr;
        return &it->second;
    }

    int newBinding(const std::string& name, const TypePtr& ty) {
        int dp = pos_++;
        if (scopes_.empty()) scopes_.push_back({});
        scopes_.back()[name] = dp;
        Binding b;
        b.name = name;
        b.declPos = dp;
        b.state = OwnState::Owned;
        b.isMoveTyped = !isCopyType(ty);
        bindings_[dp] = std::move(b);
        return dp;
    }

    // Reconstruct a minimal TypePtr from an AST TypeRef so isCopyType
    // can classify it. We only care about the Copy/Move distinction.
    TypePtr paramTypeFromAst(const ast::TypeRef& tr) {
        if (tr.isRef) {
            ast::TypeRef inner = tr;
            inner.isRef = false;
            inner.refIsMut = false;
            return makeRef(paramTypeFromAst(inner), tr.refIsMut);
        }
        if (tr.name == "i64") return makeInt();
        if (tr.name == "bool") return makeBool();
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Struct;
        t->structName = tr.name;
        return t;
    }

    TypePtr typeOf(const ast::Expr& e) {
        auto it = tc_.exprTypes.find(&e);
        if (it == tc_.exprTypes.end()) return nullptr;
        return it->second;
    }

    // --- Pass 1: pre-pass that assigns positions and records the last
    // position at which each binding is referenced. Pass 1 mirrors Pass
    // 2's traversal order so positions agree.

    void prePass(const ast::Expr& e) {
        int myPos = pos_++;
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            int dp = lookupDeclPos(id->name);
            if (dp >= 0) {
                auto& b = bindings_[dp];
                b.lastUsePos = std::max(b.lastUsePos, myPos);
            }
            return;
        }
        if (dynamic_cast<const ast::IntLitExpr*>(&e)) return;
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            prePass(*bin->lhs);
            prePass(*bin->rhs);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args) prePass(*a);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            prePass(*mc->receiver);
            for (const auto& a : mc->args) prePass(*a);
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            prePass(*ie->cond);
            prePass(*ie->thenBranch);
            prePass(*ie->elseBranch);
            return;
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            prePassBlock(*block);
            return;
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields) prePass(*v);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            prePass(*fe->object);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            prePass(*me->scrutinee);
            for (const auto& arm : me->arms) {
                scopes_.push_back({});
                bindPattern(*arm.pattern, typeOf(*me->scrutinee),
                            /*prePass=*/true);
                prePass(*arm.body);
                scopes_.pop_back();
            }
            return;
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            prePass(*te->operand);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            // Treat `&x` as a use of x for lastUsePos accounting, so a
            // borrow-then-move is correctly ordered when we drop loans
            // in pass 2.
            prePass(*re->operand);
            return;
        }
    }

    void prePassBlock(const ast::BlockExpr& block) {
        scopes_.push_back({});
        for (const auto& stmt : block.stmts) {
            if (auto* let = dynamic_cast<const ast::LetStmt*>(stmt.get())) {
                prePass(*let->value);
                newBinding(let->name, typeOf(*let->value));
                continue;
            }
            if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(stmt.get())) {
                if (ret->value) prePass(*ret->value);
                continue;
            }
            if (auto* es = dynamic_cast<const ast::ExprStmt*>(stmt.get())) {
                prePass(*es->expr);
                continue;
            }
        }
        if (block.tail) prePass(*block.tail);
        scopes_.pop_back();
    }

    // --- Pass 2: actual borrow check ---

    void checkFn(const ast::FnDecl& fn) {
        scopes_.clear();
        bindings_.clear();
        activeLoans_.clear();
        pos_ = 0;
        scopes_.push_back({});
        // Pass 1: pre-walk params + body to fill lastUsePos for every
        // binding (params + lets + pattern names).
        for (const auto& p : fn.params) {
            newBinding(p.name, paramTypeFromAst(p.type));
        }
        if (fn.body) prePass(*fn.body);

        // Reset for Pass 2. We KEEP `bindings_` (lastUsePos is now
        // populated for every binding-decl-pos), but reset scopes and
        // re-walk with checks. We DON'T reset `pos_` because we want the
        // exact same position numbers — Pass 2 increments through them
        // identically.
        for (auto& [_dp, b] : bindings_) b.state = OwnState::Owned;
        scopes_.clear();
        scopes_.push_back({});
        // Re-bind params at the same declPos numbers used in pass 1. We
        // do that by reusing the bindings_ entries: just rebind the name
        // -> declPos mapping in the current scope.
        int paramDeclPos = 0; // pass 1 assigned params declPos 0..N-1
        for (const auto& p : fn.params) {
            scopes_.back()[p.name] = paramDeclPos++;
        }
        // Pass 2 uses a fresh position counter starting at the same
        // value pass 1 ended with all params bound. The trick: we don't
        // need to re-emit positions during pass 2 — we only need to keep
        // the loan-expire comparisons consistent. Restart pos_ at the
        // param count and the same walk order will reproduce identical
        // positions.
        pos_ = static_cast<int>(fn.params.size());
        if (fn.body) consume(*fn.body, /*expectExpire=*/-1);
    }

    // --- Pass 2: walk an expression. `consume` says "treat the result
    // value as moved into the surrounding context". Returns the highest
    // position visited inside this subtree — the caller's enclosing
    // statement passes this back as `expectExpire` to retire temporary
    // borrows at end-of-statement.
    int consume(const ast::Expr& e, int expectExpire) {
        int startPos = pos_++;
        int lastInSubtree = startPos;

        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            consumeIdent(*id);
            return startPos;
        }
        if (dynamic_cast<const ast::IntLitExpr*>(&e)) {
            return startPos;
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*bin->lhs, expectExpire));
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*bin->rhs, expectExpire));
            return lastInSubtree;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            for (const auto& a : call->args) {
                lastInSubtree = std::max(lastInSubtree,
                                           consume(*a, expectExpire));
            }
            return lastInSubtree;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*mc->receiver, expectExpire));
            for (const auto& a : mc->args) {
                lastInSubtree = std::max(lastInSubtree,
                                           consume(*a, expectExpire));
            }
            return lastInSubtree;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*ie->cond, expectExpire));
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*ie->thenBranch, expectExpire));
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*ie->elseBranch, expectExpire));
            return lastInSubtree;
        }
        if (auto* block = dynamic_cast<const ast::BlockExpr*>(&e)) {
            return consumeBlock(*block);
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            for (const auto& [_n, v] : sl->fields) {
                lastInSubtree = std::max(lastInSubtree,
                                           consume(*v, expectExpire));
            }
            return lastInSubtree;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            if (auto* ido = dynamic_cast<const ast::IdentExpr*>(
                    fe->object.get())) {
                checkRead(*ido);
                // Field access still bumps the IdentExpr's pos counter
                // (mirroring pass 1's walk) so subsequent positions stay
                // in sync.
                ++pos_;
            } else {
                lastInSubtree = std::max(lastInSubtree,
                                           consume(*fe->object, expectExpire));
            }
            return lastInSubtree;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            lastInSubtree = std::max(lastInSubtree,
                                       consume(*me->scrutinee, expectExpire));
            for (const auto& arm : me->arms) {
                scopes_.push_back({});
                bindPattern(*arm.pattern, typeOf(*me->scrutinee),
                            /*prePass=*/false);
                lastInSubtree = std::max(lastInSubtree,
                                           consume(*arm.body, expectExpire));
                scopes_.pop_back();
            }
            return lastInSubtree;
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            return std::max(lastInSubtree,
                            consume(*te->operand, expectExpire));
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            return handleRefExpr(*re, expectExpire);
        }
        return lastInSubtree;
    }

    int consumeBlock(const ast::BlockExpr& block) {
        scopes_.push_back({});
        int last = pos_ - 1;
        for (const auto& stmt : block.stmts) {
            if (auto* let = dynamic_cast<const ast::LetStmt*>(stmt.get())) {
                // Only `let r = &x;` (RefExpr directly as RHS) creates a
                // *named* borrow whose lifetime ties to `r`. RefExprs
                // nested inside the RHS (e.g. `read(&p)`) are temporary
                // borrows that expire at end-of-statement.
                const bool rhsIsRef =
                    dynamic_cast<const ast::RefExpr*>(let->value.get()) !=
                    nullptr;
                int rhsEnd = consume(*let->value, /*expectExpire=*/-1);
                int borrowerDp = pos_++;
                scopes_.back()[let->name] = borrowerDp;
                if (rhsIsRef) attachLoanToBorrower(borrowerDp);
                retireExpiredLoans(rhsEnd);
                last = std::max(last, rhsEnd);
                continue;
            }
            if (auto* ret = dynamic_cast<const ast::ReturnStmt*>(stmt.get())) {
                if (ret->value) {
                    int p = consume(*ret->value, /*expectExpire=*/-1);
                    retireExpiredLoans(p);
                    last = std::max(last, p);
                }
                continue;
            }
            if (auto* es = dynamic_cast<const ast::ExprStmt*>(stmt.get())) {
                int p = consume(*es->expr, /*expectExpire=*/-1);
                retireExpiredLoans(p);
                last = std::max(last, p);
                continue;
            }
        }
        if (block.tail) {
            int p = consume(*block.tail, /*expectExpire=*/-1);
            retireExpiredLoans(p);
            last = std::max(last, p);
        }
        scopes_.pop_back();
        return last;
    }

    int handleRefExpr(const ast::RefExpr& re, int expectExpire) {
        (void)expectExpire;
        // NOTE: consume() already incremented pos_ for the RefExpr node
        // itself before dispatching here. `myPos` is therefore one less
        // than the current pos_.
        int myPos = pos_ - 1;
        auto* id = dynamic_cast<const ast::IdentExpr*>(re.operand.get());
        if (!id) {
            return consume(*re.operand, expectExpire);
        }
        // Match pass 1's recursive prePass(operand): one position for the
        // inner IdentExpr.
        ++pos_;
        Binding* b = lookupBinding(id->name);
        if (!b) return myPos;
        // Rules at the borrow site:
        //   - Cannot borrow a moved value.
        //   - `&x` requires no active &mut on x.
        //   - `&mut x` requires no active &mut or &shared on x.
        if (b->state == OwnState::Moved) {
            error("borrow of moved value `" + id->name + "` (moved at " +
                      std::to_string(b->moveLine) + ":" +
                      std::to_string(b->moveCol) + ")",
                  id->line, id->column);
            return myPos;
        }
        if (re.isMut) {
            if (b->sharedLoans > 0) {
                error("cannot borrow `" + id->name +
                          "` mutably while shared borrows are active",
                      re.line, re.column);
            } else if (b->mutLoanActive) {
                error("cannot borrow `" + id->name +
                          "` mutably more than once at a time",
                      re.line, re.column);
            } else {
                b->mutLoanActive = true;
            }
        } else {
            if (b->mutLoanActive) {
                error("cannot borrow `" + id->name +
                          "` immutably while a mutable borrow is active",
                      re.line, re.column);
            } else {
                ++b->sharedLoans;
            }
        }
        // Add a loan record with no borrower yet (temporary by default).
        // If this RefExpr is the immediate RHS of a `let`, the enclosing
        // consumeBlock will retag the borrower below.
        Loan loan;
        loan.borrowedDeclPos = b->declPos;
        loan.borrowerDeclPos = -1;
        loan.isMut = re.isMut;
        loan.expirePos = myPos; // temp loans expire at end of statement
        loan.line = re.line;
        loan.column = re.column;
        activeLoans_.push_back(loan);
        return myPos;
    }

    // After `let r = <RHS>`, find the most-recent loan added that was
    // synthesized during the RHS walk and retag it with this let's
    // borrower-declPos + expirePos = r.lastUsePos.
    void attachLoanToBorrower(int borrowerDeclPos) {
        if (activeLoans_.empty()) return;
        Loan& back = activeLoans_.back();
        if (back.borrowerDeclPos != -1) return; // already attached
        // Confirm the back-loan came from this let's RHS by checking it
        // was added recently. The simplest invariant: the most recent
        // loan with borrowerDeclPos == -1 belongs to the just-walked RHS.
        back.borrowerDeclPos = borrowerDeclPos;
        auto it = bindings_.find(borrowerDeclPos);
        if (it != bindings_.end()) {
            back.expirePos = std::max(it->second.lastUsePos, borrowerDeclPos);
        }
    }

    // Drop loans whose expirePos <= `at`. `at` is the position of the
    // last node visited in the just-finished statement, so a loan
    // created at position p has its "last live" point at p too — the
    // current statement is the loan's enclosing scope, and statement-
    // end is the natural drop point for temporaries. Named borrows that
    // had their expirePos retagged to the borrower's lastUsePos retire
    // the same way once the borrower has been used for the last time.
    void retireExpiredLoans(int at) {
        auto it = std::remove_if(activeLoans_.begin(), activeLoans_.end(),
                                   [&](const Loan& l) {
                                       if (l.expirePos <= at) {
                                           releaseLoan(l);
                                           return true;
                                       }
                                       return false;
                                   });
        activeLoans_.erase(it, activeLoans_.end());
    }

    void releaseLoan(const Loan& l) {
        auto bIt = bindings_.find(l.borrowedDeclPos);
        if (bIt == bindings_.end()) return;
        Binding& b = bIt->second;
        if (l.isMut) {
            b.mutLoanActive = false;
        } else if (b.sharedLoans > 0) {
            --b.sharedLoans;
        }
    }

    void consumeIdent(const ast::IdentExpr& id) {
        Binding* b = lookupBinding(id.name);
        if (!b) return;
        if (!b->isMoveTyped) return;
        if (b->state == OwnState::Moved) {
            error("use of moved value `" + id.name + "` (moved at " +
                      std::to_string(b->moveLine) + ":" +
                      std::to_string(b->moveCol) + ")",
                  id.line, id.column);
            return;
        }
        // Phase 2.4c: can't move while borrowed.
        if (b->sharedLoans > 0 || b->mutLoanActive) {
            error("cannot move `" + id.name +
                      "` while it is borrowed",
                  id.line, id.column);
            return;
        }
        b->state = OwnState::Moved;
        b->moveLine = id.line;
        b->moveCol = id.column;
    }

    void checkRead(const ast::IdentExpr& id) {
        Binding* b = lookupBinding(id.name);
        if (!b) return;
        if (!b->isMoveTyped) return;
        if (b->state == OwnState::Moved) {
            error("field access on moved value `" + id.name + "` (moved at " +
                      std::to_string(b->moveLine) + ":" +
                      std::to_string(b->moveCol) + ")",
                  id.line, id.column);
        }
    }

    void bindPattern(const ast::Pattern& pat, const TypePtr& expected,
                     bool prePass) {
        if (auto* vp = dynamic_cast<const ast::VarPat*>(&pat)) {
            auto vit = tc_.variantIndex.find(vp->name);
            if (vit == tc_.variantIndex.end()) {
                if (prePass) newBinding(vp->name, expected);
                else {
                    // Pass 2: just bump pos for the binding-decl slot and
                    // rebind the name. The Binding entry was created in
                    // pass 1 at the same position.
                    int dp = pos_++;
                    scopes_.back()[vp->name] = dp;
                }
            }
            return;
        }
        if (auto* cp = dynamic_cast<const ast::CtorPat*>(&pat)) {
            TypePtr re = expected ? resolve(expected) : nullptr;
            std::vector<TypePtr> payloadTypes;
            if (re && re->kind == TypeKind::Enum) {
                for (const auto& v : re->enumVariants) {
                    if (v.name == cp->ctorName) {
                        payloadTypes = v.payloadTypes;
                        break;
                    }
                }
            }
            for (std::size_t i = 0; i < cp->subpatterns.size(); ++i) {
                TypePtr sub = (i < payloadTypes.size())
                                  ? payloadTypes[i]
                                  : TypePtr{};
                bindPattern(*cp->subpatterns[i], sub, prePass);
            }
            return;
        }
    }
};

} // namespace

BorrowCheckResult borrow_check(const ast::Program& program,
                                const TypeCheckResult& tc) {
    BorrowChecker bc(program, tc);
    return bc.run();
}

} // namespace kardashev
