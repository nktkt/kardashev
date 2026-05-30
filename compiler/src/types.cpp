#include "kardashev/types.hpp"

#include <unordered_set>

namespace kardashev {

namespace {
// Process-wide counter for fresh type variables.
int gNextVarId = 0;
} // namespace

TypePtr makeInt() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    return t; // default i64-signed (intWidth=64, intSigned=true)
}

TypePtr makeIntW(int width, bool isSigned) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->intWidth = width;
    t->intSigned = isSigned;
    return t;
}

TypePtr makeIntLitVar() {
    TypePtr v = makeFreshVar(); // a Var, so it links via union-find
    v->intLitVar = true;
    return v;
}

std::string intTypeName(int width, bool isSigned) {
    return std::string(isSigned ? "i" : "u") + std::to_string(width);
}

TypePtr makeFloat() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Float;
    return t;
}

TypePtr makeBool() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Bool;
    return t;
}

TypePtr makeUnit() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Unit;
    return t;
}

TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Function;
    t->args = std::move(args);
    t->ret = std::move(ret);
    return t;
}

TypePtr makeFunction(std::vector<TypePtr> args, TypePtr ret,
                     std::vector<std::string> effectLabels,
                     TypePtr effectRowVar) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Function;
    t->args = std::move(args);
    t->ret = std::move(ret);
    t->effectLabels = std::move(effectLabels);
    t->effectRowVar = std::move(effectRowVar);
    return t;
}

TypePtr makeFreshVar() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Var;
    t->varId = gNextVarId++;
    return t;
}

TypePtr makeStruct(std::string name, std::vector<std::pair<std::string, TypePtr>> fields) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Struct;
    t->structName = std::move(name);
    t->structFields = std::move(fields);
    return t;
}

TypePtr makeEnum(std::string name, std::vector<EnumVariantType> variants) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Enum;
    t->enumName = std::move(name);
    t->enumVariants = std::move(variants);
    return t;
}

TypePtr makeRef(TypePtr inner, bool isMut) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Ref;
    t->refInner = std::move(inner);
    t->refIsMut = isMut;
    return t;
}

TypePtr makeDyn(std::string traitName) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Dyn;
    t->dynTraitName = std::move(traitName);
    return t;
}

TypePtr makeBox(TypePtr inner) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Box;
    t->refInner = std::move(inner);
    return t;
}

TypePtr makeSlice(TypePtr elem) {
    // Phase 13b: a slice `&[T]` is modeled as the built-in single-layout
    // struct `Slice` carrying its element type in `typeArgs[0]`. Codegen
    // lowers every Slice to one `{ i8* ptr, i64 len }` fat-pointer layout
    // (element stride is computed separately, like Vec).
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Struct;
    t->structName = "Slice";
    t->typeArgs = {std::move(elem)};
    return t;
}

TypePtr makeArray(TypePtr elem, std::size_t len) {
    // Phase 22: `[T; N]` — a value-type fixed-size array. Element in
    // `arrayElem`, length in `arrayLen`. Lowers to `[N x <T>]`.
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Array;
    t->arrayElem = std::move(elem);
    t->arrayLen = len;
    return t;
}

TypePtr makeTuple(std::vector<TypePtr> elems) {
    // Phase 22: `(A, B, ...)` — a value-type anonymous product. Lowers to an
    // anonymous LLVM struct `{ A, B, ... }`.
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Tuple;
    t->tupleElems = std::move(elems);
    return t;
}

TypePtr makeConstValue(long long v) {
    // Phase 58 (v10): a const-generic VALUE argument. A fresh Int node so it
    // is safe to stamp `isConstValue` / `constValue` (makeInt() never returns
    // a shared singleton). Lives only in a struct/enum/fn instance typeArgs.
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->isConstValue = true;
    t->constValue = v;
    return t;
}

TypePtr makeConstSymbol(std::string name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->isConstValue = true;
    t->constValueName = std::move(name);
    return t;
}

namespace {
TypePtr substConstLenRec(
    const TypePtr& t,
    const std::unordered_map<std::string, std::size_t>& lengths,
    std::unordered_set<const Type*>& visiting) {
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Array: {
        TypePtr elem = substConstLenRec(r->arrayElem, lengths, visiting);
        std::size_t len = r->arrayLen;
        std::string param = r->arrayLenParam;
        bool changed = (elem.get() != r->arrayElem.get());
        if (!param.empty()) {
            if (auto it = lengths.find(param); it != lengths.end()) {
                len = it->second;
                param.clear(); // length is now concrete
                changed = true;
            }
        }
        if (!changed) return r;
        TypePtr a = makeArray(elem, len);
        a->arrayLenParam = param;
        return a;
    }
    case TypeKind::Struct: {
        // Cycle guard: a self-recursive named type re-enters its own node.
        if (!visiting.insert(r.get()).second) return r;
        bool changed = false;
        std::vector<std::pair<std::string, TypePtr>> nf;
        nf.reserve(r->structFields.size());
        for (const auto& [n, f] : r->structFields) {
            TypePtr f2 = substConstLenRec(f, lengths, visiting);
            if (f2.get() != f.get()) changed = true;
            nf.emplace_back(n, std::move(f2));
        }
        std::vector<TypePtr> na;
        na.reserve(r->typeArgs.size());
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = substConstLenRec(a, lengths, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        visiting.erase(r.get());
        if (!changed) return r;
        TypePtr res = makeStruct(r->structName, std::move(nf));
        res->typeArgs = std::move(na);
        return res;
    }
    case TypeKind::Enum: {
        if (!visiting.insert(r.get()).second) return r;
        bool changed = false;
        std::vector<EnumVariantType> nv;
        nv.reserve(r->enumVariants.size());
        for (const auto& v : r->enumVariants) {
            EnumVariantType e;
            e.name = v.name;
            e.payloadTypes.reserve(v.payloadTypes.size());
            for (const auto& p : v.payloadTypes) {
                TypePtr p2 = substConstLenRec(p, lengths, visiting);
                if (p2.get() != p.get()) changed = true;
                e.payloadTypes.push_back(std::move(p2));
            }
            nv.push_back(std::move(e));
        }
        std::vector<TypePtr> na;
        na.reserve(r->typeArgs.size());
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = substConstLenRec(a, lengths, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        visiting.erase(r.get());
        if (!changed) return r;
        TypePtr res = makeEnum(r->enumName, std::move(nv));
        res->typeArgs = std::move(na);
        return res;
    }
    case TypeKind::Ref: {
        TypePtr inner = substConstLenRec(r->refInner, lengths, visiting);
        if (inner.get() == r->refInner.get()) return r;
        return makeRef(inner, r->refIsMut);
    }
    case TypeKind::Box: {
        TypePtr inner = substConstLenRec(r->refInner, lengths, visiting);
        if (inner.get() == r->refInner.get()) return r;
        return makeBox(inner);
    }
    case TypeKind::Tuple: {
        bool changed = false;
        std::vector<TypePtr> ne;
        ne.reserve(r->tupleElems.size());
        for (const auto& el : r->tupleElems) {
            TypePtr e2 = substConstLenRec(el, lengths, visiting);
            if (e2.get() != el.get()) changed = true;
            ne.push_back(std::move(e2));
        }
        if (!changed) return r;
        return makeTuple(std::move(ne));
    }
    case TypeKind::Function: {
        bool changed = false;
        std::vector<TypePtr> na;
        na.reserve(r->args.size());
        for (const auto& a : r->args) {
            TypePtr a2 = substConstLenRec(a, lengths, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        TypePtr nr = substConstLenRec(r->ret, lengths, visiting);
        if (nr.get() != r->ret.get()) changed = true;
        if (!changed) return r;
        return makeFunction(std::move(na), nr, r->effectLabels,
                            r->effectRowVar);
    }
    case TypeKind::Int:
        // Phase 61/62 fix: a SYMBOLIC const-value typeArg (e.g. the `N` passed
        // to a nested `Inner<N>` field of `Outer<N>`) must resolve to the bound
        // value — not just symbolic array lengths. Without this the nested
        // field keeps a symbolic `N` and mangles to `Inner__c0`, clashing with
        // the value's `Inner__c<N>` (an LLVM-verifier failure).
        if (r->isConstValue && !r->constValueName.empty()) {
            if (auto it = lengths.find(r->constValueName); it != lengths.end())
                return makeConstValue(static_cast<long long>(it->second));
        }
        return r;
    default:
        return r;
    }
}
} // namespace

TypePtr substituteConstLengths(
    const TypePtr& t,
    const std::unordered_map<std::string, std::size_t>& lengths) {
    if (lengths.empty()) return t;
    std::unordered_set<const Type*> visiting;
    return substConstLenRec(t, lengths, visiting);
}

namespace {
// Phase 61: deep-copy `t`, RENAMING every symbolic array length `[T; N]` whose
// `arrayLenParam` is a key in `renames` to the new (still symbolic) name. Used
// when a generic type is instantiated with a SYMBOLIC const arg — e.g.
// `Matrix<C, R>` rebinds `Matrix`'s `R`/`C` field lengths to the enclosing
// scope's `C`/`R`. Recursive types are cycle-guarded.
TypePtr renameConstLenRec(const TypePtr& t,
                          const std::unordered_map<std::string, std::string>& rn,
                          std::unordered_set<const Type*>& visiting) {
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Array: {
        TypePtr elem = renameConstLenRec(r->arrayElem, rn, visiting);
        std::string param = r->arrayLenParam;
        bool changed = (elem.get() != r->arrayElem.get());
        if (!param.empty()) {
            if (auto it = rn.find(param); it != rn.end()) {
                param = it->second;
                changed = true;
            }
        }
        if (!changed) return r;
        TypePtr a = makeArray(elem, r->arrayLen);
        a->arrayLenParam = param;
        return a;
    }
    case TypeKind::Struct: {
        if (!visiting.insert(r.get()).second) return r;
        bool changed = false;
        std::vector<std::pair<std::string, TypePtr>> nf;
        for (const auto& [n, f] : r->structFields) {
            TypePtr f2 = renameConstLenRec(f, rn, visiting);
            if (f2.get() != f.get()) changed = true;
            nf.emplace_back(n, std::move(f2));
        }
        std::vector<TypePtr> na;
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = renameConstLenRec(a, rn, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        visiting.erase(r.get());
        if (!changed) return r;
        TypePtr res = makeStruct(r->structName, std::move(nf));
        res->typeArgs = std::move(na);
        return res;
    }
    case TypeKind::Ref:
        { TypePtr i = renameConstLenRec(r->refInner, rn, visiting);
          return i.get() == r->refInner.get() ? r : makeRef(i, r->refIsMut); }
    case TypeKind::Box:
        { TypePtr i = renameConstLenRec(r->refInner, rn, visiting);
          return i.get() == r->refInner.get() ? r : makeBox(i); }
    case TypeKind::Tuple: {
        bool changed = false;
        std::vector<TypePtr> ne;
        for (const auto& el : r->tupleElems) {
            TypePtr e2 = renameConstLenRec(el, rn, visiting);
            if (e2.get() != el.get()) changed = true;
            ne.push_back(std::move(e2));
        }
        return changed ? makeTuple(std::move(ne)) : r;
    }
    case TypeKind::Int:
        // Phase 61/62 fix: rename a SYMBOLIC const-value typeArg (a nested
        // `Inner<N>` field) through the symbolic rename map, mirroring the
        // array-length rename — so `Matrix<C, R>` with a nested const-generic
        // field rebinds correctly instead of keeping a stale symbol.
        if (r->isConstValue && !r->constValueName.empty()) {
            if (auto it = rn.find(r->constValueName); it != rn.end())
                return makeConstSymbol(it->second);
        }
        return r;
    case TypeKind::Function: {
        // Needed so renaming a fn SIGNATURE (forwarding a const-generic array
        // into another const-generic fn) reaches the param/return types.
        bool changed = false;
        std::vector<TypePtr> na;
        na.reserve(r->args.size());
        for (const auto& a : r->args) {
            TypePtr a2 = renameConstLenRec(a, rn, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        TypePtr nr = renameConstLenRec(r->ret, rn, visiting);
        if (nr.get() != r->ret.get()) changed = true;
        if (!changed) return r;
        return makeFunction(std::move(na), nr, r->effectLabels, r->effectRowVar);
    }
    case TypeKind::Enum: {
        if (!visiting.insert(r.get()).second) return r;
        bool changed = false;
        std::vector<EnumVariantType> nv;
        for (const auto& v : r->enumVariants) {
            EnumVariantType e;
            e.name = v.name;
            for (const auto& p : v.payloadTypes) {
                TypePtr p2 = renameConstLenRec(p, rn, visiting);
                if (p2.get() != p.get()) changed = true;
                e.payloadTypes.push_back(std::move(p2));
            }
            nv.push_back(std::move(e));
        }
        std::vector<TypePtr> na;
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = renameConstLenRec(a, rn, visiting);
            if (a2.get() != a.get()) changed = true;
            na.push_back(std::move(a2));
        }
        visiting.erase(r.get());
        if (!changed) return r;
        TypePtr res = makeEnum(r->enumName, std::move(nv));
        res->typeArgs = std::move(na);
        return res;
    }
    default:
        return r;
    }
}
} // namespace

TypePtr renameConstLengths(
    const TypePtr& t,
    const std::unordered_map<std::string, std::string>& renames) {
    if (renames.empty()) return t;
    std::unordered_set<const Type*> visiting;
    return renameConstLenRec(t, renames, visiting);
}

TypePtr instantiateGeneric(const TypePtr& schemaType,
                           const std::vector<TypePtr>& genericVars,
                           const std::vector<std::string>& constParamNames,
                           std::vector<TypePtr> typeArgs, bool isStruct) {
    std::unordered_map<int, TypePtr> subst;
    std::unordered_map<std::string, std::size_t> constSubst;
    std::unordered_map<std::string, std::string> constRename;
    for (std::size_t i = 0;
         i < genericVars.size() && i < typeArgs.size(); ++i) {
        bool isConstSlot =
            i < constParamNames.size() && !constParamNames[i].empty();
        if (isConstSlot) {
            TypePtr a = resolve(typeArgs[i]);
            if (a->isConstValue && !a->constValueName.empty())
                constRename[constParamNames[i]] = a->constValueName; // symbolic
            else if (a->isConstValue && a->constValue >= 0)
                constSubst[constParamNames[i]] =
                    static_cast<std::size_t>(a->constValue);
        } else {
            subst[genericVars[i]->varId] = typeArgs[i];
        }
    }
    TypePtr inst = instantiate(schemaType, subst);
    if (!constRename.empty()) {
        std::unordered_set<const Type*> v;
        inst = renameConstLenRec(inst, constRename, v);
    }
    if (!constSubst.empty())
        inst = substituteConstLengths(inst, constSubst);
    // Guarantee a fresh node before stamping typeArgs (an all-const generic
    // with no symbolic length leaves `inst` aliasing the shared schema).
    if (inst.get() == schemaType.get()) {
        inst = isStruct ? makeStruct(inst->structName, inst->structFields)
                        : makeEnum(inst->enumName, inst->enumVariants);
    }
    inst->typeArgs = std::move(typeArgs);
    return inst;
}

TypePtr makeFuture(TypePtr result) {
    // Phase 17b: a `Future<T>` is the built-in single-layout struct `Future`
    // carrying its result type in `typeArgs[0]`. Codegen lowers every Future
    // to one `{ i8* poll, i8* frame }` layout regardless of T (the result is
    // surfaced separately per await/block_on site, like Vec's element).
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Struct;
    t->structName = "Future";
    t->typeArgs = {std::move(result)};
    return t;
}

TypePtr resolve(const TypePtr& t) {
    if (t->kind != TypeKind::Var || !t->link) return t;
    TypePtr rep = resolve(t->link);
    t->link = rep; // path compression
    return rep;
}

namespace {
bool occurs(int varId, const TypePtr& t) {
    TypePtr r = resolve(t);
    if (r->kind == TypeKind::Var) return r->varId == varId;
    if (r->kind == TypeKind::Function) {
        for (const auto& a : r->args) {
            if (occurs(varId, a)) return true;
        }
        if (occurs(varId, r->ret)) return true;
        // The effect-row tail var is part of the function type, so the
        // occurs check must descend into it too.
        if (r->effectRowVar && occurs(varId, r->effectRowVar)) return true;
        return false;
    }
    // Phase 11: a Box<T> can hold a Var T, so descend (mirrors how the
    // existing code descends into Function args).
    if (r->kind == TypeKind::Box) return occurs(varId, r->refInner);
    // Phase 22: arrays / tuples can hold Var element types, so descend.
    if (r->kind == TypeKind::Array) return occurs(varId, r->arrayElem);
    if (r->kind == TypeKind::Tuple) {
        for (const auto& el : r->tupleElems)
            if (occurs(varId, el)) return true;
        return false;
    }
    return false;
}
} // namespace

namespace {
// Phase 10a helpers for effect-row unification.
//
// An effect row on a Function type is (closed-labels L, optional tail var
// v). A solved tail var stashes its concrete labels on its own node
// (`effectRowSolved` + `effectLabels`). These helpers read/extend that.

bool labelSetContains(const std::vector<std::string>& v, const std::string& l) {
    for (const auto& x : v) if (x == l) return true;
    return false;
}

// The labels a (resolved) row-var node currently stands for: nothing while
// unbound, or its solved remainder once unification pinned it.
const std::vector<std::string>& rowVarLabels(const TypePtr& rv) {
    static const std::vector<std::string> kEmpty;
    if (rv->effectRowSolved) return rv->effectLabels;
    return kEmpty;
}

// Pull the closed labels of a function row, merging in a solved tail var's
// labels (so e.g. `{io, e}` with `e := {alloc}` reads back as {io, alloc}).
std::vector<std::string> effectiveRowLabels(const TypePtr& fn) {
    std::vector<std::string> out = fn->effectLabels;
    if (fn->effectRowVar) {
        TypePtr v = resolve(fn->effectRowVar);
        if (v->kind == TypeKind::Var) {
            for (const auto& l : rowVarLabels(v))
                if (!labelSetContains(out, l)) out.push_back(l);
        }
    }
    return out;
}

// Solve an (unbound) row-var node to exactly the given label set.
void solveRowVar(const TypePtr& rv, const std::vector<std::string>& labels) {
    rv->effectRowSolved = true;
    rv->effectLabels = labels;
}

// Unify two function effect rows. Concrete labels present on one side but
// absent on the other must be absorbed by that other side's tail var; if
// the other side has no tail var (a closed row) this is a mismatch and we
// return false. When both sides carry an (unbound) tail var we link them so
// future solving stays in sync. This mirrors HM type-var unification but
// over effect-label sets.
bool unifyEffectRows(const TypePtr& fa, const TypePtr& fb) {
    std::vector<std::string> la = effectiveRowLabels(fa);
    std::vector<std::string> lb = effectiveRowLabels(fb);
    TypePtr va = fa->effectRowVar ? resolve(fa->effectRowVar) : nullptr;
    TypePtr vb = fb->effectRowVar ? resolve(fb->effectRowVar) : nullptr;
    bool aOpen = va && va->kind == TypeKind::Var && !va->effectRowSolved;
    bool bOpen = vb && vb->kind == TypeKind::Var && !vb->effectRowSolved;

    // Labels only on A: B must be able to absorb them via an open tail.
    std::vector<std::string> onlyA, onlyB;
    for (const auto& l : la) if (!labelSetContains(lb, l)) onlyA.push_back(l);
    for (const auto& l : lb) if (!labelSetContains(la, l)) onlyB.push_back(l);
    if (!onlyA.empty() && !bOpen) return false;
    if (!onlyB.empty() && !aOpen) return false;

    // Grow each open tail var so the rows become label-equal. After this,
    // both rows denote the union of their labels.
    if (bOpen && !onlyA.empty()) {
        std::vector<std::string> grown = rowVarLabels(vb);
        for (const auto& l : onlyA)
            if (!labelSetContains(grown, l)) grown.push_back(l);
        solveRowVar(vb, grown);
    }
    if (aOpen && !onlyB.empty()) {
        std::vector<std::string> grown = rowVarLabels(va);
        for (const auto& l : onlyB)
            if (!labelSetContains(grown, l)) grown.push_back(l);
        solveRowVar(va, grown);
    }
    // Two still-open tail vars: link them so they solve together later.
    if (va && vb && va.get() != vb.get() &&
        va->kind == TypeKind::Var && vb->kind == TypeKind::Var &&
        !va->effectRowSolved && !vb->effectRowSolved) {
        if (va->varId < vb->varId) vb->link = va;
        else va->link = vb;
    }
    return true;
}
} // namespace

std::vector<std::string> resolveEffectRow(const TypePtr& fnType) {
    TypePtr r = resolve(fnType);
    if (r->kind != TypeKind::Function) return {};
    return effectiveRowLabels(r);
}

bool unify(const TypePtr& a, const TypePtr& b) {
    TypePtr ra = resolve(a);
    TypePtr rb = resolve(b);
    if (ra.get() == rb.get()) return true;

    // Var-Var: link the higher-id (younger) to the lower-id (older). Phase
    // 3 monomorphization depends on this: the SCHEMA Vars introduced for a
    // generic fn's type parameters are created strictly before any
    // call-site fresh Vars that get unified against them, so the schema
    // Var stays at the root of the union-find tree. Codegen's
    // `resolveInInstance` keys its substitution map by schema Var IDs, so
    // resolution chains must end at those schema Vars to reach the
    // concrete TypePtr for the instance.
    if (ra->kind == TypeKind::Var && rb->kind == TypeKind::Var) {
        if (ra->varId < rb->varId) rb->link = ra;
        else ra->link = rb;
        return true;
    }
    if (ra->kind == TypeKind::Var) {
        if (occurs(ra->varId, rb)) return false;
        ra->link = rb;
        return true;
    }
    if (rb->kind == TypeKind::Var) {
        if (occurs(rb->varId, ra)) return false;
        rb->link = ra;
        return true;
    }
    if (ra->kind != rb->kind) return false;

    // v11: two CONCRETE machine integers unify iff same width AND signedness
    // (no implicit widening; `as` bridges). Const-value args (Phase 58) keep
    // their own guard below.
    if (ra->kind == TypeKind::Int && !ra->isConstValue && !rb->isConstValue) {
        if (ra->intWidth != rb->intWidth || ra->intSigned != rb->intSigned)
            return false;
    }

    if (ra->kind == TypeKind::Function) {
        if (ra->args.size() != rb->args.size()) return false;
        for (std::size_t i = 0; i < ra->args.size(); ++i) {
            if (!unify(ra->args[i], rb->args[i])) return false;
        }
        if (!unify(ra->ret, rb->ret)) return false;
        // Phase 10a: function types agree only if their effect rows do.
        return unifyEffectRows(ra, rb);
    }

    if (ra->kind == TypeKind::Struct) {
        if (ra->structName != rb->structName) return false;
        // For generic structs, the typeArgs vector carries the
        // instantiation identity. Two Box<T> aren't equal unless their T's
        // are; conversely, two distinct TypePtr instantiations of Box<i64>
        // do unify because their typeArgs unify pointwise. Note: the
        // structFields vector is derived from typeArgs at materialization
        // time, so unifying typeArgs implicitly unifies fields — we still
        // walk fields below as a defensive consistency check.
        if (ra->typeArgs.size() != rb->typeArgs.size()) return false;
        for (std::size_t i = 0; i < ra->typeArgs.size(); ++i) {
            if (!unify(ra->typeArgs[i], rb->typeArgs[i])) return false;
        }
        if (ra->structFields.size() != rb->structFields.size()) return false;
        for (std::size_t i = 0; i < ra->structFields.size(); ++i) {
            if (ra->structFields[i].first != rb->structFields[i].first) return false;
            if (!unify(ra->structFields[i].second, rb->structFields[i].second)) return false;
        }
        return true;
    }

    if (ra->kind == TypeKind::Ref) {
        // Phase 2.4b: `&T ~ &U` iff T ~ U and mutability matches. Phase
        // 2.4c may relax this to allow `&mut T` subtyping into `&T`.
        if (ra->refIsMut != rb->refIsMut) return false;
        return unify(ra->refInner, rb->refInner);
    }

    if (ra->kind == TypeKind::Dyn) {
        // Phase 11: two `dyn Trait` objects unify iff the trait matches.
        // Phase 49: a PARAMETERIZED trait object also requires equal trait args
        // (`dyn Producer<i64>` != `dyn Producer<String>`).
        if (ra->dynTraitName != rb->dynTraitName) return false;
        if (ra->typeArgs.size() != rb->typeArgs.size()) return false;
        for (std::size_t i = 0; i < ra->typeArgs.size(); ++i)
            if (!unify(ra->typeArgs[i], rb->typeArgs[i])) return false;
        return true;
    }

    if (ra->kind == TypeKind::Box) {
        // Phase 11: `Box<T> ~ Box<U>` iff T ~ U.
        return unify(ra->refInner, rb->refInner);
    }

    if (ra->kind == TypeKind::Array) {
        // Phase 22: `[T; N] ~ [U; M]` iff N == M and T ~ U.
        // Phase 61: a SYMBOLIC length (arrayLenParam set, e.g. `[T; CAP]` in a
        // generic impl) matches any length; two CONCRETE lengths must be equal.
        if (ra->arrayLenParam.empty() && rb->arrayLenParam.empty() &&
            ra->arrayLen != rb->arrayLen)
            return false;
        return unify(ra->arrayElem, rb->arrayElem);
    }

    if (ra->kind == TypeKind::Tuple) {
        // Phase 22: tuples unify iff same arity and pointwise element unify.
        if (ra->tupleElems.size() != rb->tupleElems.size()) return false;
        for (std::size_t i = 0; i < ra->tupleElems.size(); ++i) {
            if (!unify(ra->tupleElems[i], rb->tupleElems[i])) return false;
        }
        return true;
    }

    if (ra->kind == TypeKind::Enum) {
        if (ra->enumName != rb->enumName) return false;
        if (ra->typeArgs.size() != rb->typeArgs.size()) return false;
        for (std::size_t i = 0; i < ra->typeArgs.size(); ++i) {
            if (!unify(ra->typeArgs[i], rb->typeArgs[i])) return false;
        }
        if (ra->enumVariants.size() != rb->enumVariants.size()) return false;
        for (std::size_t i = 0; i < ra->enumVariants.size(); ++i) {
            if (ra->enumVariants[i].name != rb->enumVariants[i].name) return false;
            if (ra->enumVariants[i].payloadTypes.size() != rb->enumVariants[i].payloadTypes.size()) return false;
            for (std::size_t j = 0; j < ra->enumVariants[i].payloadTypes.size(); ++j) {
                if (!unify(ra->enumVariants[i].payloadTypes[j], rb->enumVariants[i].payloadTypes[j])) return false;
            }
        }
        return true;
    }

    // Phase 58: const-generic VALUE arguments are Int nodes carrying a value.
    // `Mat<3>` and `Mat<5>` must NOT unify; a const value never unifies with
    // an ordinary i64 (they only ever meet in matching type-arg slots). Two
    // concrete const values unify iff equal — a mismatch is the compile-time
    // dimension error the const-generic feature promises.
    if (ra->kind == TypeKind::Int &&
        (ra->isConstValue || rb->isConstValue)) {
        // Phase 61: a SYMBOLIC const arg (a const param in scope, value not yet
        // known) matches any const value — the concrete check happens at the
        // leaf monomorphization. Two CONCRETE const values must be equal (the
        // Phase 59 dimension check).
        if (!ra->constValueName.empty() || !rb->constValueName.empty())
            return ra->isConstValue && rb->isConstValue;
        return ra->isConstValue && rb->isConstValue &&
               ra->constValue == rb->constValue;
    }

    // Same primitive kind (Int=Int, Bool=Bool, Unit=Unit).
    return true;
}

TypePtr instantiate(const TypePtr& t,
                    const std::unordered_map<int, TypePtr>& subst) {
    if (subst.empty()) return t;
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Var: {
        auto it = subst.find(r->varId);
        if (it != subst.end()) return it->second;
        return r;
    }
    case TypeKind::Function: {
        bool changed = false;
        std::vector<TypePtr> newArgs;
        newArgs.reserve(r->args.size());
        for (const auto& a : r->args) {
            TypePtr a2 = instantiate(a, subst);
            if (a2.get() != a.get()) changed = true;
            newArgs.push_back(std::move(a2));
        }
        TypePtr newRet = instantiate(r->ret, subst);
        if (newRet.get() != r->ret.get()) changed = true;
        // Phase 10a: the effect-row tail var is substituted exactly like a
        // type Var, so instantiating a schema at a call site gives each use
        // a fresh row var (which unification then binds to the actual
        // callee's effects). Concrete labels are copied verbatim.
        TypePtr newRowVar = r->effectRowVar;
        if (r->effectRowVar) {
            newRowVar = instantiate(r->effectRowVar, subst);
            if (newRowVar.get() != r->effectRowVar.get()) changed = true;
        }
        if (!changed) return r;
        return makeFunction(std::move(newArgs), newRet, r->effectLabels,
                            newRowVar);
    }
    case TypeKind::Struct: {
        bool changed = false;
        std::vector<TypePtr> newTypeArgs;
        newTypeArgs.reserve(r->typeArgs.size());
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = instantiate(a, subst);
            if (a2.get() != a.get()) changed = true;
            newTypeArgs.push_back(std::move(a2));
        }
        std::vector<std::pair<std::string, TypePtr>> newFields;
        newFields.reserve(r->structFields.size());
        for (const auto& [fname, fty] : r->structFields) {
            TypePtr fty2 = instantiate(fty, subst);
            if (fty2.get() != fty.get()) changed = true;
            newFields.emplace_back(fname, std::move(fty2));
        }
        if (!changed) return r;
        TypePtr res = makeStruct(r->structName, std::move(newFields));
        res->typeArgs = std::move(newTypeArgs);
        return res;
    }
    case TypeKind::Enum: {
        bool changed = false;
        std::vector<TypePtr> newTypeArgs;
        newTypeArgs.reserve(r->typeArgs.size());
        for (const auto& a : r->typeArgs) {
            TypePtr a2 = instantiate(a, subst);
            if (a2.get() != a.get()) changed = true;
            newTypeArgs.push_back(std::move(a2));
        }
        std::vector<EnumVariantType> newVariants;
        newVariants.reserve(r->enumVariants.size());
        for (const auto& v : r->enumVariants) {
            EnumVariantType nv;
            nv.name = v.name;
            nv.payloadTypes.reserve(v.payloadTypes.size());
            for (const auto& p : v.payloadTypes) {
                TypePtr p2 = instantiate(p, subst);
                if (p2.get() != p.get()) changed = true;
                nv.payloadTypes.push_back(std::move(p2));
            }
            newVariants.push_back(std::move(nv));
        }
        if (!changed) return r;
        TypePtr res = makeEnum(r->enumName, std::move(newVariants));
        res->typeArgs = std::move(newTypeArgs);
        return res;
    }
    case TypeKind::Ref: {
        TypePtr inner = instantiate(r->refInner, subst);
        if (inner.get() == r->refInner.get()) return r;
        return makeRef(inner, r->refIsMut);
    }
    case TypeKind::Box: {
        TypePtr inner = instantiate(r->refInner, subst);
        if (inner.get() == r->refInner.get()) return r;
        return makeBox(inner);
    }
    case TypeKind::Array: {
        TypePtr elem = instantiate(r->arrayElem, subst);
        if (elem.get() == r->arrayElem.get()) return r;
        TypePtr a = makeArray(elem, r->arrayLen);
        // Phase 58: a symbolic length `[T; N]` survives element substitution;
        // it is resolved to a concrete length later by substituteConstLengths.
        a->arrayLenParam = r->arrayLenParam;
        return a;
    }
    case TypeKind::Tuple: {
        bool changed = false;
        std::vector<TypePtr> newElems;
        newElems.reserve(r->tupleElems.size());
        for (const auto& el : r->tupleElems) {
            TypePtr el2 = instantiate(el, subst);
            if (el2.get() != el.get()) changed = true;
            newElems.push_back(std::move(el2));
        }
        if (!changed) return r;
        return makeTuple(std::move(newElems));
    }
    default:
        return r;
    }
}

std::string typeToString(const TypePtr& t) {
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Int:
        // Phase 58/61: a const-generic value argument prints as its value (or
        // its symbolic param name), so `Matrix<3, 4>` / `Matrix<C, R>` read
        // naturally in diagnostics, not `i64`.
        if (r->isConstValue)
            return r->constValueName.empty() ? std::to_string(r->constValue)
                                             : r->constValueName;
        return intTypeName(r->intWidth, r->intSigned); // v11: i8..u64
    case TypeKind::Bool: return "bool";
    case TypeKind::Unit: return "()";
    case TypeKind::Var:
        // v11: an unconstrained integer-literal var defaults to i64.
        if (r->intLitVar) return "i64";
        return "'" + std::to_string(r->varId);
    case TypeKind::Function: {
        std::string s = "fn(";
        for (std::size_t i = 0; i < r->args.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeToString(r->args[i]);
        }
        s += ") -> ";
        s += typeToString(r->ret);
        // Phase 10a: append the effect row when non-pure, so diagnostics
        // distinguish `fn(i64)->i64` from `fn(i64)->i64 ! {io}`. An unsolved
        // row var prints as its Var name (e.g. `'7`).
        std::vector<std::string> labels = r->effectLabels;
        bool hasOpenVar = false;
        if (r->effectRowVar) {
            TypePtr v = resolve(r->effectRowVar);
            if (v->kind == TypeKind::Var && v->effectRowSolved) {
                for (const auto& l : v->effectLabels) {
                    bool dup = false;
                    for (const auto& e : labels) if (e == l) { dup = true; break; }
                    if (!dup) labels.push_back(l);
                }
            } else if (v->kind == TypeKind::Var) {
                hasOpenVar = true;
            }
        }
        if (!labels.empty() || hasOpenVar) {
            s += " ! {";
            bool first = true;
            for (const auto& l : labels) {
                s += (first ? " " : ", ");
                s += l;
                first = false;
            }
            if (hasOpenVar) {
                s += (first ? " " : ", ");
                s += typeToString(r->effectRowVar);
                first = false;
            }
            s += " }";
        }
        return s;
    }
    case TypeKind::Struct:
    case TypeKind::Enum: {
        const std::string& base = r->kind == TypeKind::Struct
                                      ? r->structName
                                      : r->enumName;
        if (r->typeArgs.empty()) return base;
        std::string s = base + "<";
        for (std::size_t i = 0; i < r->typeArgs.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeToString(r->typeArgs[i]);
        }
        s += ">";
        return s;
    }
    case TypeKind::Ref:
        return std::string(r->refIsMut ? "&mut " : "&") +
               typeToString(r->refInner);
    case TypeKind::Dyn:
        return "dyn " + r->dynTraitName;
    case TypeKind::Box:
        return "Box<" + typeToString(r->refInner) + ">";
    case TypeKind::Array:
        return "[" + typeToString(r->arrayElem) + "; " +
               std::to_string(r->arrayLen) + "]";
    case TypeKind::Tuple: {
        std::string s = "(";
        for (std::size_t i = 0; i < r->tupleElems.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeToString(r->tupleElems[i]);
        }
        s += ")";
        return s;
    }
    }
    return "?";
}

} // namespace kardashev
