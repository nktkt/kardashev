// Maranget usefulness algorithm — implementation.
//
// We normalize each ast::Pattern into a small `PatView` variant (Wild /
// Lit / Ctor) and operate on a matrix of `PatView` rows. The driver
// asks "is the wildcard row useful against the matrix of arms?" — if
// yes, the match is non-exhaustive and the recursion has synthesized a
// concrete row of patterns *not* covered, which we pretty-print.
//
// Column types are tracked alongside the matrix: when we specialize on
// a constructor of arity k, the column-0 type is replaced by k columns
// of the constructor's payload types. This is what lets us detect
// "complete signature" of an enum vs an infinite type like Int.

#include "kardashev/pattern_match.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kardashev::pattern_match {

namespace {

// -------------------- PatView (normalized pattern) --------------------

struct PatView;
using PatViewPtr = std::shared_ptr<PatView>;

struct Wild {
    // If non-empty, this wildcard came from a `VarPat` that the user
    // wrote as a real binding (not a unit-constructor). Decision-tree
    // compilation uses this to emit `(name, occurrence-path)` bindings on
    // Leaf nodes. Coverage logic ignores this field entirely.
    std::string bindingName;
};
struct Lit {
    std::int64_t value;
};
struct Ctor {
    std::string name;
    std::vector<PatViewPtr> subs;
};

struct PatView {
    std::variant<Wild, Lit, Ctor> v;
};

PatViewPtr mkWild() {
    return std::make_shared<PatView>(PatView{Wild{}});
}
PatViewPtr mkWildNamed(std::string name) {
    return std::make_shared<PatView>(PatView{Wild{std::move(name)}});
}
PatViewPtr mkLit(std::int64_t value) {
    return std::make_shared<PatView>(PatView{Lit{value}});
}
PatViewPtr mkCtor(std::string name, std::vector<PatViewPtr> subs) {
    return std::make_shared<PatView>(PatView{Ctor{std::move(name), std::move(subs)}});
}

// Normalize an AST pattern into a PatView. A bare VarPat whose name is
// in the global variant index is rewritten to a unit-Ctor — this mirrors
// the typechecker's reinterpretation of bare `Ident` in pattern position.
PatViewPtr view(
    const ast::Pattern& p,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex) {
    if (dynamic_cast<const ast::WildPat*>(&p)) {
        return mkWild();
    }
    if (auto lp = dynamic_cast<const ast::LitIntPat*>(&p)) {
        return mkLit(lp->value);
    }
    if (auto vp = dynamic_cast<const ast::VarPat*>(&p)) {
        auto it = variantIndex.find(vp->name);
        if (it != variantIndex.end()) {
            // Bare ident that names a constructor → unit-Ctor.
            return mkCtor(vp->name, {});
        }
        // Real binding — wildcard-equivalent for coverage, but remember
        // the name so decision-tree compilation can emit a binding.
        return mkWildNamed(vp->name);
    }
    if (auto cp = dynamic_cast<const ast::CtorPat*>(&p)) {
        std::vector<PatViewPtr> subs;
        subs.reserve(cp->subpatterns.size());
        for (const auto& sub : cp->subpatterns) {
            subs.push_back(view(*sub, variantIndex));
        }
        return mkCtor(cp->ctorName, std::move(subs));
    }
    // Phase 36: a tuple pattern is a single-constructor (irrefutable) shape —
    // model it as a reserved `(tuple)` ctor so specialize / useful treat it
    // uniformly with enum constructors.
    if (auto tp = dynamic_cast<const ast::TuplePat*>(&p)) {
        std::vector<PatViewPtr> subs;
        subs.reserve(tp->elements.size());
        for (const auto& sub : tp->elements) {
            subs.push_back(view(*sub, variantIndex));
        }
        return mkCtor("(tuple)", std::move(subs));
    }
    // Should never happen for a well-formed AST.
    return mkWild();
}

// -------------------- Matrix and column types --------------------

using Row = std::vector<PatViewPtr>;
using Matrix = std::vector<Row>;

// `columnTypes[i]` is the resolved type of the i-th column. Used to
// compute the constructor signature for the head column in `useful`.
using ColumnTypes = std::vector<TypePtr>;

// Lookup the EnumVariantType for a given variant name in an enum type.
const EnumVariantType* findVariant(const TypePtr& enumType,
                                   const std::string& name) {
    TypePtr r = resolve(enumType);
    if (r->kind != TypeKind::Enum) return nullptr;
    for (const auto& v : r->enumVariants) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

// -------------------- Matrix operations --------------------

// Specialize on a constructor — for each row whose head matches `ctorName`,
// keep it (replacing the head with its k subpatterns); for wildcard heads,
// expand to k wildcards. Other heads are dropped.
Matrix specialize(const Matrix& P, const std::string& ctorName,
                  unsigned arity) {
    Matrix R;
    for (const auto& row : P) {
        assert(!row.empty());
        const PatView& head = *row[0];
        if (auto ctor = std::get_if<Ctor>(&head.v)) {
            if (ctor->name == ctorName) {
                Row newRow;
                newRow.reserve(arity + row.size() - 1);
                for (const auto& s : ctor->subs) newRow.push_back(s);
                for (std::size_t i = 1; i < row.size(); ++i) {
                    newRow.push_back(row[i]);
                }
                R.push_back(std::move(newRow));
            }
        } else if (std::holds_alternative<Wild>(head.v)) {
            Row newRow;
            newRow.reserve(arity + row.size() - 1);
            for (unsigned i = 0; i < arity; ++i) newRow.push_back(mkWild());
            for (std::size_t i = 1; i < row.size(); ++i) {
                newRow.push_back(row[i]);
            }
            R.push_back(std::move(newRow));
        }
        // Lit head → skip (a literal value doesn't match a ctor).
    }
    return R;
}

// Specialize on a specific integer literal value.
Matrix specializeLit(const Matrix& P, std::int64_t value) {
    Matrix R;
    for (const auto& row : P) {
        assert(!row.empty());
        const PatView& head = *row[0];
        if (auto lit = std::get_if<Lit>(&head.v)) {
            if (lit->value == value) {
                Row newRow(row.begin() + 1, row.end());
                R.push_back(std::move(newRow));
            }
        } else if (std::holds_alternative<Wild>(head.v)) {
            Row newRow(row.begin() + 1, row.end());
            R.push_back(std::move(newRow));
        }
    }
    return R;
}

// Default matrix — keep only rows whose head is wildcard, drop the head.
Matrix defaultMatrix(const Matrix& P) {
    Matrix R;
    for (const auto& row : P) {
        assert(!row.empty());
        if (std::holds_alternative<Wild>(row[0]->v)) {
            Row newRow(row.begin() + 1, row.end());
            R.push_back(std::move(newRow));
        }
    }
    return R;
}

// Build the column types after specializing on a ctor of given arity:
// drop column 0, prepend the ctor's payload types.
ColumnTypes specializeColumnTypes(const ColumnTypes& cols,
                                  const std::vector<TypePtr>& payload) {
    ColumnTypes out;
    out.reserve(payload.size() + cols.size() - 1);
    for (const auto& t : payload) out.push_back(t);
    for (std::size_t i = 1; i < cols.size(); ++i) out.push_back(cols[i]);
    return out;
}

ColumnTypes dropFirst(const ColumnTypes& cols) {
    return ColumnTypes(cols.begin() + 1, cols.end());
}

// Collect head-column constructor / literal info from a matrix.
struct HeadCtors {
    // ctor name -> arity (from the first occurrence; algorithm doesn't
    // care about the value past presence).
    std::set<std::string> ctorNames;
    std::set<std::int64_t> litValues;
};

HeadCtors collectHeads(const Matrix& P) {
    HeadCtors h;
    for (const auto& row : P) {
        assert(!row.empty());
        const PatView& head = *row[0];
        if (auto c = std::get_if<Ctor>(&head.v)) {
            h.ctorNames.insert(c->name);
        } else if (auto l = std::get_if<Lit>(&head.v)) {
            h.litValues.insert(l->value);
        }
    }
    return h;
}

// -------------------- Pretty-printing witness rows --------------------

std::string render(const PatView& p) {
    if (std::holds_alternative<Wild>(p.v)) return "_";
    if (auto l = std::get_if<Lit>(&p.v)) return std::to_string(l->value);
    if (auto c = std::get_if<Ctor>(&p.v)) {
        if (c->subs.empty()) return c->name;
        std::string s = c->name + "(";
        for (std::size_t i = 0; i < c->subs.size(); ++i) {
            if (i > 0) s += ", ";
            s += render(*c->subs[i]);
        }
        s += ")";
        return s;
    }
    return "_";
}

// -------------------- Core `useful` recursion --------------------
//
// Returns (true, witnessRow) if `q` is useful against `P` — meaning some
// vector of values matches `q` but none of the rows of P. The witness
// row has the same column count as q, and its head is something that
// covers a non-matched value.
//
// For exhaustiveness checking, we always call this with `q` being a row
// of all wildcards. The interesting work happens in the wildcard
// branch: we either drive the recursion through every variant of a
// complete signature (and report the first one that yields a useful
// witness), or fall through to the default matrix when the signature is
// incomplete or infinite.

struct UsefulResult {
    bool useful;
    Row witness;
};

UsefulResult useful(const Matrix& P, const Row& q, const ColumnTypes& cols);

// Specialize-then-recurse helper. Builds the new column types and
// reconstructs the parent witness by re-wrapping the first `arity`
// columns of the child witness as a Ctor pattern.
UsefulResult usefulSpecialize(const Matrix& P, const Row& q,
                              const ColumnTypes& cols,
                              const std::string& ctorName,
                              const std::vector<TypePtr>& payload) {
    unsigned arity = static_cast<unsigned>(payload.size());
    Matrix Pspec = specialize(P, ctorName, arity);

    // qspec = (q[0]'s subs if q[0] is Ctor; else `arity` wildcards) ++ q[1..]
    Row qspec;
    qspec.reserve(arity + q.size() - 1);
    const PatView& q0 = *q[0];
    if (auto c = std::get_if<Ctor>(&q0.v)) {
        for (const auto& s : c->subs) qspec.push_back(s);
    } else {
        // q[0] is Wild — expand to `arity` wildcards.
        for (unsigned i = 0; i < arity; ++i) qspec.push_back(mkWild());
    }
    for (std::size_t i = 1; i < q.size(); ++i) qspec.push_back(q[i]);

    ColumnTypes colsSpec = specializeColumnTypes(cols, payload);
    UsefulResult sub = useful(Pspec, qspec, colsSpec);
    if (!sub.useful) return {false, {}};

    // Rebuild parent witness: wrap first `arity` of sub.witness as Ctor.
    Row out;
    std::vector<PatViewPtr> ctorSubs;
    ctorSubs.reserve(arity);
    for (unsigned i = 0; i < arity; ++i) ctorSubs.push_back(sub.witness[i]);
    out.push_back(mkCtor(ctorName, std::move(ctorSubs)));
    for (std::size_t i = arity; i < sub.witness.size(); ++i) {
        out.push_back(sub.witness[i]);
    }
    return {true, std::move(out)};
}

UsefulResult useful(const Matrix& P, const Row& q, const ColumnTypes& cols) {
    // Base case: zero columns.
    if (q.empty()) {
        if (P.empty()) return {true, {}};
        return {false, {}};
    }

    const PatView& q0 = *q[0];

    // q[0] is a concrete Ctor → specialize on its name.
    if (auto qc = std::get_if<Ctor>(&q0.v)) {
        // Determine payload arity by consulting the column type (if Enum)
        // or fall back to the subpattern count.
        TypePtr col0 = cols.empty() ? nullptr : resolve(cols[0]);
        std::vector<TypePtr> payload;
        if (col0 && col0->kind == TypeKind::Enum) {
            if (auto var = findVariant(col0, qc->name)) {
                payload = var->payloadTypes;
            } else {
                // Unknown ctor for this enum — treat as wildcard payload.
                payload.assign(qc->subs.size(), nullptr);
            }
        } else if (col0 && col0->kind == TypeKind::Tuple) {
            // Phase 36: the `(tuple)` ctor's payload types ARE the tuple's
            // element types — using them (not nulls) keeps the recursive
            // `useful` columns typed (a null column type crashes resolve()).
            payload = col0->tupleElems;
        } else {
            payload.assign(qc->subs.size(), nullptr);
        }
        return usefulSpecialize(P, q, cols, qc->name, payload);
    }

    // q[0] is a Lit → specialize on that value.
    if (auto ql = std::get_if<Lit>(&q0.v)) {
        Matrix Pspec = specializeLit(P, ql->value);
        Row qspec(q.begin() + 1, q.end());
        ColumnTypes colsSpec = dropFirst(cols);
        UsefulResult sub = useful(Pspec, qspec, colsSpec);
        if (!sub.useful) return {false, {}};
        Row out;
        out.reserve(sub.witness.size() + 1);
        out.push_back(mkLit(ql->value));
        for (const auto& w : sub.witness) out.push_back(w);
        return {true, std::move(out)};
    }

    // q[0] is Wild — the interesting case.
    HeadCtors heads = collectHeads(P);

    // Compute the constructor signature for the column-0 type.
    TypePtr col0 = cols.empty() ? nullptr : resolve(cols[0]);
    bool isEnum = col0 && col0->kind == TypeKind::Enum;

    if (isEnum) {
        const auto& variants = col0->enumVariants;
        // Check if signature is complete: every variant present in heads.
        bool complete = true;
        for (const auto& v : variants) {
            if (heads.ctorNames.count(v.name) == 0) { complete = false; break; }
        }
        if (complete && !variants.empty()) {
            // Drive every variant through specialize. If all are
            // non-useful, the match is exhaustive at this column.
            for (const auto& v : variants) {
                UsefulResult r = usefulSpecialize(P, q, cols, v.name,
                                                  v.payloadTypes);
                if (r.useful) return r;
            }
            return {false, {}};
        }
        // Incomplete signature → default matrix path. Witness head is
        // some variant not in heads (with all-wildcard payload).
        Matrix Pdef = defaultMatrix(P);
        Row qrest(q.begin() + 1, q.end());
        ColumnTypes colsRest = dropFirst(cols);
        UsefulResult sub = useful(Pdef, qrest, colsRest);
        if (!sub.useful) return {false, {}};
        // Pick the first missing variant.
        const EnumVariantType* missing = nullptr;
        for (const auto& v : variants) {
            if (heads.ctorNames.count(v.name) == 0) { missing = &v; break; }
        }
        Row out;
        if (missing) {
            std::vector<PatViewPtr> subs(missing->payloadTypes.size(),
                                         mkWild());
            out.push_back(mkCtor(missing->name, std::move(subs)));
        } else {
            // Empty enum (variants.empty()) — fall back to wildcard;
            // but an empty enum has no values, so this case is degenerate.
            out.push_back(mkWild());
        }
        for (const auto& w : sub.witness) out.push_back(w);
        return {true, std::move(out)};
    }

    // Phase 36: a tuple column has a SINGLE constructor `(tuple)` — a complete
    // signature of one. If it is present in the heads, drive it through
    // specialize (exhaustive iff the element sub-patterns are); otherwise the
    // missing witness is `(_, _, ...)`.
    if (col0 && col0->kind == TypeKind::Tuple) {
        unsigned arity = static_cast<unsigned>(col0->tupleElems.size());
        if (heads.ctorNames.count("(tuple)")) {
            return usefulSpecialize(P, q, cols, "(tuple)", col0->tupleElems);
        }
        Matrix Pdef = defaultMatrix(P);
        Row qrest(q.begin() + 1, q.end());
        ColumnTypes colsRest = dropFirst(cols);
        UsefulResult sub = useful(Pdef, qrest, colsRest);
        if (!sub.useful) return {false, {}};
        std::vector<PatViewPtr> subs(arity, mkWild());
        Row out;
        out.push_back(mkCtor("(tuple)", std::move(subs)));
        for (const auto& w : sub.witness) out.push_back(w);
        return {true, std::move(out)};
    }

    // Non-enum column (Int, Bool, Struct, etc.) → infinite signature.
    // Drive the default matrix; witness head is `_`.
    Matrix Pdef = defaultMatrix(P);
    Row qrest(q.begin() + 1, q.end());
    ColumnTypes colsRest = dropFirst(cols);
    UsefulResult sub = useful(Pdef, qrest, colsRest);
    if (!sub.useful) return {false, {}};
    Row out;
    out.reserve(sub.witness.size() + 1);
    out.push_back(mkWild());
    for (const auto& w : sub.witness) out.push_back(w);
    return {true, std::move(out)};
}

// -------------------- Decision-tree compilation --------------------

// Flat layout of an enum value is `{ i32 tag, var0_payload0,
// var0_payload1, ..., var1_payload0, ... }` (matching codegen's
// `enumPayloadIndices_`). For variant `v` of an enum E with variants
// `[V0(arity=a0), V1(arity=a1), ...]`, variant `v`'s payload position
// `k` lives at field index:
//   1 + sum(a0..a{v-1}) + k
// where the leading `1` accounts for the tag at field 0.
unsigned payloadFieldIndex(const TypePtr& enumType, unsigned variantIdx,
                           unsigned payloadPos) {
    TypePtr r = resolve(enumType);
    assert(r && r->kind == TypeKind::Enum);
    unsigned base = 1; // tag at field 0
    for (unsigned i = 0; i < variantIdx && i < r->enumVariants.size(); ++i) {
        base += static_cast<unsigned>(r->enumVariants[i].payloadTypes.size());
    }
    return base + payloadPos;
}

// Build a single-column matrix-row from one arm's pattern.
Row rowFromArm(
    const ast::MatchArm& a,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex) {
    Row r;
    r.push_back(view(*a.pattern, variantIndex));
    return r;
}

// A row of the working matrix that remembers which source arm it came
// from. The decision-tree compiler tracks this so a Leaf can name the
// arm index without recomputing it.
struct DTRow {
    Row pats;
    unsigned armIndex = 0;
};
using DTMatrix = std::vector<DTRow>;

// Specialize the DTMatrix on a constructor.
DTMatrix dtSpecialize(const DTMatrix& P, std::size_t col,
                      const std::string& ctorName, unsigned arity) {
    DTMatrix R;
    for (const auto& dr : P) {
        const Row& row = dr.pats;
        assert(col < row.size());
        const PatView& head = *row[col];
        if (auto c = std::get_if<Ctor>(&head.v)) {
            if (c->name == ctorName) {
                Row newRow;
                newRow.reserve(row.size() - 1 + arity);
                for (std::size_t i = 0; i < col; ++i) newRow.push_back(row[i]);
                for (const auto& s : c->subs) newRow.push_back(s);
                for (std::size_t i = col + 1; i < row.size(); ++i)
                    newRow.push_back(row[i]);
                R.push_back({std::move(newRow), dr.armIndex});
            }
        } else if (std::holds_alternative<Wild>(head.v)) {
            Row newRow;
            newRow.reserve(row.size() - 1 + arity);
            for (std::size_t i = 0; i < col; ++i) newRow.push_back(row[i]);
            for (unsigned k = 0; k < arity; ++k) newRow.push_back(mkWild());
            for (std::size_t i = col + 1; i < row.size(); ++i)
                newRow.push_back(row[i]);
            R.push_back({std::move(newRow), dr.armIndex});
        }
        // Lit head with a Ctor column would be ill-typed; skip.
    }
    return R;
}

// Specialize the DTMatrix on an integer literal at `col`. Literal column
// has no payload — drop it.
DTMatrix dtSpecializeLit(const DTMatrix& P, std::size_t col,
                         std::int64_t value) {
    DTMatrix R;
    for (const auto& dr : P) {
        const Row& row = dr.pats;
        assert(col < row.size());
        const PatView& head = *row[col];
        if (auto lit = std::get_if<Lit>(&head.v)) {
            if (lit->value == value) {
                Row newRow;
                newRow.reserve(row.size() - 1);
                for (std::size_t i = 0; i < row.size(); ++i)
                    if (i != col) newRow.push_back(row[i]);
                R.push_back({std::move(newRow), dr.armIndex});
            }
        } else if (std::holds_alternative<Wild>(head.v)) {
            Row newRow;
            newRow.reserve(row.size() - 1);
            for (std::size_t i = 0; i < row.size(); ++i)
                if (i != col) newRow.push_back(row[i]);
            R.push_back({std::move(newRow), dr.armIndex});
        }
        // Ctor head with a Lit column would be ill-typed; skip.
    }
    return R;
}

// Default matrix at column `col`: keep only rows whose head is wildcard,
// dropping the column.
DTMatrix dtDefaultMatrix(const DTMatrix& P, std::size_t col) {
    DTMatrix R;
    for (const auto& dr : P) {
        const Row& row = dr.pats;
        assert(col < row.size());
        if (std::holds_alternative<Wild>(row[col]->v)) {
            Row newRow;
            newRow.reserve(row.size() - 1);
            for (std::size_t i = 0; i < row.size(); ++i)
                if (i != col) newRow.push_back(row[i]);
            R.push_back({std::move(newRow), dr.armIndex});
        }
    }
    return R;
}

// Is every entry in `row` a Wild? (Note: Lit and Ctor are NOT wild.)
bool rowAllWild(const Row& row) {
    for (const auto& p : row) {
        if (!std::holds_alternative<Wild>(p->v)) return false;
    }
    return true;
}

// Pick the leftmost column whose head in the first row is non-wildcard.
// Precondition: first row is not all-wildcard.
std::size_t pickColumn(const Row& firstRow) {
    for (std::size_t i = 0; i < firstRow.size(); ++i) {
        if (!std::holds_alternative<Wild>(firstRow[i]->v)) return i;
    }
    // Should never reach here when row is not all-wild.
    return 0;
}

// Collect head constructor names from a column.
std::set<std::string> dtCollectCtors(const DTMatrix& P, std::size_t col) {
    std::set<std::string> s;
    for (const auto& dr : P) {
        if (auto c = std::get_if<Ctor>(&dr.pats[col]->v)) {
            s.insert(c->name);
        }
    }
    return s;
}

// Collect head literal values from a column, preserving source order
// (first appearance).
std::vector<std::int64_t> dtCollectLitsInOrder(const DTMatrix& P,
                                               std::size_t col) {
    std::vector<std::int64_t> vs;
    std::set<std::int64_t> seen;
    for (const auto& dr : P) {
        if (auto l = std::get_if<Lit>(&dr.pats[col]->v)) {
            if (seen.insert(l->value).second) vs.push_back(l->value);
        }
    }
    return vs;
}

std::unique_ptr<DecisionTree> dtCompile(
    const DTMatrix& P,
    const std::vector<std::vector<unsigned>>& occurrences,
    const ColumnTypes& cols,
    const std::unordered_map<std::string, TypePtr>& enums);

std::unique_ptr<DecisionTree> makeFail() {
    auto t = std::make_unique<DecisionTree>();
    t->kind = DecisionTree::Fail;
    return t;
}

std::unique_ptr<DecisionTree> makeLeaf(const DTRow& dr,
                                       const std::vector<std::vector<unsigned>>&
                                           occurrences) {
    auto t = std::make_unique<DecisionTree>();
    t->kind = DecisionTree::Leaf;
    t->armIndex = dr.armIndex;
    // Walk the row; for each Wild with a non-empty bindingName, record
    // (name, occurrence-path).
    assert(dr.pats.size() == occurrences.size());
    for (std::size_t i = 0; i < dr.pats.size(); ++i) {
        if (auto w = std::get_if<Wild>(&dr.pats[i]->v)) {
            if (!w->bindingName.empty()) {
                t->bindings.push_back({w->bindingName, occurrences[i]});
            }
        }
    }
    return t;
}

std::unique_ptr<DecisionTree> dtCompile(
    const DTMatrix& P,
    const std::vector<std::vector<unsigned>>& occurrences,
    const ColumnTypes& cols,
    const std::unordered_map<std::string, TypePtr>& enums) {
    if (P.empty()) {
        return makeFail();
    }
    const Row& firstRow = P[0].pats;
    if (rowAllWild(firstRow)) {
        return makeLeaf(P[0], occurrences);
    }

    // Pick a column to dispatch on.
    std::size_t col = pickColumn(firstRow);
    TypePtr colType = (col < cols.size()) ? resolve(cols[col]) : nullptr;

    // Phase 36: a tuple column is irrefutable (one shape) — destructure it in
    // place. Replace the column with its element occurrences/types and recurse
    // WITHOUT emitting a Switch (there is no tag to test); codegen reads the
    // elements by their field index along the occurrence path.
    if (colType && colType->kind == TypeKind::Tuple) {
        unsigned arity = static_cast<unsigned>(colType->tupleElems.size());
        DTMatrix Pspec = dtSpecialize(P, col, "(tuple)", arity);
        std::vector<std::vector<unsigned>> newOccs;
        newOccs.reserve(occurrences.size() - 1 + arity);
        for (std::size_t i = 0; i < col; ++i)
            newOccs.push_back(occurrences[i]);
        for (unsigned k = 0; k < arity; ++k) {
            std::vector<unsigned> path = occurrences[col];
            path.push_back(k); // tuple element k lives at LLVM field index k
            newOccs.push_back(std::move(path));
        }
        for (std::size_t i = col + 1; i < occurrences.size(); ++i)
            newOccs.push_back(occurrences[i]);
        ColumnTypes newCols;
        newCols.reserve(cols.size() - 1 + arity);
        for (std::size_t i = 0; i < col; ++i) newCols.push_back(cols[i]);
        for (const auto& et : colType->tupleElems) newCols.push_back(et);
        for (std::size_t i = col + 1; i < cols.size(); ++i)
            newCols.push_back(cols[i]);
        return dtCompile(Pspec, newOccs, newCols, enums);
    }

    if (colType && colType->kind == TypeKind::Enum) {
        auto tree = std::make_unique<DecisionTree>();
        tree->kind = DecisionTree::Switch;
        tree->occurrence = occurrences[col];

        const auto& variants = colType->enumVariants;
        std::set<std::string> heads = dtCollectCtors(P, col);
        bool complete = !variants.empty();
        for (const auto& v : variants) {
            if (heads.count(v.name) == 0) { complete = false; break; }
        }

        for (unsigned vi = 0; vi < variants.size(); ++vi) {
            const auto& v = variants[vi];
            unsigned arity = static_cast<unsigned>(v.payloadTypes.size());
            DTMatrix Pspec = dtSpecialize(P, col, v.name, arity);

            // New occurrences: drop col, insert `arity` payload occurrences
            // at that position.
            std::vector<std::vector<unsigned>> newOccs;
            newOccs.reserve(occurrences.size() - 1 + arity);
            for (std::size_t i = 0; i < col; ++i)
                newOccs.push_back(occurrences[i]);
            for (unsigned k = 0; k < arity; ++k) {
                std::vector<unsigned> path = occurrences[col];
                path.push_back(payloadFieldIndex(colType, vi, k));
                newOccs.push_back(std::move(path));
            }
            for (std::size_t i = col + 1; i < occurrences.size(); ++i)
                newOccs.push_back(occurrences[i]);

            // New column types: drop col, insert payload types.
            ColumnTypes newCols;
            newCols.reserve(cols.size() - 1 + arity);
            for (std::size_t i = 0; i < col; ++i) newCols.push_back(cols[i]);
            for (const auto& pt : v.payloadTypes) newCols.push_back(pt);
            for (std::size_t i = col + 1; i < cols.size(); ++i)
                newCols.push_back(cols[i]);

            DecisionTree::Case c;
            c.discKind = DecisionTree::Case::DiscCtor;
            c.ctorName = v.name;
            c.ctorTag = vi;
            c.child = dtCompile(Pspec, newOccs, newCols, enums);
            tree->cases.push_back(std::move(c));
        }
        if (complete) {
            tree->defaultCase = nullptr;
        } else {
            // Non-exhaustive enum — produce a Fail default for safety.
            tree->defaultCase = makeFail();
        }
        return tree;
    }

    if (colType && colType->kind == TypeKind::Int) {
        auto tree = std::make_unique<DecisionTree>();
        tree->kind = DecisionTree::Switch;
        tree->occurrence = occurrences[col];

        std::vector<std::int64_t> lits = dtCollectLitsInOrder(P, col);
        // Compute occurrences/cols once for the case-children (col is dropped).
        std::vector<std::vector<unsigned>> newOccs;
        newOccs.reserve(occurrences.size() - 1);
        for (std::size_t i = 0; i < occurrences.size(); ++i)
            if (i != col) newOccs.push_back(occurrences[i]);
        ColumnTypes newCols;
        newCols.reserve(cols.size() - 1);
        for (std::size_t i = 0; i < cols.size(); ++i)
            if (i != col) newCols.push_back(cols[i]);

        for (std::int64_t v : lits) {
            DTMatrix Pspec = dtSpecializeLit(P, col, v);
            DecisionTree::Case c;
            c.discKind = DecisionTree::Case::DiscLit;
            c.litValue = v;
            c.child = dtCompile(Pspec, newOccs, newCols, enums);
            tree->cases.push_back(std::move(c));
        }
        // Int columns always need a default.
        DTMatrix Pdef = dtDefaultMatrix(P, col);
        tree->defaultCase = dtCompile(Pdef, newOccs, newCols, enums);
        return tree;
    }

    // Non-enum / non-int column with non-wildcard heads — shouldn't happen
    // for V1, but be defensive.
    return makeFail();
}

} // namespace

std::optional<Witness> checkExhaustiveness(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& /*enums*/,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex) {
    // Build the one-column matrix of arm patterns.
    Matrix P;
    P.reserve(arms.size());
    for (const auto& arm : arms) {
        Row r;
        r.push_back(view(*arm.pattern, variantIndex));
        P.push_back(std::move(r));
    }

    Row q;
    q.push_back(mkWild());

    ColumnTypes cols;
    cols.push_back(scrutineeType);

    UsefulResult r = useful(P, q, cols);
    if (!r.useful) return std::nullopt;

    Witness w;
    assert(!r.witness.empty());
    w.text = render(*r.witness[0]);
    return w;
}

std::vector<unsigned> findRedundantArms(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& /*enums*/,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex) {
    std::vector<unsigned> result;
    // Build rows up-front so we can share state.
    std::vector<Row> rows;
    rows.reserve(arms.size());
    for (const auto& a : arms) {
        rows.push_back(rowFromArm(a, variantIndex));
    }

    // M[0..i-1] is the matrix of all preceding arms. Arm i is redundant
    // iff `useful(M, rows[i], cols)` is false.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        Matrix M;
        M.reserve(i);
        for (std::size_t j = 0; j < i; ++j) {
            M.push_back(rows[j]);
        }
        Row q = rows[i];
        ColumnTypes cols;
        cols.push_back(scrutineeType);
        UsefulResult r = useful(M, q, cols);
        if (!r.useful) {
            result.push_back(static_cast<unsigned>(i));
        }
    }
    return result;
}

std::unique_ptr<DecisionTree> compileDecisionTree(
    const TypePtr& scrutineeType,
    const std::vector<ast::MatchArm>& arms,
    const std::unordered_map<std::string, TypePtr>& enums,
    const std::unordered_map<std::string, std::pair<std::string, unsigned>>&
        variantIndex) {
    DTMatrix P;
    P.reserve(arms.size());
    for (std::size_t i = 0; i < arms.size(); ++i) {
        DTRow dr;
        dr.pats = rowFromArm(arms[i], variantIndex);
        dr.armIndex = static_cast<unsigned>(i);
        P.push_back(std::move(dr));
    }

    std::vector<std::vector<unsigned>> occurrences;
    occurrences.push_back({}); // root scrutinee
    ColumnTypes cols;
    cols.push_back(scrutineeType);

    return dtCompile(P, occurrences, cols, enums);
}

} // namespace kardashev::pattern_match
