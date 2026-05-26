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
        if (ra->structFields.size() != rb->structFields.size()) return false;
        for (std::size_t i = 0; i < ra->structFields.size(); ++i) {
            if (ra->structFields[i].first != rb->structFields[i].first) return false;
            if (!unify(ra->structFields[i].second, rb->structFields[i].second)) return false;
        }
        return true;
    }

    // Same primitive kind (Int=Int, Bool=Bool, Unit=Unit).
    return true;
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
    case TypeKind::Struct: return r->structName;
    }
    return "?";
}

} // namespace kardashev
