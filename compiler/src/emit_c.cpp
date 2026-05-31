// v23 Phase 129: the C-source backend. See emit_c.hpp for scope + rationale.
// v29 Phase 157: structs — typedefs, struct literals, field access/assignment,
// and struct-typed lets/params/returns. The backend is now TYPE-AWARE (a value
// is `int64_t` for the scalar subset, or `struct <Name>` for a user struct of
// scalar/struct fields) rather than assuming everything is int64_t.
#include "kardashev/emit_c.hpp"

#include <set>
#include <sstream>
#include <unordered_map>

namespace kardashev {
namespace {

using namespace ast;

struct CEmitter {
    std::ostringstream out;
    std::vector<std::string> errors;
    bool hasMain = false;
    // v29 Phase 157: user structs + fns by name (for type-of-expression), and
    // the C type of each in-scope value name (param / let). varCType_ is reset
    // per function.
    std::unordered_map<std::string, const StructDecl*> structs_;
    std::unordered_map<std::string, const FnDecl*> fns_;
    std::unordered_map<std::string, std::string> varCType_;
    // v29 Phase 158: enums lower to a tagged struct `struct E { int64_t tag;
    // int64_t p0..p<maxArity-1>; }`. variantInfo_ maps a (globally-unique)
    // variant name to its enum + tag index + the variant's payload TypeRefs.
    struct VariantInfo {
        std::string enumName;
        int tag = 0;
        const std::vector<TypeRef>* payloadTypes = nullptr;
    };
    std::unordered_map<std::string, VariantInfo> variantInfo_;
    std::unordered_map<std::string, const EnumDecl*> enums_;
    int matchCounter_ = 0; // fresh names for match / loop temporaries
    // v29 Phase 160: the result-variable name of each enclosing `loop` (so a
    // `break <value>` writes the loop's value). Only `loop` (not for/while) can
    // break WITH a value, so for/while pushes nothing.
    std::vector<std::string> loopResultVars_;

    void err(const std::string& m) { errors.push_back("emit-c: " + m); }
    bool ok() const { return errors.empty(); }

    // --- identifier hygiene: don't collide with C keywords / our own names ---
    static bool isReserved(const std::string& n) {
        static const std::set<std::string> kw = {
            "auto", "break", "case", "char", "const", "continue", "default",
            "do", "double", "else", "enum", "extern", "float", "for", "goto",
            "if", "inline", "int", "long", "register", "restrict", "return",
            "short", "signed", "sizeof", "static", "struct", "switch",
            "typedef", "union", "unsigned", "void", "volatile", "while",
            "_Bool", "main", "int64_t", "stdint"};
        return kw.count(n) > 0;
    }
    // A value name (param / let / const). Functions live in the `kd_`-prefixed
    // namespace, so a value literally named `kd_<fn>` would otherwise shadow a
    // function; mangle those (and C keywords) to a disjoint `kdv_` prefix.
    static std::string vname(const std::string& n) {
        if (isReserved(n) || n.rfind("kd_", 0) == 0) return "kdv_" + n;
        return n;
    }

    static bool isScalarName(const std::string& n) {
        return n == "i64" || n == "bool";
    }
    bool isStructName(const std::string& n) const {
        return structs_.count(n) > 0;
    }
    bool isEnumName(const std::string& n) const { return enums_.count(n) > 0; }

    // v29 Phase 157: the C type of a kardashev TypeRef. Scalars -> int64_t, a
    // user struct -> `struct <Name>`. Anything else (refs, generics, enums,
    // strings, fn types, tuples, arrays) is outside this phase's subset; returns
    // "" and records an error so the caller refuses the program.
    std::string ctype(const TypeRef& t) {
        // v29 Phase 159: a reference `&T` / `&mut T` -> a C pointer `<T>*`.
        // (`&` and `&mut` both map to a non-const pointer — const-correctness
        // isn't needed for exit-code-faithful lowering, and it keeps mutation
        // through a `&mut` straightforward.) Slices are NOT plain references.
        if (t.isRef && !t.isSlice) {
            TypeRef inner = t;
            inner.isRef = false;
            inner.refIsMut = false;
            std::string ci = ctype(inner);
            return ci.empty() ? "" : ci + "*";
        }
        if (t.isFn || t.isSlice || t.isArray || t.isTuple || t.isDyn ||
            !t.assocName.empty()) {
            err("type `" + (t.name.empty() ? std::string("<complex>") : t.name) +
                "` (slice/array/tuple/fn/dyn/assoc) is outside the "
                "C-backend subset");
            return "";
        }
        // v29 Phase 159: the unit type `()` (a no-`-> T` return, or an explicit
        // `()`) lowers to int64_t — a unit-returning fn yields the block value 0,
        // which the caller (an ExprStmt) discards. Keeps the uniform
        // `return <block value>;` lowering for a void-like function.
        if (t.name == "unit") return "int64_t";
        if (isScalarName(t.name)) return "int64_t";
        if (isStructName(t.name) || isEnumName(t.name)) {
            if (!t.typeArgs.empty()) {
                err("generic type `" + t.name +
                    "<…>` is outside the C-backend subset");
                return "";
            }
            return "struct " + t.name;
        }
        err("type `" + t.name + "` is outside the C-backend subset "
            "(i64/bool/struct/enum only)");
        return "";
    }

    // --- binary / unary operator spellings (C == kardashev for the subset) ---
    const char* binOp(BinOp op) {
        switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Mod: return "%";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Eq: return "==";
        case BinOp::NotEq: return "!=";
        case BinOp::And: return "&&";
        case BinOp::Or: return "||";
        case BinOp::BitAnd: return "&";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        }
        return nullptr;
    }

    // v29 Phase 157: the C type of an EXPRESSION (needed to type a struct-valued
    // `let` with no annotation, a compound literal, etc.). The AST is already
    // typechecked, so this structural derivation is sound for the subset.
    std::string ctypeOfExpr(const Expr& e) {
        if (auto* sl = dynamic_cast<const StructLitExpr*>(&e))
            return "struct " + sl->structName;
        if (auto* id = dynamic_cast<const IdentExpr*>(&e)) {
            // v29 Phase 158: a bare unit-variant constructor (`None`) is a value
            // of its enum type.
            auto vi = variantInfo_.find(id->name);
            if (vi != variantInfo_.end()) return "struct " + vi->second.enumName;
            auto it = varCType_.find(id->name);
            return it != varCType_.end() ? it->second : "int64_t";
        }
        if (auto* mx = dynamic_cast<const MatchExpr*>(&e))
            return mx->arms.empty() ? "int64_t" : ctypeOfExpr(*mx->arms[0].body);
        // v29 Phase 159: `&x` -> a pointer; `*r` -> the pointee.
        if (auto* re = dynamic_cast<const RefExpr*>(&e))
            return ctypeOfExpr(*re->operand) + "*";
        if (auto* un = dynamic_cast<const UnaryExpr*>(&e)) {
            if (un->op == UnaryOp::Deref) {
                std::string t = ctypeOfExpr(*un->operand);
                return (!t.empty() && t.back() == '*') ? t.substr(0, t.size() - 1)
                                                       : "int64_t";
            }
            return "int64_t";
        }
        if (auto* fe = dynamic_cast<const FieldExpr*>(&e)) {
            std::string objTy = ctypeOfExpr(*fe->object);
            // v29 Phase 159: auto-deref — `r.field` on a `&Struct` reads the
            // pointee's field, so strip one pointer layer before the lookup.
            while (!objTy.empty() && objTy.back() == '*')
                objTy.pop_back();
            if (objTy.rfind("struct ", 0) == 0) {
                const std::string sn = objTy.substr(7);
                auto sit = structs_.find(sn);
                if (sit != structs_.end())
                    for (const auto& f : sit->second->fields)
                        if (f.name == fe->fieldName) return ctype(f.type);
            }
            return "int64_t";
        }
        if (auto* call = dynamic_cast<const CallExpr*>(&e)) {
            // v29 Phase 158: a variant constructor with a payload (`Some(5)`).
            auto vi = variantInfo_.find(call->callee);
            if (vi != variantInfo_.end()) return "struct " + vi->second.enumName;
            auto fit = fns_.find(call->callee);
            if (fit != fns_.end()) return ctype(fit->second->returnType);
            return "int64_t"; // a builtin / scalar call
        }
        if (auto* iff = dynamic_cast<const IfExpr*>(&e))
            return ctypeOfExpr(*iff->thenBranch);
        if (auto* blk = dynamic_cast<const BlockExpr*>(&e))
            return blk->tail ? ctypeOfExpr(*blk->tail) : "int64_t";
        return "int64_t"; // literals, arithmetic, comparisons, unary, while, ...
    }

    // --- expression -> a C expression string ---
    std::string expr(const Expr& e) {
        if (auto* il = dynamic_cast<const IntLitExpr*>(&e))
            return "INT64_C(" + std::to_string(il->value) + ")";
        if (auto* bl = dynamic_cast<const BoolLitExpr*>(&e))
            return bl->value ? "INT64_C(1)" : "INT64_C(0)";
        if (auto* id = dynamic_cast<const IdentExpr*>(&e)) {
            // v29 Phase 158: a bare unit-variant constructor (`None`).
            auto vi = variantInfo_.find(id->name);
            if (vi != variantInfo_.end())
                return "((struct " + vi->second.enumName + "){ .tag = " +
                       std::to_string(vi->second.tag) + " })";
            return vname(id->name);
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(&e)) {
            const char* op = nullptr;
            switch (un->op) {
            case UnaryOp::Neg: op = "-"; break;
            case UnaryOp::Not: op = "!"; break;
            case UnaryOp::BitNot: op = "~"; break;
            case UnaryOp::Deref:
                // v29 Phase 159: `*r` dereferences a C pointer.
                return "(*(" + expr(*un->operand) + "))";
            }
            return std::string("(") + op + expr(*un->operand) + ")";
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(&e)) {
            const char* op = binOp(bin->op);
            if (!op) { err("unknown binary operator"); return "0"; }
            return "(" + expr(*bin->lhs) + " " + op + " " + expr(*bin->rhs) +
                   ")";
        }
        // v29 Phase 157: a struct literal -> a C compound literal with
        // designated initializers (so source field order is irrelevant).
        if (auto* slit = dynamic_cast<const StructLitExpr*>(&e)) {
            std::string s = "((struct " + slit->structName + "){ ";
            for (std::size_t i = 0; i < slit->fields.size(); ++i) {
                if (i) s += ", ";
                s += "." + slit->fields[i].first + " = " +
                     expr(*slit->fields[i].second);
            }
            return s + " })";
        }
        // v29 Phase 157/159: a field access `obj.field`, auto-dereferencing if
        // `obj` is a reference (`r.field` on a `&Struct` -> `(*r).field`).
        if (auto* fe = dynamic_cast<const FieldExpr*>(&e)) {
            std::string objTy = ctypeOfExpr(*fe->object);
            std::string base = expr(*fe->object);
            if (!objTy.empty() && objTy.back() == '*')
                return "((*(" + base + "))." + fe->fieldName + ")";
            return "((" + base + ")." + fe->fieldName + ")";
        }
        // v29 Phase 159: `&x` / `&mut x` -> a C address-of. For `&<temporary>`
        // (a struct/enum literal) this is `&(compound literal)`, a pointer to a
        // C99 block-scoped temporary — matching kardashev's statement-scoped
        // ref-to-rvalue.
        if (auto* re = dynamic_cast<const RefExpr*>(&e))
            return "(&(" + expr(*re->operand) + "))";
        if (auto* call = dynamic_cast<const CallExpr*>(&e)) {
            // v29 Phase 158: a variant constructor with a payload (`Some(5)`) —
            // a compound literal `((struct E){ .tag = idx, .p0 = a, ... })`.
            auto vi = variantInfo_.find(call->callee);
            if (vi != variantInfo_.end()) {
                std::string s = "((struct " + vi->second.enumName +
                                "){ .tag = " + std::to_string(vi->second.tag);
                for (std::size_t i = 0; i < call->args.size(); ++i)
                    s += ", .p" + std::to_string(i) + " = " +
                         expr(*call->args[i]);
                return s + " })";
            }
            std::string s = "kd_" + call->callee + "(";
            for (std::size_t i = 0; i < call->args.size(); ++i) {
                if (i) s += ", ";
                s += expr(*call->args[i]);
            }
            return s + ")";
        }
        // v29 Phase 158: a `match` -> an if/else chain on the tag (enum) or value
        // (int), each arm binding its payloads, in a statement-expression.
        if (auto* mx = dynamic_cast<const MatchExpr*>(&e))
            return matchExpr(*mx);
        if (auto* iff = dynamic_cast<const IfExpr*>(&e)) {
            return "(" + expr(*iff->cond) + " ? " + expr(*iff->thenBranch) +
                   " : " + expr(*iff->elseBranch) + ")";
        }
        if (auto* blk = dynamic_cast<const BlockExpr*>(&e))
            return blockValue(*blk);
        if (auto* wh = dynamic_cast<const WhileExpr*>(&e)) {
            auto* body = dynamic_cast<const BlockExpr*>(wh->body.get());
            if (!body) { err("while body is not a block"); return "0"; }
            // unit-valued: a statement-expression that runs the loop then 0.
            return "({ while (" + expr(*wh->cond) + ") " + blockStmts(*body) +
                   " INT64_C(0); })";
        }
        // v29 Phase 160: `for x in a..b { body }` (the range fast path) -> a C
        // `for`. The loop is unit-valued (yields 0). A general-iterator `for`
        // (over an `Iterator` impl) uses Option/methods — outside the subset.
        if (auto* fe = dynamic_cast<const ForExpr*>(&e)) {
            if (fe->iteratorDesugar) {
                err("`for` over a general iterator is outside the C-backend "
                    "subset (range loops only)");
                return "0";
            }
            auto* range = dynamic_cast<const RangeExpr*>(fe->iter.get());
            auto* vp = dynamic_cast<const VarPat*>(fe->pattern.get());
            auto* body = dynamic_cast<const BlockExpr*>(fe->body.get());
            if (!range || !vp || !body) {
                err("`for` over a non-range / complex pattern is outside the "
                    "C-backend subset");
                return "0";
            }
            const std::string v = vname(vp->name);
            varCType_[vp->name] = "int64_t";
            const char* cmp = range->inclusive ? " <= " : " < ";
            return "({ for (int64_t " + v + " = (" + expr(*range->start) +
                   "); " + v + cmp + "(" + expr(*range->end) + "); " + v +
                   "++) " + blockStmts(*body) + " INT64_C(0); })";
        }
        // v29 Phase 160: `loop { ... }` — a `while (1)` whose value is whatever a
        // `break <value>` writes (int64_t result; a struct loop value is rare
        // and deferred). A breakless `loop` would be infinite (the type checker
        // keeps it unit / diverging).
        if (auto* lp = dynamic_cast<const LoopExpr*>(&e)) {
            auto* body = dynamic_cast<const BlockExpr*>(lp->body.get());
            if (!body) { err("loop body is not a block"); return "0"; }
            const std::string lv = "__lv" + std::to_string(matchCounter_++);
            loopResultVars_.push_back(lv);
            std::string bodyStr = blockStmts(*body);
            loopResultVars_.pop_back();
            return "({ int64_t " + lv + " = 0; while (1) " + bodyStr + " " + lv +
                   "; })";
        }
        if (auto* br = dynamic_cast<const BreakExpr*>(&e)) {
            if (br->value) {
                if (loopResultVars_.empty()) {
                    err("`break <value>` outside a `loop`");
                    return "0";
                }
                return "({ " + loopResultVars_.back() + " = (" +
                       expr(*br->value) + "); break; INT64_C(0); })";
            }
            return "({ break; INT64_C(0); })";
        }
        if (dynamic_cast<const ContinueExpr*>(&e))
            return "({ continue; INT64_C(0); })";
        err("unsupported expression in the C backend (outside the subset)");
        return "0";
    }

    // v29 Phase 158: lower a `match` to an if/else chain in a statement-
    // expression. An ENUM scrutinee tests `.tag`; an INT scrutinee tests the
    // value. A CtorPat binds its scalar payloads from `.p<i>`. Patterns outside
    // the subset (nested ctor args, char/or/tuple/slice, guards) are refused.
    std::string matchExpr(const MatchExpr& mx) {
        std::string scrutCTy = ctypeOfExpr(*mx.scrutinee);
        // v29 Phase 159: match-through-reference (`match o` with `o: &Enum`) —
        // copy the pointee into a value and match it (sound for scalar payloads:
        // a payload borrow `&i64` reads the same value as a copied i64).
        bool refScrut = !scrutCTy.empty() && scrutCTy.back() == '*';
        std::string valCTy =
            refScrut ? scrutCTy.substr(0, scrutCTy.size() - 1) : scrutCTy;
        std::string scrutE = refScrut ? ("(*(" + expr(*mx.scrutinee) + "))")
                                      : expr(*mx.scrutinee);
        std::string resCTy =
            mx.arms.empty() ? "int64_t" : ctypeOfExpr(*mx.arms[0].body);
        int id = matchCounter_++;
        const std::string m = "__m" + std::to_string(id);
        const std::string r = "__r" + std::to_string(id);
        std::string s = "({ " + valCTy + " " + m + " = (" + scrutE + "); " +
                        resCTy + " " + r + "; ";
        bool first = true, hasCatchAll = false;
        for (const auto& arm : mx.arms) {
            std::string test, binds;
            const Pattern& p = *arm.pattern;
            if (auto* cp = dynamic_cast<const CtorPat*>(&p)) {
                auto vi = variantInfo_.find(cp->ctorName);
                if (vi == variantInfo_.end()) {
                    err("unknown variant `" + cp->ctorName + "` in match");
                    return "0";
                }
                test = m + ".tag == " + std::to_string(vi->second.tag);
                for (std::size_t i = 0; i < cp->subpatterns.size(); ++i) {
                    if (auto* vp = dynamic_cast<const VarPat*>(
                            cp->subpatterns[i].get())) {
                        std::string pty =
                            (vi->second.payloadTypes &&
                             i < vi->second.payloadTypes->size())
                                ? ctype((*vi->second.payloadTypes)[i])
                                : "int64_t";
                        if (pty.empty()) return "0";
                        binds += pty + " " + vname(vp->name) + " = " + m + ".p" +
                                 std::to_string(i) + "; ";
                    } else if (!dynamic_cast<const WildPat*>(
                                   cp->subpatterns[i].get())) {
                        err("a nested pattern in a match arm is outside the "
                            "C-backend subset");
                        return "0";
                    }
                }
            } else if (auto* vp = dynamic_cast<const VarPat*>(&p)) {
                auto vi = variantInfo_.find(vp->name);
                if (vi != variantInfo_.end()) {
                    test = m + ".tag == " + std::to_string(vi->second.tag);
                } else {
                    test = "1";
                    hasCatchAll = true;
                    binds = scrutCTy + " " + vname(vp->name) + " = " + m + "; ";
                }
            } else if (dynamic_cast<const WildPat*>(&p)) {
                test = "1";
                hasCatchAll = true;
            } else if (auto* lp = dynamic_cast<const LitIntPat*>(&p)) {
                test = "(" + m + " == INT64_C(" + std::to_string(lp->value) +
                       "))";
            } else {
                err("a match pattern (char/or/tuple/slice/guard) is outside "
                    "the C-backend subset");
                return "0";
            }
            s += (first ? "if (" : "else if (") + test + ") { " + binds + r +
                 " = " + expr(*arm.body) + "; } ";
            first = false;
        }
        // The type checker guarantees exhaustiveness; a defensive zero default
        // keeps the C compiler from warning about a possibly-unset result.
        if (!hasCatchAll) s += "else { " + r + " = (" + resCTy + "){0}; } ";
        s += r + "; })";
        return s;
    }

    // A block used as a VALUE: `({ stmts...; tail; })`. A unit block (no tail)
    // yields 0 so it still type-checks as int64_t in expression position.
    std::string blockValue(const BlockExpr& b) {
        std::string s = "({ ";
        for (const auto& st : b.stmts) s += stmt(*st) + " ";
        s += b.tail ? (expr(*b.tail) + ";") : "INT64_C(0);";
        s += " })";
        return s;
    }

    // A block used for EFFECT (a while body): `{ stmts...; tail-for-effect; }`.
    std::string blockStmts(const BlockExpr& b) {
        std::string s = "{ ";
        for (const auto& st : b.stmts) s += stmt(*st) + " ";
        if (b.tail) s += expr(*b.tail) + ";";
        return s + " }";
    }

    // --- statement -> a C statement string ---
    std::string stmt(const Stmt& s) {
        if (auto* let = dynamic_cast<const LetStmt*>(&s)) {
            if (!let->tupleNames.empty()) {
                err("tuple-destructuring `let` is outside the C-backend subset");
                return ";";
            }
            // v29 Phase 157: the binding's C type — from the annotation if
            // present, else derived from the value (a struct literal / call).
            std::string cty = let->annotation ? ctype(*let->annotation)
                                              : ctypeOfExpr(*let->value);
            if (cty.empty()) return ";"; // ctype() already recorded the error
            varCType_[let->name] = cty;
            return cty + " " + vname(let->name) + " = " + expr(*let->value) +
                   ";";
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(&s)) {
            return ret->value ? ("return " + expr(*ret->value) + ";")
                              : "return INT64_C(0);";
        }
        if (auto* es = dynamic_cast<const ExprStmt*>(&s))
            return expr(*es->expr) + ";";
        if (auto* as = dynamic_cast<const AssignStmt*>(&s)) {
            // v29 Phase 157: assignment to a variable OR a field place
            // (`p.x = …`). Index/deref places stay outside the subset.
            if (dynamic_cast<const IdentExpr*>(as->target.get()) ||
                dynamic_cast<const FieldExpr*>(as->target.get()))
                return expr(*as->target) + " = " + expr(*as->value) + ";";
            err("assignment to a non-variable/field place is outside the "
                "C-backend subset");
            return ";";
        }
        err("unsupported statement in the C backend");
        return ";";
    }

    // --- a function signature `<ret> kd_<name>(<params>)` ---
    std::string signature(const FnDecl& fn) {
        std::string ret = ctype(fn.returnType);
        if (ret.empty()) return "";
        std::string s = ret + " kd_" + fn.name + "(";
        if (fn.params.empty()) {
            s += "void";
        } else {
            for (std::size_t i = 0; i < fn.params.size(); ++i) {
                if (i) s += ", ";
                std::string pty = ctype(fn.params[i].type);
                if (pty.empty()) return "";
                s += pty + " " + vname(fn.params[i].name);
            }
        }
        return s + ")";
    }

    bool fnInSubset(const FnDecl& fn) {
        if (!fn.genericParams.empty()) {
            err("generic fn `" + fn.name + "` is outside the C-backend subset");
            return false;
        }
        if (fn.isAsync) {
            err("async fn `" + fn.name + "` is outside the C-backend subset");
            return false;
        }
        // ctype() of the return + each param validates they're in the subset.
        if (ctype(fn.returnType).empty()) return false;
        for (const auto& p : fn.params)
            if (ctype(p.type).empty()) return false;
        return true;
    }

    // v29 Phase 157: emit struct typedefs, inner-before-outer so a struct field
    // of struct type is complete at the point of use. A simple iterate-until-
    // fixpoint topological emission (the subset has no recursive structs — a
    // by-value cycle is infinite-sized and the type checker rejects it).
    void emitStructDefs(const Program& program) {
        std::set<std::string> emitted;
        bool progress = true;
        while (progress && emitted.size() < program.structs.size()) {
            progress = false;
            for (const auto& s : program.structs) {
                if (emitted.count(s.name)) continue;
                // All struct-typed field dependencies must be emitted first.
                bool ready = true;
                for (const auto& f : s.fields)
                    if (isStructName(f.type.name) && !emitted.count(f.type.name))
                        ready = false;
                if (!ready) continue;
                out << "struct " << s.name << " {\n";
                for (const auto& f : s.fields) {
                    std::string fty = ctype(f.type);
                    if (fty.empty()) return; // out-of-subset field type
                    out << "  " << fty << " " << f.name << ";\n";
                }
                out << "};\n";
                emitted.insert(s.name);
                progress = true;
            }
        }
        if (emitted.size() != program.structs.size())
            err("a struct field cycle / out-of-subset field blocks the C "
                "backend");
        if (!program.structs.empty()) out << "\n";
    }

    // v29 Phase 158: emit a tagged-struct typedef per (non-generic, scalar-
    // payload) enum: `struct E { int64_t tag; int64_t p0..p<maxArity-1>; }`.
    void emitEnumDefs(const Program& program) {
        for (const auto& en : program.enums) {
            if (!en.genericParams.empty()) {
                err("generic enum `" + en.name +
                    "` is outside the C-backend subset");
                return;
            }
            std::size_t maxArity = 0;
            for (const auto& v : en.variants) {
                if (v.payloadTypes.size() > maxArity)
                    maxArity = v.payloadTypes.size();
                for (const auto& pt : v.payloadTypes)
                    if (ctype(pt) != "int64_t") {
                        err("enum `" + en.name + "` variant `" + v.name +
                            "` has a non-scalar payload — outside the "
                            "C-backend subset (i64/bool payloads only)");
                        return;
                    }
            }
            out << "struct " << en.name << " {\n  int64_t tag;\n";
            for (std::size_t i = 0; i < maxArity; ++i)
                out << "  int64_t p" << i << ";\n";
            out << "};\n";
        }
        if (!program.enums.empty()) out << "\n";
    }

    void emitProgram(const Program& program) {
        // v29 Phase 157/158: structs + enums are in the subset; traits/impls/
        // extern/mod remain refused (later phases).
        if (!program.traits.empty() || !program.impls.empty() ||
            !program.externFns.empty() || !program.mods.empty()) {
            err("the program uses traits/impls/extern/mod — outside the "
                "C-backend subset (i64/bool/struct/enum functions + const)");
            return;
        }
        for (const auto& s : program.structs) structs_[s.name] = &s;
        for (const auto& fn : program.functions) fns_[fn.name] = &fn;
        for (const auto& en : program.enums) {
            enums_[en.name] = &en;
            for (int i = 0; i < static_cast<int>(en.variants.size()); ++i)
                variantInfo_[en.variants[i].name] = {
                    en.name, i, &en.variants[i].payloadTypes};
        }

        out << "/* Generated by kardashev --emit-c (v29 Phase 158). */\n";
        out << "#include <stdint.h>\n\n";

        emitStructDefs(program);
        if (!ok()) return;
        emitEnumDefs(program);
        if (!ok()) return;

        // Top-level consts (a const may reference an earlier one, so keep order).
        for (const auto& c : program.consts) {
            std::string cty = ctype(c.type);
            if (cty.empty()) continue; // error already recorded
            out << "static const " << cty << " " << vname(c.name) << " = "
                << expr(*c.value) << ";\n";
        }
        if (!program.consts.empty()) out << "\n";

        // Forward prototypes first — kardashev allows calling a fn defined later
        // (forward references + mutual recursion).
        for (const auto& fn : program.functions) {
            if (!fnInSubset(fn)) return;
            std::string sig = signature(fn);
            if (sig.empty()) return;
            out << sig << ";\n";
            if (fn.name == "main") hasMain = true;
        }
        out << "\n";

        // Definitions.
        for (const auto& fn : program.functions) {
            if (!fn.body) {
                err("fn `" + fn.name + "` has no body");
                return;
            }
            varCType_.clear();
            for (const auto& p : fn.params)
                varCType_[p.name] = ctype(p.type);
            std::string sig = signature(fn);
            if (sig.empty()) return;
            out << sig << " {\n  return " << blockValue(*fn.body) << ";\n}\n\n";
            if (!ok()) return;
        }

        // The C entry point: kardashev `main` (no params in the subset) becomes
        // the process exit code, matching the LLVM backend's `ret i64` from main
        // truncated to the low byte.
        if (hasMain) {
            out << "int main(void) { return (int)kd_main(); }\n";
        }
    }
};

} // namespace

EmitCResult emit_c(const ast::Program& program) {
    CEmitter em;
    em.emitProgram(program);
    EmitCResult r;
    if (!em.errors.empty()) {
        r.success = false;
        r.errors = std::move(em.errors);
        return r;
    }
    r.success = true;
    r.code = em.out.str();
    return r;
}

} // namespace kardashev
