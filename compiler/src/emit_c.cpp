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
    // v30 Phase 162: the set of String builtins this program calls (so the C
    // runtime emits only what's used). A call to one of these (or a String type
    // / string literal) opts the program into the String runtime.
    bool usesString_ = false;
    bool programCallsStringBuiltin_ = false; // set by the pre-scan
    // v30 Phase 162: escape a kardashev string's bytes into a C string literal.
    static std::string cStringLit(const std::string& s) {
        std::string out = "\"";
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
                else {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                }
            }
        }
        return out + "\"";
    }
    static bool isStringBuiltin(const std::string& n) {
        static const std::set<std::string> b = {
            "string_new", "str_len", "str_char_at", "str_push_byte",
            "string_push_str", "print_str", "print_string", "print_no_nl",
            "println", "int_to_string", "str_eq", "str_substring", "print"};
        return b.count(n) > 0;
    }
    // v30 Phase 163: scalar-element Vec builtins with a C runtime (int64_t
    // elements: Vec<i64> / Vec<bool>). Struct/String-element Vecs are refused
    // for now (a later piece monomorphizes per element type).
    bool usesVec_ = false;
    bool sawVecCall_ = false; // set by the pre-scan (exprUsesString walk)
    static bool isVecBuiltin(const std::string& n) {
        static const std::set<std::string> b = {
            "vec_new", "vec_push", "vec_get", "vec_get_ref", "vec_len",
            "vec_pop", "vec_remove", "vec_insert", "vec_reverse", "vec_swap"};
        return b.count(n) > 0;
    }
    // v30 Phase 163: the builtins for which the C backend emits a real runtime.
    // Currently the String + scalar-Vec sets; HashMap/HashSet etc. are added as
    // their C runtimes land. A call to anything not here (and not a user fn) is
    // refused (Phase 163 part 1).
    static bool isImplementedBuiltin(const std::string& n) {
        return isStringBuiltin(n) || isVecBuiltin(n);
    }

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
        // v30 Phase 162: String -> the C runtime's `struct kdstr`.
        if (t.name == "String") { usesString_ = true; return "struct kdstr"; }
        // v30 Phase 163: Vec<T> -> the C runtime's `struct kdvec`, but only for a
        // SCALAR element (i64/bool, both -> int64_t in C). Vec<struct>/Vec<String>
        // need a per-element-type monomorphized runtime (a later piece) — refuse
        // them cleanly rather than emit a wrong int64_t-element vec.
        if (t.name == "Vec") {
            if (t.typeArgs.size() == 1 && isScalarName(t.typeArgs[0].name)) {
                usesVec_ = true;
                return "struct kdvec";
            }
            err("Vec with a non-scalar element is outside the C-backend subset "
                "(Vec<i64>/Vec<bool> only for now)");
            return "";
        }
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
        // v30 Phase 162: a string literal + the String-returning builtins.
        if (dynamic_cast<const StringLitExpr*>(&e)) {
            usesString_ = true;
            return "struct kdstr";
        }
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
            // v30 Phase 162: String-returning builtins.
            if (call->callee == "string_new" ||
                call->callee == "int_to_string" ||
                call->callee == "str_substring") {
                usesString_ = true;
                return "struct kdstr";
            }
            // v30 Phase 163: Vec-returning / Vec-element builtins.
            if (call->callee == "vec_new") { usesVec_ = true; return "struct kdvec"; }
            if (call->callee == "vec_get_ref") return "int64_t*";
            // vec_get/pop/remove/len/push/insert/reverse/swap -> int64_t
            return "int64_t"; // a scalar builtin call
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
        // v30 Phase 162: a string literal -> a borrowed kdstr { data, len,
        // cap=0 } over a C string literal (cap==0 = read-only, never freed,
        // copy-on-write on mutation — same convention as the LLVM backend).
        if (auto* sl = dynamic_cast<const StringLitExpr*>(&e)) {
            usesString_ = true;
            return "((struct kdstr){ .data = (char*)" + cStringLit(sl->value) +
                   ", .len = INT64_C(" + std::to_string(sl->value.size()) +
                   "), .cap = INT64_C(0) })";
        }
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
            // v30 Phase 163 (soundness): a call must resolve to a user fn, a
            // variant ctor (handled above), or an IMPLEMENTED builtin. Anything
            // else (an unimplemented stdlib builtin — vec_*, hashmap_*, thread_*,
            // async, …) would emit `kd_<name>(...)` with no definition = broken
            // C. Refuse it instead, preserving "never emit wrong C". (Earlier
            // phases silently emitted undefined-symbol calls for these; the
            // phase129 smoke never exercised a builtin so it went unnoticed.)
            if (!fns_.count(call->callee) &&
                !isImplementedBuiltin(call->callee)) {
                err("call to `" + call->callee +
                    "` is outside the C-backend subset (no C runtime for this "
                    "builtin yet)");
                return "0";
            }
            // v30 Phase 162: a String builtin opts the program into the C
            // String runtime (emitted up front as `kd_<name>` definitions).
            if (isStringBuiltin(call->callee)) usesString_ = true;
            // v30 Phase 163: a Vec builtin opts into the scalar-Vec runtime. A
            // vec_push/insert whose pushed element isn't a scalar (int64_t)
            // means a Vec<struct>/Vec<String> — refuse (no monomorphized runtime
            // yet) rather than store a struct into an int64_t slot.
            if (isVecBuiltin(call->callee)) {
                usesVec_ = true;
                if ((call->callee == "vec_push" && call->args.size() == 2) ||
                    (call->callee == "vec_insert" && call->args.size() == 3)) {
                    const Expr& elem = (call->callee == "vec_push")
                                           ? *call->args[1]
                                           : *call->args[2];
                    if (ctypeOfExpr(elem) != "int64_t") {
                        err("Vec with a non-scalar element is outside the "
                            "C-backend subset (Vec<i64>/Vec<bool> only for now)");
                        return "0";
                    }
                }
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

    // v30 Phase 164 (Drop / RAII): count occurrences of the identifier `name`
    // that are NOT a borrow (`&name` / `&mut name`). A borrow keeps ownership in
    // this scope; a BARE occurrence (returned as a value, moved into a call,
    // reassigned, used as a field/index base that could move it out) means the
    // binding may escape, so it is NOT safe to drop at scope exit. Conservative:
    // any bare use ⇒ don't drop (a leak is sound; a wrong drop double-frees).
    int bareUses(const Expr* e, const std::string& name) {
        if (!e) return 0;
        if (auto* re = dynamic_cast<const RefExpr*>(e)) {
            // `&name` / `&mut name` is a borrow, not a bare use. A `&(other
            // expr)` still recurses to find bare uses inside it.
            if (dynamic_cast<const IdentExpr*>(re->operand.get())) return 0;
            return bareUses(re->operand.get(), name);
        }
        if (auto* id = dynamic_cast<const IdentExpr*>(e))
            return id->name == name ? 1 : 0;
        int n = 0;
        if (auto* u = dynamic_cast<const UnaryExpr*>(e)) n += bareUses(u->operand.get(), name);
        else if (auto* b = dynamic_cast<const BinaryExpr*>(e)) { n += bareUses(b->lhs.get(), name); n += bareUses(b->rhs.get(), name); }
        else if (auto* f = dynamic_cast<const FieldExpr*>(e)) n += bareUses(f->object.get(), name);
        else if (auto* iff = dynamic_cast<const IfExpr*>(e)) { n += bareUses(iff->cond.get(), name); n += bareUses(iff->thenBranch.get(), name); n += bareUses(iff->elseBranch.get(), name); }
        else if (auto* w = dynamic_cast<const WhileExpr*>(e)) { n += bareUses(w->cond.get(), name); n += bareUses(w->body.get(), name); }
        else if (auto* lp = dynamic_cast<const LoopExpr*>(e)) n += bareUses(lp->body.get(), name);
        else if (auto* fo = dynamic_cast<const ForExpr*>(e)) { n += bareUses(fo->iter.get(), name); n += bareUses(fo->body.get(), name); }
        else if (auto* c = dynamic_cast<const CallExpr*>(e)) { for (auto& a : c->args) n += bareUses(a.get(), name); }
        else if (auto* mx = dynamic_cast<const MatchExpr*>(e)) { n += bareUses(mx->scrutinee.get(), name); for (auto& arm : mx->arms) n += bareUses(arm.body.get(), name); }
        else if (auto* sl = dynamic_cast<const StructLitExpr*>(e)) { for (auto& fld : sl->fields) n += bareUses(fld.second.get(), name); }
        else if (auto* br = dynamic_cast<const BreakExpr*>(e)) n += bareUses(br->value.get(), name);
        else if (auto* blk = dynamic_cast<const BlockExpr*>(e)) {
            for (auto& st : blk->stmts) {
                if (auto* l = dynamic_cast<const LetStmt*>(st.get())) n += bareUses(l->value.get(), name);
                else if (auto* es = dynamic_cast<const ExprStmt*>(st.get())) n += bareUses(es->expr.get(), name);
                else if (auto* as = dynamic_cast<const AssignStmt*>(st.get())) { n += bareUses(as->target.get(), name); n += bareUses(as->value.get(), name); }
                else if (auto* rs = dynamic_cast<const ReturnStmt*>(st.get())) n += bareUses(rs->value.get(), name);
            }
            n += bareUses(blk->tail.get(), name);
        }
        return n;
    }

    // v30 Phase 164: does any statement (at any depth) early-`return`? If so we
    // disable scope-exit drops for the whole fn (a drop placed only at the tail
    // would be skipped on the early-return path — a leak; never a double-free).
    bool hasEarlyReturn(const Expr* e) {
        if (!e) return false;
        if (auto* iff = dynamic_cast<const IfExpr*>(e))
            return hasEarlyReturn(iff->cond.get()) || hasEarlyReturn(iff->thenBranch.get()) || hasEarlyReturn(iff->elseBranch.get());
        if (auto* w = dynamic_cast<const WhileExpr*>(e)) return hasEarlyReturn(w->cond.get()) || hasEarlyReturn(w->body.get());
        if (auto* lp = dynamic_cast<const LoopExpr*>(e)) return hasEarlyReturn(lp->body.get());
        if (auto* fo = dynamic_cast<const ForExpr*>(e)) return hasEarlyReturn(fo->iter.get()) || hasEarlyReturn(fo->body.get());
        if (auto* mx = dynamic_cast<const MatchExpr*>(e)) { if (hasEarlyReturn(mx->scrutinee.get())) return true; for (auto& arm : mx->arms) if (hasEarlyReturn(arm.body.get())) return true; return false; }
        if (auto* blk = dynamic_cast<const BlockExpr*>(e)) {
            for (auto& st : blk->stmts) {
                if (dynamic_cast<const ReturnStmt*>(st.get())) return true;
                if (auto* es = dynamic_cast<const ExprStmt*>(st.get())) if (hasEarlyReturn(es->expr.get())) return true;
                if (auto* l = dynamic_cast<const LetStmt*>(st.get())) if (hasEarlyReturn(l->value.get())) return true;
            }
            return hasEarlyReturn(blk->tail.get());
        }
        return false;
    }

    // v30 Phase 164: emit a FUNCTION body that drops its non-escaping heap-owning
    // top-level locals (String -> kd__str_drop, Vec -> kd__vec_drop) at exit.
    // The tail is captured to `__ret` FIRST (so a tail that borrows a local is
    // computed before the drop), then drops run, then `__ret` is yielded. Only
    // top-level lets are considered, only when the binding has zero BARE uses
    // (all uses are borrows) and the body has no early `return` — otherwise the
    // local is left alone (a sound leak). retCTy types the captured result.
    std::string fnBodyWithDrops(const FnDecl& fn, const std::string& retCTy) {
        const BlockExpr& b = *fn.body;
        // total bare (non-borrow) uses of `name` across the whole body.
        auto totalBare = [&](const std::string& name, const Stmt* skip) -> int {
            int bare = 0;
            for (const auto& st2 : b.stmts) {
                if (st2.get() == skip) continue; // skip a let's own decl
                if (auto* l2 = dynamic_cast<const LetStmt*>(st2.get())) bare += bareUses(l2->value.get(), name);
                else if (auto* es = dynamic_cast<const ExprStmt*>(st2.get())) bare += bareUses(es->expr.get(), name);
                else if (auto* as = dynamic_cast<const AssignStmt*>(st2.get())) { bare += bareUses(as->target.get(), name); bare += bareUses(as->value.get(), name); }
                else if (auto* rs = dynamic_cast<const ReturnStmt*>(st2.get())) bare += bareUses(rs->value.get(), name);
            }
            bare += bareUses(b.tail.get(), name);
            return bare;
        };
        const char* helperFor = nullptr; // (set per check below)
        auto heapHelper = [](const std::string& cty) -> const char* {
            if (cty == "struct kdstr") return "kd__str_drop";
            if (cty == "struct kdvec") return "kd__vec_drop";
            return nullptr;
        };
        (void)helperFor;
        // Collect droppable bindings: a by-value heap PARAM (the callee owns the
        // moved-in value), then top-level heap LOCALS — both only when every use
        // is a borrow and the fn has no early return. Params drop AFTER locals
        // (reverse declaration order overall is fine — independent buffers).
        std::vector<std::pair<std::string, std::string>> drops; // (cname, helper)
        bool earlyRet = hasEarlyReturn(&b);
        if (!earlyRet) {
            for (const auto& st : b.stmts) {
                auto* let = dynamic_cast<const LetStmt*>(st.get());
                if (!let || !let->tupleNames.empty()) continue;
                std::string cty = let->annotation ? ctype(*let->annotation)
                                                  : ctypeOfExpr(*let->value);
                const char* helper = heapHelper(cty);
                if (helper && totalBare(let->name, st.get()) == 0)
                    drops.emplace_back(vname(let->name), helper);
            }
            // v30 Phase 164 (fix): a by-value heap PARAM is owned by this fn (the
            // caller moved it in), so drop it at exit unless it escapes (bare use
            // / moved on). A `&T` / `&mut T` param is a borrow (its C type ends
            // in `*`) — never dropped.
            for (const auto& p : fn.params) {
                std::string pty = ctype(p.type);
                const char* helper = heapHelper(pty);
                if (helper && totalBare(p.name, nullptr) == 0)
                    drops.emplace_back(vname(p.name), helper);
            }
        }
        if (drops.empty()) return blockValue(b); // nothing to drop: as before
        std::string s = "({ ";
        for (const auto& st : b.stmts) s += stmt(*st) + " ";
        s += retCTy + " __ret = (" +
             (b.tail ? expr(*b.tail) : std::string("INT64_C(0)")) + "); ";
        for (const auto& [cn, helper] : drops) s += helper + ("(&" + cn + "); ");
        s += "__ret; })";
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

    // v30 Phase 162: does any signature mention String? (A pre-scan so the C
    // String runtime is emitted before the fns that use it. A String-builtin
    // call inside a body also opts in, but a program that calls a string builtin
    // necessarily has a String value somewhere a signature/let exposes; to be
    // safe we also scan const/fn return+param types here.)
    // v30 Phase 162: recursively scan an expression for a String-builtin call or
    // a string literal (so the runtime is emitted up front, before the bodies).
    bool exprUsesString(const Expr* e) {
        if (!e) return false;
        if (dynamic_cast<const StringLitExpr*>(e)) return true;
        if (auto* c = dynamic_cast<const CallExpr*>(e)) {
            // v30 Phase 163: piggyback Vec detection on this whole-tree walk;
            // always recurse args (for the sawVecCall_ side-effect) before
            // returning the String result.
            if (isVecBuiltin(c->callee)) sawVecCall_ = true;
            bool found = isStringBuiltin(c->callee);
            for (auto& a : c->args) found |= exprUsesString(a.get());
            return found;
        }
        if (auto* b = dynamic_cast<const BinaryExpr*>(e))
            return exprUsesString(b->lhs.get()) || exprUsesString(b->rhs.get());
        if (auto* u = dynamic_cast<const UnaryExpr*>(e))
            return exprUsesString(u->operand.get());
        if (auto* r = dynamic_cast<const RefExpr*>(e))
            return exprUsesString(r->operand.get());
        if (auto* f = dynamic_cast<const FieldExpr*>(e))
            return exprUsesString(f->object.get());
        if (auto* iff = dynamic_cast<const IfExpr*>(e))
            return exprUsesString(iff->cond.get()) ||
                   exprUsesString(iff->thenBranch.get()) ||
                   exprUsesString(iff->elseBranch.get());
        if (auto* w = dynamic_cast<const WhileExpr*>(e))
            return exprUsesString(w->cond.get()) ||
                   exprUsesString(w->body.get());
        if (auto* lp = dynamic_cast<const LoopExpr*>(e))
            return exprUsesString(lp->body.get());
        if (auto* fo = dynamic_cast<const ForExpr*>(e))
            return exprUsesString(fo->iter.get()) ||
                   exprUsesString(fo->body.get());
        if (auto* mx = dynamic_cast<const MatchExpr*>(e)) {
            if (exprUsesString(mx->scrutinee.get())) return true;
            for (auto& arm : mx->arms)
                if (exprUsesString(arm.body.get())) return true;
            return false;
        }
        if (auto* sl = dynamic_cast<const StructLitExpr*>(e)) {
            for (auto& f : sl->fields)
                if (exprUsesString(f.second.get())) return true;
            return false;
        }
        if (auto* blk = dynamic_cast<const BlockExpr*>(e)) {
            // v30 Phase 163: visit ALL statements (don't short-circuit) so the
            // sawVecCall_ side-effect is set even when an earlier statement
            // already used a String; accumulate the String result.
            bool found = false;
            for (auto& st : blk->stmts) {
                if (auto* l = dynamic_cast<const LetStmt*>(st.get()))
                    found |= exprUsesString(l->value.get());
                if (auto* es = dynamic_cast<const ExprStmt*>(st.get()))
                    found |= exprUsesString(es->expr.get());
                if (auto* as = dynamic_cast<const AssignStmt*>(st.get()))
                    found |= exprUsesString(as->value.get()) ||
                             exprUsesString(as->target.get());
                if (auto* rs = dynamic_cast<const ReturnStmt*>(st.get()))
                    found |= exprUsesString(rs->value.get());
            }
            found |= exprUsesString(blk->tail.get());
            return found;
        }
        return false;
    }

    bool programUsesString(const Program& program) {
        auto mentions = [](const TypeRef& t) {
            return t.name == "String";
        };
        for (const auto& c : program.consts)
            if (mentions(c.type)) return true;
        for (const auto& s : program.structs)
            for (const auto& f : s.fields)
                if (mentions(f.type)) return true;
        for (const auto& fn : program.functions) {
            if (mentions(fn.returnType)) return true;
            for (const auto& p : fn.params)
                if (mentions(p.type)) return true;
        }
        // Fall back to a textual probe of the whole program's call names: a
        // program may only touch String via builtins (e.g. print_str(&"x")).
        return programCallsStringBuiltin_;
    }

    // v30 Phase 163: does any signature / field / const type mention Vec<T>?
    // (sawVecCall_ separately covers a vec builtin call in a body.)
    bool programUsesVec(const Program& program) {
        auto mentions = [](const TypeRef& t) { return t.name == "Vec"; };
        for (const auto& c : program.consts)
            if (mentions(c.type)) return true;
        for (const auto& s : program.structs)
            for (const auto& f : s.fields)
                if (mentions(f.type)) return true;
        for (const auto& fn : program.functions) {
            if (mentions(fn.returnType)) return true;
            for (const auto& p : fn.params)
                if (mentions(p.type)) return true;
        }
        return false;
    }

    // v30 Phase 162: a faithful C re-implementation of the String runtime the
    // LLVM backend builds as IR. `struct kdstr { char* data; int64_t len;
    // int64_t cap; }`; cap==0 marks a borrowed/read-only literal (never freed),
    // so a mutation copies-on-write to a heap buffer first. Reads work uniformly
    // on borrowed + heap strings. Semantics mirror codegen.cpp's builtins
    // exactly (so the differential gate holds): str_char_at clamps OOB to -1,
    // str_substring clamps, str_eq is byte-exact, push grows by doubling,
    // int_to_string is decimal, the print fns use the same printf formats.
    void emitStringRuntime() {
        out << "#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n";
        out << "struct kdstr { char* data; int64_t len; int64_t cap; };\n";
        out <<
"static void kd__str_reserve(struct kdstr* s, int64_t need) {\n"
"  if (s->cap == 0) { /* borrowed: copy-on-write */\n"
"    int64_t nc = need > 8 ? need : 8; char* nb = (char*)malloc((size_t)nc);\n"
"    if (s->len > 0 && s->data) memcpy(nb, s->data, (size_t)s->len);\n"
"    s->data = nb; s->cap = nc; return; }\n"
"  if (s->cap >= need) return;\n"
"  int64_t nc = s->cap * 2; if (nc < need) nc = need;\n"
"  s->data = (char*)realloc(s->data, (size_t)nc); s->cap = nc;\n"
"}\n"
"static struct kdstr kd_string_new(void) {\n"
"  struct kdstr s; s.data = 0; s.len = 0; s.cap = 0; return s; /* lazy alloc */\n"
"}\n"
"static int64_t kd_str_len(struct kdstr* s) { return s->len; }\n"
"static int64_t kd_str_char_at(struct kdstr* s, int64_t i) {\n"
"  if (i < 0 || i >= s->len) return -1;\n"
"  return (int64_t)(unsigned char)s->data[i];\n"
"}\n"
"static int64_t kd_str_push_byte(struct kdstr* s, int64_t b) {\n"
"  kd__str_reserve(s, s->len + 1); s->data[s->len] = (char)(unsigned char)b;\n"
"  s->len += 1; return 0;\n"
"}\n"
"static int64_t kd_string_push_str(struct kdstr* s, struct kdstr o) {\n"
"  if (o.len > 0) { kd__str_reserve(s, s->len + o.len);\n"
"    memcpy(s->data + s->len, o.data, (size_t)o.len); s->len += o.len; }\n"
"  if (o.cap != 0) free(o.data); /* `other` is moved-in: free a heap-owned arg */\n"
"  return 0;\n"
"}\n"
"static int64_t kd_str_eq(struct kdstr* a, struct kdstr* b) {\n"
"  if (a->len != b->len) return 0;\n"
"  return memcmp(a->data, b->data, (size_t)a->len) == 0 ? 1 : 0;\n"
"}\n"
"static struct kdstr kd_str_substring(struct kdstr* s, int64_t start, int64_t len) {\n"
"  if (start < 0) start = 0; if (start > s->len) start = s->len;\n"
"  if (len < 0) len = 0; if (start + len > s->len) len = s->len - start;\n"
"  struct kdstr r; r.cap = len > 0 ? len : 1; r.data = (char*)malloc((size_t)r.cap);\n"
"  if (len > 0) memcpy(r.data, s->data + start, (size_t)len); r.len = len; return r;\n"
"}\n"
"static struct kdstr kd_int_to_string(int64_t v) {\n"
"  char buf[24]; int n = snprintf(buf, sizeof(buf), \"%lld\", (long long)v);\n"
"  struct kdstr r; r.cap = n > 0 ? n : 1; r.data = (char*)malloc((size_t)r.cap);\n"
"  if (n > 0) memcpy(r.data, buf, (size_t)n); r.len = n > 0 ? n : 0; return r;\n"
"}\n"
"static int64_t kd_print_str(struct kdstr* s) { printf(\"%.*s\\n\", (int)s->len, s->data); return 0; }\n"
"static int64_t kd_print_string(struct kdstr* s) { return kd_print_str(s); }\n"
"static int64_t kd_println(struct kdstr* s) { return kd_print_str(s); }\n"
"static int64_t kd_print_no_nl(struct kdstr* s) { printf(\"%.*s\", (int)s->len, s->data); return 0; }\n"
"static int64_t kd_print(int64_t v) { printf(\"%lld\\n\", (long long)v); return 0; }\n"
// v30 Phase 164: drop a String — free only a heap-owned buffer (cap != 0); a
// borrowed literal (cap == 0) points at a C string literal and must NOT be freed.
"static void kd__str_drop(struct kdstr* s) { if (s->cap != 0 && s->data) free(s->data); }\n";
        out << "\n";
    }

    // v30 Phase 163: the scalar-element Vec runtime (int64_t elements: Vec<i64>/
    // Vec<bool>). `struct kdvec { int64_t* data; int64_t len; int64_t cap; }`.
    // Mirrors the LLVM Vec builtins: vec_new {null,0,0}; push doubles from 4;
    // get/get_ref/pop/remove bounds-check (OOB read -> 0 / null) like the LLVM
    // backend; remove/insert memmove the tail; reverse in place; swap by value.
    void emitVecRuntime() {
        out << "#include <stdlib.h>\n#include <string.h>\n";
        out << "struct kdvec { int64_t* data; int64_t len; int64_t cap; };\n";
        out <<
"static struct kdvec kd_vec_new(void) { struct kdvec v; v.data=0; v.len=0; v.cap=0; return v; }\n"
"static int64_t kd_vec_len(struct kdvec* v) { return v->len; }\n"
"static int64_t kd_vec_push(struct kdvec* v, int64_t x) {\n"
"  if (v->len == v->cap) { int64_t nc = v->cap ? v->cap*2 : 4;\n"
"    v->data = (int64_t*)realloc(v->data, (size_t)nc*sizeof(int64_t)); v->cap = nc; }\n"
"  v->data[v->len++] = x; return 0;\n"
"}\n"
"static int64_t kd_vec_get(struct kdvec* v, int64_t i) {\n"
"  if (i < 0 || i >= v->len || !v->data) return 0; return v->data[i];\n"
"}\n"
"static int64_t* kd_vec_get_ref(struct kdvec* v, int64_t i) {\n"
"  if (i < 0 || i >= v->len || !v->data) return 0; return &v->data[i];\n"
"}\n"
"static int64_t kd_vec_pop(struct kdvec* v) {\n"
"  if (v->len <= 0 || !v->data) return 0; return v->data[--v->len];\n"
"}\n"
"static int64_t kd_vec_remove(struct kdvec* v, int64_t i) {\n"
"  if (i < 0 || i >= v->len || !v->data) return 0; int64_t e = v->data[i];\n"
"  memmove(&v->data[i], &v->data[i+1], (size_t)(v->len-i-1)*sizeof(int64_t));\n"
"  v->len--; return e;\n"
"}\n"
"static int64_t kd_vec_insert(struct kdvec* v, int64_t i, int64_t x) {\n"
"  if (i < 0 || i > v->len) return 0;\n"
"  if (v->len == v->cap) { int64_t nc = v->cap ? v->cap*2 : 4;\n"
"    v->data = (int64_t*)realloc(v->data, (size_t)nc*sizeof(int64_t)); v->cap = nc; }\n"
"  memmove(&v->data[i+1], &v->data[i], (size_t)(v->len-i)*sizeof(int64_t));\n"
"  v->data[i] = x; v->len++; return 0;\n"
"}\n"
"static int64_t kd_vec_reverse(struct kdvec* v) {\n"
"  for (int64_t a=0,b=v->len-1; a<b; a++,b--) { int64_t t=v->data[a]; v->data[a]=v->data[b]; v->data[b]=t; }\n"
"  return 0;\n"
"}\n"
"static int64_t kd_vec_swap(struct kdvec* v, int64_t i, int64_t j) {\n"
"  if (i<0||j<0||i>=v->len||j>=v->len||!v->data||i==j) return 0;\n"
"  int64_t t=v->data[i]; v->data[i]=v->data[j]; v->data[j]=t; return 0;\n"
"}\n"
// v30 Phase 164: drop a Vec — free its (scalar-element) heap buffer. data is
// null for an empty Vec; free(NULL) is a no-op, so this is unconditional.
"static void kd__vec_drop(struct kdvec* v) { free(v->data); }\n";
        out << "\n";
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

        out << "/* Generated by kardashev --emit-c (v30 Phase 162). */\n";
        out << "#include <stdint.h>\n";

        emitStructDefs(program);
        if (!ok()) return;
        emitEnumDefs(program);
        if (!ok()) return;
        // v30 Phase 162: the String runtime (emitted only if the program uses
        // String). Pre-scan every fn body for a string literal / builtin call so
        // the runtime is emitted before the fns that reference it.
        for (const auto& fn : program.functions)
            if (fn.body && exprUsesString(fn.body.get()))
                programCallsStringBuiltin_ = true;
        for (const auto& c : program.consts)
            if (exprUsesString(c.value.get()))
                programCallsStringBuiltin_ = true;
        if (programUsesString(program)) emitStringRuntime();
        // v30 Phase 163: the scalar-Vec runtime, gated on a vec builtin call
        // (sawVecCall_, set during the pre-scan above) or a Vec<T> in a
        // signature / field / const type.
        if (sawVecCall_ || programUsesVec(program)) emitVecRuntime();

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
            // v30 Phase 164: drop non-escaping heap-owning top-level locals at
            // function exit (RAII). retCTy types the captured tail value.
            std::string retCTy = ctype(fn.returnType);
            if (retCTy.empty()) return;
            out << sig << " {\n  return " << fnBodyWithDrops(fn, retCTy)
                << ";\n}\n\n";
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
