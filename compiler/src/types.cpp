#include "kardashev/types.hpp"

namespace kardashev {

namespace {
// Process-wide counter for fresh type variables.
int gNextVarId = 0;
} // namespace

TypePtr makeInt() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
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
        return occurs(varId, r->ret);
    }
    return false;
}
} // namespace

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

    if (ra->kind == TypeKind::Function) {
        if (ra->args.size() != rb->args.size()) return false;
        for (std::size_t i = 0; i < ra->args.size(); ++i) {
            if (!unify(ra->args[i], rb->args[i])) return false;
        }
        return unify(ra->ret, rb->ret);
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
        if (!changed) return r;
        return makeFunction(std::move(newArgs), newRet);
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
    default:
        return r;
    }
}

std::string typeToString(const TypePtr& t) {
    TypePtr r = resolve(t);
    switch (r->kind) {
    case TypeKind::Int: return "i64";
    case TypeKind::Bool: return "bool";
    case TypeKind::Unit: return "()";
    case TypeKind::Var: return "'" + std::to_string(r->varId);
    case TypeKind::Function: {
        std::string s = "fn(";
        for (std::size_t i = 0; i < r->args.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeToString(r->args[i]);
        }
        s += ") -> ";
        s += typeToString(r->ret);
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
    }
    return "?";
}

} // namespace kardashev
