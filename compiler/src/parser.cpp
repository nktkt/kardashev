#include "kardashev/parser.hpp"

#include "kardashev/ast_clone.hpp"
#include "kardashev/lexer.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace kardashev {
namespace {

class Parser {
public:
    // v24 Phase 134: filter `///` DocComment tokens out of the stream (so the
    // rest of the parser is untouched) into `docAt_`, keyed by the index of the
    // token that immediately follows the doc run. Decl parsers query it.
    explicit Parser(std::vector<Token> tokens) {
        std::string pending;
        for (auto& t : tokens) {
            if (t.kind == TokenKind::DocComment) {
                if (!pending.empty()) pending += "\n";
                pending += t.lexeme;
            } else {
                if (!pending.empty()) {
                    docAt_[tokens_.size()] = std::move(pending);
                    pending.clear();
                }
                tokens_.push_back(std::move(t));
            }
        }
    }

    ParseResult parseProgram() {
        ast::Program prog;
        while (!check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            // v24 Phase 134: grab the doc comment attached to this decl's first
            // token; the decl parser (fn/struct/enum) consumes pendingDeclDoc_.
            pendingDeclDoc_ = takeDocAt(pos_);
            // Phase 42: `#[derive(...)]` attributes precede a struct/enum; the
            // parsed list is stashed and consumed by the next struct/enum decl.
            if (check(TokenKind::Pound)) parseAttributes();
            if (check(TokenKind::KwFn) ||
                (peek().kind == TokenKind::Identifier &&
                 peek().lexeme == "async" &&
                 peek(1).kind == TokenKind::KwFn)) {
                prog.functions.push_back(parseFnDecl());
            } else if (check(TokenKind::KwStruct)) {
                prog.structs.push_back(parseStructDecl());
            } else if (check(TokenKind::KwEnum)) {
                prog.enums.push_back(parseEnumDecl());
            } else if (check(TokenKind::KwTrait)) {
                prog.traits.push_back(parseTraitDecl());
            } else if (check(TokenKind::KwImpl)) {
                prog.impls.push_back(parseImplDecl());
            } else if (check(TokenKind::KwMod)) {
                prog.mods.push_back(parseModDecl());
            } else if (check(TokenKind::KwExtern)) {
                parseExternDecls(prog);
            } else if (check(TokenKind::KwConst)) {
                // Phase 25: `const fn ...` is a function with the const
                // qualifier; `const NAME: T = ...;` is a const item. Peek the
                // token after `const` to disambiguate.
                if (peek(1).kind == TokenKind::KwFn) {
                    prog.functions.push_back(parseConstFnDecl());
                } else {
                    prog.consts.push_back(parseConstDecl());
                }
            } else if (check(TokenKind::Identifier) &&
                       peek().lexeme == "use") {
                // v26 Phase 146: a top-level import `use a::b::c;` /
                // `use a::b as d;` (the `pub use` re-export is handled in the
                // KwPub branch below).
                prog.uses.push_back(parseUseDecl());
            } else if (check(TokenKind::Identifier) &&
                       peek().lexeme == "type") {
                // v26 Phase 144: a top-level type alias `type Name = Target;`.
                consume(); // `type`
                Token aliasName = expect(TokenKind::Identifier,
                                         "type alias name after `type`");
                expect(TokenKind::Eq, "=");
                ast::TypeRef target = parseTypeRef();
                expect(TokenKind::Semi, ";");
                prog.typeAliases.emplace_back(aliasName.lexeme,
                                              std::move(target));
            } else if (check(TokenKind::KwPub)) {
                // Phase 7.3b: `pub` sticks to fn decls and is enforced on
                // path-qualified call sites. Bare-name calls keep working
                // under Phase 7.1's flat-merge — only `foo::bar()` form goes
                // through the visibility check. Phase 15: `pub` now also
                // sticks to struct / enum / trait / impl decls (stored on the
                // decl; type-level visibility enforcement is deferred — see
                // the report). Bare-name type references still resolve under
                // flat-merge, matching the fn behavior.
                advance();
                // v26 Phase 146: an optional visibility restriction
                // `pub(crate)` / `pub(super)` / `pub(self)` / `pub(in path)`.
                // `pub(self)` is private (Rust semantics); the rest are
                // path-reachable in this crate. See parsePubRestriction.
                bool pubReach = parsePubRestriction();
                if (check(TokenKind::KwFn) ||
                    (peek().kind == TokenKind::Identifier &&
                     peek().lexeme == "async" &&
                     peek(1).kind == TokenKind::KwFn)) {
                    auto fn = parseFnDecl();
                    fn.isPub = pubReach;
                    prog.functions.push_back(std::move(fn));
                } else if (check(TokenKind::KwStruct)) {
                    auto s = parseStructDecl();
                    s.isPub = pubReach;
                    prog.structs.push_back(std::move(s));
                } else if (check(TokenKind::KwEnum)) {
                    auto en = parseEnumDecl();
                    en.isPub = pubReach;
                    prog.enums.push_back(std::move(en));
                } else if (check(TokenKind::KwTrait)) {
                    auto tr = parseTraitDecl();
                    tr.isPub = pubReach;
                    prog.traits.push_back(std::move(tr));
                } else if (check(TokenKind::KwImpl)) {
                    auto im = parseImplDecl();
                    im.isPub = pubReach;
                    prog.impls.push_back(std::move(im));
                } else if (check(TokenKind::KwConst)) {
                    // Phase 25: `pub const fn` / `pub const NAME ...`.
                    if (peek(1).kind == TokenKind::KwFn) {
                        auto fn = parseConstFnDecl();
                        fn.isPub = pubReach;
                        prog.functions.push_back(std::move(fn));
                    } else {
                        auto c = parseConstDecl();
                        c.isPub = pubReach;
                        prog.consts.push_back(std::move(c));
                    }
                } else if (check(TokenKind::Identifier) &&
                           peek().lexeme == "use") {
                    // v26 Phase 146: `pub use path;` — a re-export. Parsed and
                    // recorded (with isReexport); see parseUseDecl.
                    auto u = parseUseDecl();
                    u.isReexport = true;
                    prog.uses.push_back(std::move(u));
                } else {
                    errorHere("`pub` must precede fn / struct / enum / "
                              "trait / impl / const / use");
                    advance();
                }
            } else {
                errorHere(std::string("expected 'fn', 'struct', 'enum', "
                                      "'trait', 'impl', 'mod' or 'extern' at "
                                      "top level, got ") +
                          std::string(tokenKindName(peek().kind)));
                advance();
            }
        }
        return {std::move(prog), std::move(errors_)};
    }

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    // v24 Phase 134: doc comments, keyed by following-token index; and the doc
    // for the declaration currently being parsed (consumed by the decl parser).
    std::map<std::size_t, std::string> docAt_;
    std::string pendingDeclDoc_;
    int structPatCounter_ = 0; // v26 Phase 142: fresh `__sp` names
    int fmtCounter_ = 0;       // v27 Phase 149: fresh `__fmt` names
    std::string takeDocAt(std::size_t p) {
        auto it = docAt_.find(p);
        if (it == docAt_.end()) return "";
        std::string d = std::move(it->second);
        docAt_.erase(it);
        return d;
    }
    std::vector<ParseError> errors_;
    bool restrictStructLit_ = false;
    // Phase 24: set by parseOptionalEffectRow when a `! { ... }` row was
    // actually present (even an empty `! { }`), so an extern decl can tell
    // "explicitly pure" (`! { }`) apart from "no row -> default io effect".
    bool sawEffectRow_ = false;

    const Token& peek(std::size_t offset = 0) const {
        std::size_t idx = pos_ + offset;
        if (idx >= tokens_.size()) return tokens_.back();
        return tokens_[idx];
    }

    void advance() {
        if (pos_ + 1 < tokens_.size()) ++pos_;
    }

    Token consume() {
        Token t = tokens_[pos_];
        advance();
        return t;
    }

    bool check(TokenKind kind) const { return peek().kind == kind; }

    bool accept(TokenKind kind) {
        if (check(kind)) {
            advance();
            return true;
        }
        return false;
    }

    Token expect(TokenKind kind, const char* what) {
        if (check(kind)) return consume();
        errorHere(std::string("expected ") + what + ", got " +
                  std::string(tokenKindName(peek().kind)));
        return peek(); // do not advance — let outer logic try to recover
    }

    void errorHere(std::string msg) {
        const Token& t = peek();
        errors_.push_back({std::move(msg), t.line, t.column});
    }

    void errorAt(std::string msg, std::size_t line, std::size_t column) {
        errors_.push_back({std::move(msg), line, column});
    }

    // Phase 64: parse an integer-literal lexeme that may carry an optional
    // width suffix (`5i32`) and/or a `0x`/`0b` radix prefix (`0xFF`, `0b1010`).
    // Returns the numeric value; `*outWidth`/`*outSigned` receive the suffix
    // (outWidth==0 when there is no suffix — an i64-default literal that
    // narrows in context). `tok` is only used for error reporting.
    long long parseIntLitLexeme(const Token& tok, int* outWidth,
                                bool* outSigned) {
        std::string lex = tok.lexeme;
        int width = 0;
        bool isSigned = true;
        // A width suffix is an `i`/`u` followed by 8/16/32/64. The parser
        // records it faithfully (signedness included); whether an *unsigned*
        // suffix is usable yet is a typecheck question (Phase 66 lands unsigned
        // integers — until then typecheck rejects a `u*` suffix honestly).
        static const struct { const char* s; int w; bool sg; } kSuffixes[] = {
            {"i8", 8, true},   {"i16", 16, true},  {"i32", 32, true},
            {"i64", 64, true}, {"u8", 8, false},   {"u16", 16, false},
            {"u32", 32, false}, {"u64", 64, false},
        };
        for (const auto& sx : kSuffixes) {
            std::string s = sx.s;
            if (lex.size() > s.size() &&
                lex.compare(lex.size() - s.size(), s.size(), s) == 0) {
                width = sx.w;
                isSigned = sx.sg;
                lex = lex.substr(0, lex.size() - s.size());
                break;
            }
        }
        int base = 10;
        if (lex.size() > 2 && lex[0] == '0' &&
            (lex[1] == 'x' || lex[1] == 'X')) {
            base = 16;
            lex = lex.substr(2);
        } else if (lex.size() > 2 && lex[0] == '0' &&
                   (lex[1] == 'b' || lex[1] == 'B')) {
            base = 2;
            lex = lex.substr(2);
        }
        long long value = 0;
        try {
            value = std::stoll(lex, nullptr, base);
        } catch (const std::out_of_range&) {
            // Phase 66: a value past i64::MAX is still valid for u64 (the FNV
            // offset basis `0xcbf29ce484222325`, etc.) — parse it as unsigned
            // and keep the 64-bit pattern (codegen emits it as a u64 constant).
            try {
                value = static_cast<long long>(std::stoull(lex, nullptr, base));
            } catch (const std::exception&) {
                errorHere("integer literal out of range: " + tok.lexeme);
                value = 0;
            }
        } catch (const std::exception&) {
            errorHere("integer literal out of range: " + tok.lexeme);
            value = 0;
        }
        if (outWidth) *outWidth = width;
        if (outSigned) *outSigned = isSigned;
        return value;
    }

    void fillIntLit(const Token& tok, ast::IntLitExpr& e) {
        int width = 0;
        bool isSigned = true;
        e.value = parseIntLitLexeme(tok, &width, &isSigned);
        e.suffixWidth = width;
        e.suffixSigned = isSigned;
    }

    // --- Top-level ---

    ast::ModDecl parseModDecl() {
        Token modTok = expect(TokenKind::KwMod, "mod");
        ast::ModDecl decl;
        decl.line = modTok.line;
        decl.column = modTok.column;
        Token nameTok = expect(TokenKind::Identifier, "module name");
        decl.name = nameTok.lexeme;
        expect(TokenKind::Semi, ";");
        return decl;
    }

    // Phase 24: `extern "C" ...` FFI declarations. Two accepted surface
    // forms (both supported here):
    //   per-decl:  extern "C" fn name(params) -> ret;
    //   block:     extern "C" { fn a(...) -> ...; fn b(...); }
    // The ABI string MUST be a string literal; only `"C"` is accepted by the
    // typechecker (the parser records whatever string was written and lets
    // the typechecker emit the precise error so the message carries the
    // offending ABI). Each parsed signature is appended to `prog.externFns`.
    void parseExternDecls(ast::Program& prog) {
        Token externTok = expect(TokenKind::KwExtern, "extern");
        std::string abi;
        if (check(TokenKind::StringLit)) {
            abi = consume().lexeme;
        } else {
            errorHere("expected an ABI string literal after `extern` "
                      "(e.g. `extern \"C\"`)");
            // Recover: keep abi empty so the typechecker reports the bad ABI.
        }
        if (accept(TokenKind::LBrace)) {
            // Block form: zero or more `fn ...;` signatures until `}`.
            while (!check(TokenKind::RBrace) &&
                   !check(TokenKind::EndOfInput)) {
                if (errors_.size() > 20) break;
                if (!check(TokenKind::KwFn)) {
                    errorHere("expected `fn` inside `extern` block");
                    advance();
                    continue;
                }
                prog.externFns.push_back(
                    parseExternFnSig(abi, externTok.line, externTok.column));
            }
            expect(TokenKind::RBrace, "}");
        } else {
            // Per-decl form: exactly one `fn ...;`.
            prog.externFns.push_back(
                parseExternFnSig(abi, externTok.line, externTok.column));
        }
    }

    // Parse a single bodyless `fn name(params) -> ret effect_row? ;` signature
    // for an extern block / per-decl form. Mirrors parseFnDecl's header but
    // requires a trailing `;` and forbids a `{ body }` / generics / `async`.
    ast::ExternFn parseExternFnSig(const std::string& abi, std::size_t line,
                                   std::size_t column) {
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::ExternFn ef;
        ef.abi = abi;
        ef.line = line;
        ef.column = column;
        Token nameTok = expect(TokenKind::Identifier, "function name");
        ef.name = nameTok.lexeme;
        if (check(TokenKind::Lt)) {
            errorAt("an `extern \"C\"` fn cannot be generic", nameTok.line,
                    nameTok.column);
            // Best-effort: skip a balanced <...> so the rest still parses.
            parseOptionalGenericParams();
        }
        expect(TokenKind::LParen, "(");
        if (!check(TokenKind::RParen)) {
            while (true) {
                ef.params.push_back(parseParam());
                if (!accept(TokenKind::Comma)) break;
            }
        }
        expect(TokenKind::RParen, ")");
        ef.returnType = parseOptionalReturnType();
        ef.effects = parseOptionalEffectRow();
        ef.hasExplicitEffects = !ef.effects.labels.empty() ||
                                 sawEffectRow_;
        if (check(TokenKind::LBrace)) {
            errorHere("an `extern \"C\"` fn is a declaration only and cannot "
                      "have a body");
            // Recover by skipping the block so following decls still parse.
            skipBalancedBraces();
        } else {
            expect(TokenKind::Semi, ";");
        }
        return ef;
    }

    // Skip a `{ ... }` region, matching nested braces, after an error so the
    // parser can resynchronize at the next top-level declaration.
    void skipBalancedBraces() {
        if (!accept(TokenKind::LBrace)) return;
        int depth = 1;
        while (depth > 0 && !check(TokenKind::EndOfInput)) {
            if (check(TokenKind::LBrace)) ++depth;
            else if (check(TokenKind::RBrace)) --depth;
            advance();
        }
    }

    ast::FnDecl parseFnDecl() {
        // `async fn` optional prefix marks the fn for the Phase-12
        // state-machine transform (codegen splits it into a resumable poll fn
        // over a heap frame); the typechecker implicitly adds `async` to the
        // effect row. `async` stays as an Identifier (not a keyword) so it can
        // also
        // appear in effect-row labels like `! { async }`; we detect it
        // by lexeme here.
        bool isAsync = false;
        if (peek().kind == TokenKind::Identifier &&
            peek().lexeme == "async" &&
            peek(1).kind == TokenKind::KwFn) {
            advance();
            isAsync = true;
        }
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::FnDecl decl;
        decl.doc = pendingDeclDoc_; // v24 Phase 134
        decl.isAsync = isAsync;
        decl.line = fnTok.line;
        decl.column = fnTok.column;

        Token nameTok = expect(TokenKind::Identifier, "function name");
        decl.name = nameTok.lexeme;

        // Generic params: `<T1, T2>`. The `<` here is unambiguous because it
        // sits between an identifier and `(` in fn-decl position — there's no
        // expression context to confuse with comparison.
        decl.genericParams = parseOptionalGenericParams();

        expect(TokenKind::LParen, "(");
        if (!check(TokenKind::RParen)) {
            while (true) {
                decl.params.push_back(parseParam());
                if (!accept(TokenKind::Comma)) break;
            }
        }
        expect(TokenKind::RParen, ")");
        decl.returnType = parseOptionalReturnType();
        decl.effects = parseOptionalEffectRow();
        // Phase 21b: a `where` clause desugars its constraints onto the generic
        // params we just parsed, so it behaves identically to the inline form.
        parseOptionalWhereClause(decl.genericParams);
        decl.body = parseBlockExpr();
        return decl;
    }

    // Phase 25: `const fn name(params) -> T { body }`. The `const` keyword is
    // consumed here; the rest is exactly a normal fn decl (so generics /
    // effect rows / where-clauses all still parse), with `isConst` set. A
    // const fn is also a normal runtime fn — the qualifier only marks it as
    // *eligible* for compile-time evaluation.
    ast::FnDecl parseConstFnDecl() {
        expect(TokenKind::KwConst, "const");
        ast::FnDecl decl = parseFnDecl();
        decl.isConst = true;
        return decl;
    }

    // Phase 25: a top-level `const NAME: T = <const-expr>;` item. The type
    // annotation is mandatory (a const carries an explicit type); the
    // initializer is any expression — the typechecker evaluates it at compile
    // time and rejects non-const-evaluable forms.
    ast::ConstDecl parseConstDecl() {
        Token constTok = expect(TokenKind::KwConst, "const");
        ast::ConstDecl decl;
        decl.line = constTok.line;
        decl.column = constTok.column;
        Token nameTok = expect(TokenKind::Identifier, "constant name");
        decl.name = nameTok.lexeme;
        expect(TokenKind::Colon, ": (a `const` requires a type annotation)");
        decl.type = parseTypeRef();
        expect(TokenKind::Eq, "= in const declaration");
        decl.value = parseExpr();
        expect(TokenKind::Semi, "; after const initializer");
        return decl;
    }

    // Phase 16: the return type is optional. `-> T` names an explicit return
    // type; omitting it (e.g. `fn drop(&mut self) { ... }`) means the function
    // returns `unit` — the natural spelling the `Drop` trait method uses. A
    // TypeRef with name "unit" resolves to the unit type in the typechecker.
    ast::TypeRef parseOptionalReturnType() {
        if (accept(TokenKind::Arrow)) return parseTypeRef();
        const Token& t = peek();
        ast::TypeRef tr;
        tr.name = "unit";
        tr.line = t.line;
        tr.column = t.column;
        return tr;
    }

    // Phase 4: parse an optional `! { e1, e2, ... }` effect row. Returns
    // an EffectRow with empty `labels` when the `!` is absent — meaning
    // the function is pure. Effect labels are stored as raw strings; the
    // typechecker validates them against the built-in set + any
    // declared effect-row variables.
    ast::EffectRow parseOptionalEffectRow() {
        ast::EffectRow row;
        sawEffectRow_ = false;
        if (!check(TokenKind::Bang)) return row;
        sawEffectRow_ = true;
        Token bangTok = consume();
        row.line = bangTok.line;
        row.column = bangTok.column;
        expect(TokenKind::LBrace, "{");
        if (!check(TokenKind::RBrace)) {
            while (true) {
                Token lbl = expect(TokenKind::Identifier, "effect label");
                row.labels.push_back(lbl.lexeme);
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::RBrace)) break; // trailing comma
            }
        }
        expect(TokenKind::RBrace, "}");
        return row;
    }

    ast::Param parseParam() {
        Token nameTok = expect(TokenKind::Identifier, "parameter name");
        expect(TokenKind::Colon, ":");
        return {nameTok.lexeme, parseTypeRef()};
    }

    ast::StructDecl parseStructDecl() {
        Token structTok = expect(TokenKind::KwStruct, "struct");
        ast::StructDecl decl;
        decl.doc = pendingDeclDoc_; // v24 Phase 134
        decl.line = structTok.line;
        decl.column = structTok.column;

        Token nameTok = expect(TokenKind::Identifier, "struct name");
        decl.name = nameTok.lexeme;
        decl.derives = std::move(pendingDerives_); // Phase 42
        pendingDerives_.clear();
        decl.genericParams = parseOptionalGenericParams();

        expect(TokenKind::LBrace, "{");
        if (!check(TokenKind::RBrace)) {
            while (true) {
                decl.fields.push_back(parseParam());
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::RBrace)) break; // trailing comma
            }
        }
        expect(TokenKind::RBrace, "}");
        return decl;
    }

    ast::EnumDecl parseEnumDecl() {
        Token enumTok = expect(TokenKind::KwEnum, "enum");
        ast::EnumDecl decl;
        decl.doc = pendingDeclDoc_; // v24 Phase 134
        decl.line = enumTok.line;
        decl.column = enumTok.column;

        Token nameTok = expect(TokenKind::Identifier, "enum name");
        decl.name = nameTok.lexeme;
        decl.derives = std::move(pendingDerives_); // Phase 42
        pendingDerives_.clear();
        decl.genericParams = parseOptionalGenericParams();

        expect(TokenKind::LBrace, "{");
        if (!check(TokenKind::RBrace)) {
            while (true) {
                decl.variants.push_back(parseEnumVariant());
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::RBrace)) break; // trailing comma
            }
        }
        expect(TokenKind::RBrace, "}");
        return decl;
    }

    ast::EnumVariant parseEnumVariant() {
        Token nameTok = expect(TokenKind::Identifier, "variant name");
        ast::EnumVariant v;
        v.name = nameTok.lexeme;
        v.line = nameTok.line;
        v.column = nameTok.column;
        if (accept(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                while (true) {
                    v.payloadTypes.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break; // trailing comma
                }
            }
            expect(TokenKind::RParen, ")");
        }
        return v;
    }

    // Phase 7.2 helper: consume `foo::bar::baz` and return just the last
    // segment as the name. Phase 7.1 flat-merges modules into a single
    // namespace, so a path qualifier is currently informational only —
    // it documents intent without changing resolution. When modules
    // gain real namespacing the path can be plumbed through unchanged.
    Token consumePathName(const char* what) {
        Token first = expect(TokenKind::Identifier, what);
        Token last = first;
        while (check(TokenKind::DoubleColon)) {
            consume();
            last = expect(TokenKind::Identifier, "identifier after `::`");
        }
        return last;
    }

    // Phase 22: wrap a parsed inner type in a reference, for `&(T)`. Nested
    // references aren't on the grammar, so a simple flag set on the inner node
    // suffices (the typechecker / codegen ref-peel reads isRef + refIsMut).
    ast::TypeRef refWrapTypeRef(ast::TypeRef inner, bool isMut,
                               const Token& ampTok) {
        inner.isRef = true;
        inner.refIsMut = isMut;
        inner.line = ampTok.line;
        inner.column = ampTok.column;
        return inner;
    }

    // v26 Phase 146: parse an optional visibility restriction after `pub` —
    // `pub(crate)` / `pub(super)` / `pub(self)` / `pub(in a::b)`. Returns
    // whether the item is reachable via path-qualified syntax in THIS crate:
    // `pub`, `pub(crate)`, `pub(super)` and `pub(in ...)` are reachable;
    // `pub(self)` is equivalent to private (Rust semantics) and is not. The
    // crate/super/in distinctions collapse to "reachable within the crate"
    // under the single-crate flat-merge model — the cross-crate boundary they
    // gate doesn't exist yet — but the syntax is fully parsed and validated.
    bool parsePubRestriction() {
        if (!check(TokenKind::LParen)) return true; // bare `pub`
        consume(); // (
        bool reachable = true;
        if (check(TokenKind::Identifier) && peek().lexeme == "self") {
            consume();
            reachable = false; // pub(self) == private
        } else if (check(TokenKind::Identifier) &&
                   (peek().lexeme == "crate" || peek().lexeme == "super")) {
            consume();
        } else if (check(TokenKind::Identifier) && peek().lexeme == "in") {
            consume(); // in
            expect(TokenKind::Identifier, "module path after `pub(in`");
            while (accept(TokenKind::DoubleColon))
                expect(TokenKind::Identifier, "path segment after `::`");
        } else {
            errorHere("expected `crate`, `super`, `self`, or `in <path>` "
                      "inside `pub(...)`");
        }
        expect(TokenKind::RParen, ")");
        return reachable;
    }

    // v26 Phase 146: parse a `use a::b::c;` import or `use a::b as d;` alias.
    // The leading `use` (an identifier, not a keyword) is consumed here.
    ast::UseDecl parseUseDecl() {
        Token useTok = consume(); // `use`
        ast::UseDecl u;
        u.line = useTok.line;
        u.column = useTok.column;
        u.path.push_back(
            expect(TokenKind::Identifier, "path segment after `use`").lexeme);
        while (accept(TokenKind::DoubleColon))
            u.path.push_back(
                expect(TokenKind::Identifier, "path segment after `::`").lexeme);
        // `as` is the cast keyword (KwAs), reused here for the rename clause.
        if (accept(TokenKind::KwAs)) {
            u.alias =
                expect(TokenKind::Identifier, "alias name after `as`").lexeme;
        }
        expect(TokenKind::Semi, "; after `use` import");
        return u;
    }

    // v27 Phase 149: desugar `format!("...{}...", a, b)` / `print!` / `println!`
    // to string-building. The first argument MUST be a string literal so the
    // format string is split at compile time into literal segments + `{}`
    // holes; `{{`/`}}` are escaped literal braces. Each `{}` hole pairs with the
    // next argument via `arg.to_string()` (the `Display` trait). `{:?}` (Debug)
    // arrives in Phase 150. The whole thing lowers to:
    //   { let mut __fmtN = string_new();
    //     string_push_str(&mut __fmtN, "<seg>"); string_push_str(&mut __fmtN, (a).to_string());
    //     ... <tail> }
    // where the tail is `__fmtN` for `format!`, `println(&__fmtN)` for
    // `println!`, and `print_no_nl(&__fmtN)` for `print!`.
    ast::ExprPtr parseFormatMacro(const Token& nameTok) {
        consume(); // `!`
        expect(TokenKind::LParen, "(");
        if (!check(TokenKind::StringLit)) {
            errorAt("the first argument to `" + nameTok.lexeme +
                        "!` must be a string literal",
                    nameTok.line, nameTok.column);
            while (!check(TokenKind::RParen) && !check(TokenKind::EndOfInput))
                advance();
            accept(TokenKind::RParen);
            auto e = std::make_unique<ast::StringLitExpr>();
            e->line = nameTok.line;
            e->column = nameTok.column;
            return e;
        }
        Token fmtTok = consume();
        const std::string& fmt = fmtTok.lexeme;
        std::vector<ast::ExprPtr> args;
        while (accept(TokenKind::Comma)) {
            if (check(TokenKind::RParen)) break; // trailing comma
            bool prev = restrictStructLit_;
            restrictStructLit_ = false;
            args.push_back(parseExpr());
            restrictStructLit_ = prev;
        }
        expect(TokenKind::RParen, ")");

        // Split into literal segments separated by holes. A `{}` hole formats
        // via Display (`to_string`); a `{:?}` hole via Debug (`fmt_debug`).
        std::vector<std::string> segs;
        segs.push_back("");
        std::vector<bool> holeDebug; // per hole: true = `{:?}` (Debug)
        std::size_t holeCount = 0;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            char c = fmt[i];
            if (c == '{') {
                if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                    segs.back().push_back('{');
                    ++i;
                } else if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                    ++i;
                    ++holeCount;
                    holeDebug.push_back(false);
                    segs.push_back("");
                } else if (i + 3 < fmt.size() && fmt[i + 1] == ':' &&
                           fmt[i + 2] == '?' && fmt[i + 3] == '}') {
                    // `{:?}` — Debug.
                    i += 3;
                    ++holeCount;
                    holeDebug.push_back(true);
                    segs.push_back("");
                } else {
                    errorAt("unsupported format placeholder (only `{}` and "
                            "`{:?}` are supported)",
                            fmtTok.line, fmtTok.column);
                }
            } else if (c == '}') {
                if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                    segs.back().push_back('}');
                    ++i;
                } else {
                    errorAt("unmatched `}` in format string (use `}}` for a "
                            "literal brace)",
                            fmtTok.line, fmtTok.column);
                }
            } else {
                segs.back().push_back(c);
            }
        }
        if (holeCount != args.size()) {
            errorAt("`" + nameTok.lexeme + "!` has " +
                        std::to_string(holeCount) + " placeholder(s) but " +
                        std::to_string(args.size()) + " argument(s)",
                    nameTok.line, nameTok.column);
        }

        auto block = std::make_unique<ast::BlockExpr>();
        block->line = nameTok.line;
        block->column = nameTok.column;
        const std::string tmp = "__fmt" + std::to_string(fmtCounter_++);
        {
            auto let = std::make_unique<ast::LetStmt>();
            let->name = tmp;
            let->isMut = true;
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = "string_new";
            let->value = std::move(call);
            block->stmts.push_back(std::move(let));
        }
        auto pushSlot = [&](ast::ExprPtr valueExpr) {
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = "string_push_str";
            auto ref = std::make_unique<ast::RefExpr>();
            ref->isMut = true;
            auto id = std::make_unique<ast::IdentExpr>();
            id->name = tmp;
            ref->operand = std::move(id);
            call->args.push_back(std::move(ref));
            call->args.push_back(std::move(valueExpr));
            auto es = std::make_unique<ast::ExprStmt>();
            es->expr = std::move(call);
            block->stmts.push_back(std::move(es));
        };
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (!segs[i].empty()) {
                auto lit = std::make_unique<ast::StringLitExpr>();
                lit->value = segs[i];
                pushSlot(std::move(lit));
            }
            if (i < args.size()) {
                auto mc = std::make_unique<ast::MethodCallExpr>();
                mc->methodName =
                    (i < holeDebug.size() && holeDebug[i]) ? "fmt_debug"
                                                           : "to_string";
                mc->receiver = std::move(args[i]);
                pushSlot(std::move(mc));
            }
        }
        if (nameTok.lexeme == "format") {
            auto id = std::make_unique<ast::IdentExpr>();
            id->name = tmp;
            block->tail = std::move(id);
        } else {
            auto call = std::make_unique<ast::CallExpr>();
            call->callee =
                (nameTok.lexeme == "println") ? "println" : "print_no_nl";
            auto ref = std::make_unique<ast::RefExpr>();
            ref->isMut = false;
            auto id = std::make_unique<ast::IdentExpr>();
            id->name = tmp;
            ref->operand = std::move(id);
            call->args.push_back(std::move(ref));
            block->tail = std::move(call);
        }
        return block;
    }

    ast::TypeRef parseTypeRef() {
        // Reference prefix: `&` or `&mut` wraps the rest of the type.
        // Phase 2.4b: shared `&T`. Phase 2.4c: `&mut T`. Currently we
        // recognize `mut` by lexeme (not a keyword) so the AST is forward-
        // compatible.
        bool isRef = false;
        bool refIsMut = false;
        Token ampTok{TokenKind::Identifier, "", 0, 0};
        if (check(TokenKind::Ampersand)) {
            ampTok = consume();
            isRef = true;
            if (check(TokenKind::Identifier) && peek().lexeme == "mut") {
                consume();
                refIsMut = true;
            }
        }
        // Phase 13b: slice type `&[T]` — only valid behind `&`. The element
        // type is parsed between the brackets; the whole thing becomes a
        // TypeRef with `isSlice` set and `typeArgs[0]` the element.
        if (isRef && check(TokenKind::LBracket)) {
            consume(); // [
            ast::TypeRef tr;
            tr.isSlice = true;
            tr.isRef = true;
            tr.refIsMut = refIsMut;
            tr.line = ampTok.line;
            tr.column = ampTok.column;
            tr.typeArgs.push_back(parseTypeRef());
            expect(TokenKind::RBracket, "]");
            return tr;
        }
        // Phase 22 / 25: a fixed-size array type `[T; N]` (no leading `&` —
        // that is the slice form above). N may be any compile-time-constant
        // expression: a bare integer literal (Phase 22), a `const` item, a
        // `const fn` call, or an arithmetic expr over them (Phase 25).
        // `&[T; N]` (a reference to an array) wraps the array in a Ref.
        if (check(TokenKind::LBracket)) {
            Token lb = consume(); // [
            ast::TypeRef tr;
            tr.isArray = true;
            tr.line = isRef ? ampTok.line : lb.line;
            tr.column = isRef ? ampTok.column : lb.column;
            tr.typeArgs.push_back(parseTypeRef());
            expect(TokenKind::Semi, "; in array type [T; N]");
            // Parse the length as a full const-expr. To keep the Phase 22
            // literal path byte-identical, a bare non-negative integer literal
            // is folded straight into `arrayLen` (arrayLenExpr stays null);
            // any other expression is stashed in `arrayLenExpr` for the
            // typechecker to const-evaluate.
            ast::ExprPtr lenExpr = parseExpr();
            if (auto* lit = dynamic_cast<ast::IntLitExpr*>(lenExpr.get());
                lit && lit->value >= 0) {
                tr.arrayLen = static_cast<std::size_t>(lit->value);
            } else {
                tr.arrayLenExpr = std::shared_ptr<ast::Expr>(lenExpr.release());
            }
            expect(TokenKind::RBracket, "]");
            if (isRef) {
                tr.isRef = true;
                tr.refIsMut = refIsMut;
            }
            return tr;
        }
        // Phase 22: a tuple type `(A, B, ...)`. The empty `()` is the unit type
        // (the parser synthesizes the same "unit" TypeRef the no-return case
        // uses); a single `(T)` is just `T` (parenthesized). Two-or-more comma-
        // separated types form a tuple. A `&(A, B)` wraps the tuple in a Ref.
        if (check(TokenKind::LParen)) {
            Token lp = consume(); // (
            std::size_t baseLine = isRef ? ampTok.line : lp.line;
            std::size_t baseCol = isRef ? ampTok.column : lp.column;
            // `()` — unit.
            if (check(TokenKind::RParen)) {
                consume();
                ast::TypeRef tr;
                tr.name = "unit";
                tr.line = baseLine;
                tr.column = baseCol;
                if (isRef) { tr.isRef = true; tr.refIsMut = refIsMut; }
                return tr;
            }
            std::vector<ast::TypeRef> elems;
            elems.push_back(parseTypeRef());
            bool sawComma = false;
            while (accept(TokenKind::Comma)) {
                sawComma = true;
                if (check(TokenKind::RParen)) break; // trailing comma
                elems.push_back(parseTypeRef());
            }
            expect(TokenKind::RParen, ")");
            if (!sawComma) {
                // `(T)` — a parenthesized single type, not a tuple.
                ast::TypeRef inner = std::move(elems[0]);
                if (isRef) {
                    return refWrapTypeRef(std::move(inner), refIsMut, ampTok);
                }
                return inner;
            }
            ast::TypeRef tr;
            tr.isTuple = true;
            tr.tupleElems = std::move(elems);
            tr.line = baseLine;
            tr.column = baseCol;
            if (isRef) { tr.isRef = true; tr.refIsMut = refIsMut; }
            return tr;
        }
        // Phase 10a: function type in type position —
        // `fn(T1, T2) -> Ret ! { effects }`. The trailing effect row is
        // optional (absent = pure) and reuses the same parser as fn decls.
        if (check(TokenKind::KwFn)) {
            Token fnTok = consume();
            ast::TypeRef tr;
            tr.isFn = true;
            tr.isRef = isRef;
            tr.refIsMut = refIsMut;
            tr.line = isRef ? ampTok.line : fnTok.line;
            tr.column = isRef ? ampTok.column : fnTok.column;
            expect(TokenKind::LParen, "(");
            if (!check(TokenKind::RParen)) {
                while (true) {
                    tr.fnParams.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break; // trailing comma
                }
            }
            expect(TokenKind::RParen, ")");
            expect(TokenKind::Arrow, "->");
            tr.fnRet = std::make_shared<ast::TypeRef>(parseTypeRef());
            ast::EffectRow row = parseOptionalEffectRow();
            tr.fnEffects = std::move(row.labels);
            return tr;
        }
        // Phase 145: a closure-trait bound in type position — `Fn(A) -> R`,
        // `FnMut(A) -> R`, `FnOnce(A) -> R`. Parsed exactly like the bare
        // `fn(..)` form above (same fields, same fat-pointer ABI) but tagged
        // with the required kind rank in `closureBound`. Only fires when the
        // identifier is immediately followed by `(` so a plain type named `Fn`
        // (unlikely, but harmless) still parses as a nominal type.
        if (check(TokenKind::Identifier) &&
            (peek().lexeme == "Fn" || peek().lexeme == "FnMut" ||
             peek().lexeme == "FnOnce") &&
            peek(1).kind == TokenKind::LParen) {
            Token kindTok = consume();
            ast::TypeRef tr;
            tr.isFn = true;
            tr.closureBound = kindTok.lexeme == "Fn"     ? 0
                              : kindTok.lexeme == "FnMut" ? 1
                                                          : 2;
            tr.isRef = isRef;
            tr.refIsMut = refIsMut;
            tr.line = isRef ? ampTok.line : kindTok.line;
            tr.column = isRef ? ampTok.column : kindTok.column;
            expect(TokenKind::LParen, "(");
            if (!check(TokenKind::RParen)) {
                while (true) {
                    tr.fnParams.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break; // trailing comma
                }
            }
            expect(TokenKind::RParen, ")");
            // The return type is optional: `Fn(A)` (no `-> R`) means `-> ()`,
            // matching how a unit-returning callable reads.
            if (accept(TokenKind::Arrow)) {
                tr.fnRet = std::make_shared<ast::TypeRef>(parseTypeRef());
            } else {
                ast::TypeRef unitRet;
                unitRet.name = "unit";
                unitRet.line = kindTok.line;
                unitRet.column = kindTok.column;
                tr.fnRet = std::make_shared<ast::TypeRef>(std::move(unitRet));
            }
            ast::EffectRow row = parseOptionalEffectRow();
            tr.fnEffects = std::move(row.labels);
            return tr;
        }
        // Phase 11: `dyn Trait` — an unsized trait object. `dyn` stays an
        // Identifier at the lexer level (like `mut`/`async`), so match by
        // lexeme. The trait name follows; combine with the `&` prefix above
        // for `&dyn Trait`, or nest under `Box<...>` for `Box<dyn Trait>`.
        if (check(TokenKind::Identifier) && peek().lexeme == "dyn") {
            consume();
            Token traitTok = expect(TokenKind::Identifier, "trait name after `dyn`");
            ast::TypeRef tr;
            tr.isDyn = true;
            tr.name = traitTok.lexeme;
            tr.isRef = isRef;
            tr.refIsMut = refIsMut;
            tr.line = isRef ? ampTok.line : traitTok.line;
            tr.column = isRef ? ampTok.column : traitTok.column;
            // Phase 49: a parameterized trait object `dyn Trait<Args>` (e.g.
            // `dyn Producer<i64>`). The trait's type args follow, mirroring the
            // generic-type parse below; they pin the trait's params for the
            // object's method signatures (the return/param types in the vtable
            // thunks).
            if (accept(TokenKind::Lt)) {
                if (!check(TokenKind::Gt)) {
                    while (true) {
                        tr.typeArgs.push_back(parseTypeRef());
                        if (!accept(TokenKind::Comma)) break;
                        if (check(TokenKind::Gt)) break;
                    }
                }
                expect(TokenKind::Gt, ">");
            }
            return tr;
        }
        Token t = expect(TokenKind::Identifier, "type name");
        ast::TypeRef tr;
        tr.name = t.lexeme;
        tr.isRef = isRef;
        tr.refIsMut = refIsMut;
        tr.line = isRef ? ampTok.line : t.line;
        tr.column = isRef ? ampTok.column : t.column;
        // Phase 21b: an associated-type projection `Base::Assoc` in type
        // position (e.g. `Self::Item`, `C::Item`). A single `::` segment names
        // the projected associated type; `name` keeps the base. (Type position
        // has no module-qualified-type form today, so a `::` here is
        // unambiguously a projection. Chains `A::B::C` aren't supported.)
        if (check(TokenKind::DoubleColon)) {
            consume();
            Token assocTok = expect(TokenKind::Identifier,
                                    "associated type name after `::`");
            tr.assocName = assocTok.lexeme;
            // v28 Phase 155 (GATs): type arguments on the projection —
            // `Self::Out<i64>`. Consumed here (into `assocTypeArgs`) so the
            // general type-arg parser below doesn't also claim them.
            if (accept(TokenKind::Lt)) {
                if (!check(TokenKind::Gt)) {
                    while (true) {
                        tr.assocTypeArgs.push_back(parseTypeRef());
                        if (!accept(TokenKind::Comma)) break;
                        if (check(TokenKind::Gt)) break; // trailing comma
                    }
                }
                expect(TokenKind::Gt, "> (after associated-type arguments)");
            }
            return tr;
        }
        // Optional type-args: `Name<T1, T2>`. Position is unambiguous because
        // `<` immediately after an Ident in a type-ref slot can only be the
        // start of a type-arg list (the alternative — comparison — never
        // appears in a type-annotation slot, which is always preceded by
        // `:` / `->` / `(` / `,`).
        if (accept(TokenKind::Lt)) {
            if (!check(TokenKind::Gt)) {
                while (true) {
                    // Phase 58 (v10): a const-generic VALUE argument — the `3`
                    // in `Mat<3>`. An integer literal in type-arg position is
                    // unambiguously a const value (a type never starts with a
                    // digit). It binds to the type's `const N` parameter.
                    if (check(TokenKind::Integer)) {
                        Token n = consume();
                        ast::TypeRef carg;
                        carg.isConstArg = true;
                        carg.line = n.line;
                        carg.column = n.column;
                        try {
                            carg.constArgValue = std::stoll(n.lexeme);
                        } catch (const std::exception&) {
                            errorHere("const generic argument out of range: " +
                                      n.lexeme);
                            carg.constArgValue = 0;
                        }
                        tr.typeArgs.push_back(std::move(carg));
                    } else if (check(TokenKind::KwTrue) ||
                               check(TokenKind::KwFalse)) {
                        // v28 Phase 153: a `bool` const-generic argument.
                        Token b = consume();
                        ast::TypeRef carg;
                        carg.isConstArg = true;
                        carg.constArgTypeName = "bool";
                        carg.constArgValue = (b.kind == TokenKind::KwTrue) ? 1 : 0;
                        carg.line = b.line;
                        carg.column = b.column;
                        tr.typeArgs.push_back(std::move(carg));
                    } else if (check(TokenKind::CharLit)) {
                        // v28 Phase 153: a `char` const-generic argument.
                        Token ch = consume();
                        ast::TypeRef carg;
                        carg.isConstArg = true;
                        carg.constArgTypeName = "char";
                        carg.constArgValue =
                            static_cast<long long>(std::stoul(ch.lexeme));
                        carg.line = ch.line;
                        carg.column = ch.column;
                        tr.typeArgs.push_back(std::move(carg));
                    } else {
                        tr.typeArgs.push_back(parseTypeRef());
                    }
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::Gt)) break; // trailing comma
                }
            }
            expect(TokenKind::Gt, ">");
        }
        return tr;
    }

    // Parse a single trait bound `Bound` or `Bound<Args...>` (the right side
    // of a `T: ...` constraint) into the given TypeParam's `bound` /
    // `boundTypeArgs`. Shared by inline generic params and `where` clauses so
    // a `where T: Bound<U>` desugars to exactly the inline `<T: Bound<U>>`
    // form (Phase 21b).
    void parseTraitBoundInto(ast::TypeParam& tp) {
        Token boundTok = expect(TokenKind::Identifier, "trait name after ':'");
        tp.bound = boundTok.lexeme;
        // Phase 21a: a parameterized trait bound `I: Iterator<T>`. The trait's
        // type args follow the bound name; they typically name another generic
        // param of this same decl.
        if (accept(TokenKind::Lt)) {
            if (!check(TokenKind::Gt)) {
                while (true) {
                    tp.boundTypeArgs.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::Gt)) break; // trailing ,
                }
            }
            expect(TokenKind::Gt, ">");
        }
        // Phase 28: additional bounds `T: A + B + C`. Each extra bound is a
        // trait name; an optional `<...>` arg list is accepted and discarded
        // (extra bounds dispatch by trait name — the practical multi-bound
        // case, Hash + Eq, is non-generic).
        while (accept(TokenKind::Plus)) {
            Token extraTok = expect(TokenKind::Identifier,
                                    "trait name after '+'");
            tp.extraBounds.push_back(extraTok.lexeme);
            if (accept(TokenKind::Lt)) {
                if (!check(TokenKind::Gt)) {
                    while (true) {
                        parseTypeRef(); // discard a parameterized extra bound
                        if (!accept(TokenKind::Comma)) break;
                        if (check(TokenKind::Gt)) break; // trailing ,
                    }
                }
                expect(TokenKind::Gt, ">");
            }
        }
    }

    // Helper for fn/struct/enum decls: parse optional `<T1, T2: Bound>`
    // generic params after the type name. The optional `: TraitName`
    // single-trait bound lands in TypeParam.bound; multiple bounds (`T:
    // A + B`) aren't yet in the grammar.
    std::vector<ast::TypeParam> parseOptionalGenericParams() {
        std::vector<ast::TypeParam> result;
        if (!accept(TokenKind::Lt)) return result;
        if (!check(TokenKind::Gt)) {
            while (true) {
                ast::TypeParam tp;
                if (check(TokenKind::KwConst)) {
                    // Phase 57: a const-generic parameter `const N: i64`.
                    Token kw = consume();
                    Token nameTok = expect(TokenKind::Identifier,
                                           "const-generic parameter name "
                                           "after `const`");
                    tp.isConst = true;
                    tp.name = nameTok.lexeme;
                    tp.line = kw.line;
                    tp.column = kw.column;
                    expect(TokenKind::Colon,
                           ": (a const-generic param needs a type, "
                           "e.g. `const N: i64`)");
                    Token tyTok =
                        expect(TokenKind::Identifier, "const-generic param type");
                    // v28 Phase 153: a const-generic param may be `i64`, `bool`,
                    // or `char` (its value is still monomorphized by value).
                    if (tyTok.lexeme != "i64" && tyTok.lexeme != "bool" &&
                        tyTok.lexeme != "char") {
                        errorAt("const generic parameter `" + tp.name +
                                    "` must be `i64`, `bool`, or `char`, got `" +
                                    tyTok.lexeme + "`",
                                tyTok.line, tyTok.column);
                    }
                    tp.constTypeName = tyTok.lexeme;
                } else {
                    Token tpTok = expect(TokenKind::Identifier,
                                         "generic type parameter name");
                    tp.name = tpTok.lexeme;
                    tp.line = tpTok.line;
                    tp.column = tpTok.column;
                    if (accept(TokenKind::Colon)) {
                        parseTraitBoundInto(tp);
                    }
                }
                result.push_back(std::move(tp));
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::Gt)) break; // trailing comma
            }
        }
        expect(TokenKind::Gt, ">");
        return result;
    }

    // Phase 21b: parse an optional `where` clause and DESUGAR each constraint
    // onto the matching generic param's bound list, so a `where`-bounded fn is
    // byte-for-byte identical to the inline-bounded form downstream. The clause
    // sits after a fn's return type / effect row and before the body `{`:
    //
    //   fn f<T, U>(...) -> R where T: Show, U: Iterator<T> { ... }
    //
    // `where` is an Identifier at the lexer level (like `mut`/`dyn`/`self`), so
    // we detect it by lexeme. Each constraint is `Param: Bound` /
    // `Param: Bound<Args>`; `Param` must name one of `params`, else it's an
    // error. A param may carry at most one bound (single-bound limit, matching
    // the inline grammar); a second constraint on the same param is rejected.
    void parseOptionalWhereClause(std::vector<ast::TypeParam>& params) {
        if (!(check(TokenKind::Identifier) && peek().lexeme == "where")) return;
        consume(); // `where`
        while (true) {
            Token nameTok = expect(TokenKind::Identifier,
                                   "generic parameter name in `where` clause");
            // Find the matching generic param.
            ast::TypeParam* target = nullptr;
            for (auto& p : params) {
                if (p.name == nameTok.lexeme) { target = &p; break; }
            }
            expect(TokenKind::Colon, ":");
            if (target == nullptr) {
                errorAt(std::string("`where` clause names unknown generic "
                                    "parameter '") +
                            nameTok.lexeme + "'",
                        nameTok.line, nameTok.column);
                // Still consume the bound so parsing can continue.
                ast::TypeParam scratch;
                parseTraitBoundInto(scratch);
            } else if (!target->bound.empty()) {
                // Phase 28: a second `where` constraint on the same param
                // accumulates as an extra bound, so `where T: A, T: B` is
                // equivalent to the inline `T: A + B`.
                ast::TypeParam tmp;
                parseTraitBoundInto(tmp);
                target->extraBounds.push_back(tmp.bound);
                for (const auto& eb : tmp.extraBounds)
                    target->extraBounds.push_back(eb);
            } else {
                parseTraitBoundInto(*target);
            }
            if (!accept(TokenKind::Comma)) break;
            // A trailing comma right before the body `{` ends the clause.
            if (check(TokenKind::LBrace)) break;
        }
    }

    ast::TraitDecl parseTraitDecl() {
        Token traitTok = expect(TokenKind::KwTrait, "trait");
        ast::TraitDecl decl;
        decl.line = traitTok.line;
        decl.column = traitTok.column;
        Token nameTok = expect(TokenKind::Identifier, "trait name");
        decl.name = nameTok.lexeme;
        // Phase 21a: trait type params `trait Name<T0, T1> { ... }`. Mirrors
        // struct/enum/fn generics (the `<` after a trait name is unambiguous).
        decl.genericParams = parseOptionalGenericParams();
        // v25 Phase 136: optional supertrait bounds `trait Ord: Eq + Hash { … }`.
        if (accept(TokenKind::Colon)) {
            while (true) {
                Token superTok = expect(TokenKind::Identifier, "a supertrait name");
                decl.supertraits.push_back(superTok.lexeme);
                if (!accept(TokenKind::Plus)) break;
            }
        }
        expect(TokenKind::LBrace, "{");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            // Phase 21b: an associated-type declaration `type Item;`. `type` is
            // an Identifier at the lexer level, so detect it by lexeme (like
            // `mut`/`dyn`). Methods otherwise start with `fn`.
            if (check(TokenKind::Identifier) && peek().lexeme == "type") {
                Token typeTok = consume();
                ast::AssocTypeDecl at;
                at.line = typeTok.line;
                at.column = typeTok.column;
                Token nameTok =
                    expect(TokenKind::Identifier, "associated type name");
                at.name = nameTok.lexeme;
                // v28 Phase 155 (GATs): optional generic params `type Out<T>;`.
                at.typeParams = parseOptionalGenericParams();
                expect(TokenKind::Semi, ";");
                decl.assocTypes.push_back(std::move(at));
                continue;
            }
            // v25 Phase 139: a trait associated const `const N: T;` desugars to
            // a no-self method signature `fn N() -> T;`.
            if (check(TokenKind::KwConst) && peek(1).kind != TokenKind::KwFn) {
                decl.methods.push_back(parseTraitAssocConst());
                continue;
            }
            decl.methods.push_back(parseMethodSig());
        }
        expect(TokenKind::RBrace, "}");
        return decl;
    }

    // v25 Phase 139: `const N: T;` in a trait -> a no-self method sig.
    ast::MethodSig parseTraitAssocConst() {
        Token c = expect(TokenKind::KwConst, "const");
        Token name = expect(TokenKind::Identifier, "associated const name");
        expect(TokenKind::Colon, ":");
        ast::MethodSig sig;
        sig.name = name.lexeme;
        sig.returnType = parseTypeRef();
        sig.line = c.line;
        sig.column = c.column;
        expect(TokenKind::Semi, ";");
        return sig;
    }

    // v25 Phase 139: `const N: T = expr;` in an impl -> `fn N() -> T { expr }`.
    ast::FnDecl parseImplAssocConst() {
        Token c = expect(TokenKind::KwConst, "const");
        Token name = expect(TokenKind::Identifier, "associated const name");
        expect(TokenKind::Colon, ":");
        ast::FnDecl fn;
        fn.name = name.lexeme;
        fn.returnType = parseTypeRef();
        fn.line = c.line;
        fn.column = c.column;
        expect(TokenKind::Eq, "=");
        auto body = std::make_unique<ast::BlockExpr>();
        body->line = c.line;
        body->column = c.column;
        body->tail = parseExpr();
        expect(TokenKind::Semi, ";");
        fn.body = std::move(body);
        return fn;
    }

    ast::MethodSig parseMethodSig() {
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::MethodSig sig;
        sig.line = fnTok.line;
        sig.column = fnTok.column;
        Token nameTok = expect(TokenKind::Identifier, "method name");
        sig.name = nameTok.lexeme;
        expect(TokenKind::LParen, "(");
        if (!check(TokenKind::RParen)) {
            while (true) {
                sig.params.push_back(parseSelfOrParam());
                if (!accept(TokenKind::Comma)) break;
            }
        }
        expect(TokenKind::RParen, ")");
        sig.returnType = parseOptionalReturnType();
        sig.effects = parseOptionalEffectRow();
        // v25 Phase 135: a trait method may carry a DEFAULT body `{ … }` instead
        // of `;`; impls that don't override it inherit the default.
        if (check(TokenKind::LBrace)) {
            sig.body = parseBlockExpr();
        } else {
            expect(TokenKind::Semi, ";");
        }
        return sig;
    }

    // Trait/impl method first param is conventionally `self` (lowercase).
    // We accept either `self` (by value, type `Self`), `&self` (by shared
    // reference, type `&Self` — Phase 11, required for dyn dispatch), or
    // `name: Type` for additional params.
    ast::Param parseSelfOrParam() {
        const Token& t = peek();
        // Phase 11: `&self` / `&mut self` — the receiver is borrowed. Lowers
        // to a `self: &Self` (`&mut Self`) param, so the method's LLVM first
        // arg is a pointer into the object's data. This is what a trait
        // object's vtable thunk hands across (the fat pointer's data slot).
        if (t.kind == TokenKind::Ampersand) {
            Token amp = consume();
            bool isMut = false;
            if (check(TokenKind::Identifier) && peek().lexeme == "mut") {
                consume();
                isMut = true;
            }
            if (check(TokenKind::Identifier) && peek().lexeme == "self") {
                Token selfTok = consume();
                ast::Param p;
                p.name = "self";
                p.type.name = "Self";
                p.type.isRef = true;
                p.type.refIsMut = isMut;
                p.type.line = amp.line;
                p.type.column = amp.column;
                return p;
            }
            // `&` not followed by `self` in receiver position is unsupported
            // here; emit a diagnostic via the normal param path's expect.
            errorHere("expected `self` after `&` in receiver position");
            ast::Param p;
            p.name = "self";
            p.type.name = "Self";
            p.type.isRef = true;
            p.type.refIsMut = isMut;
            return p;
        }
        if (t.kind == TokenKind::Identifier && t.lexeme == "self") {
            Token selfTok = consume();
            ast::Param p;
            p.name = "self";
            p.type.name = "Self";
            p.type.line = selfTok.line;
            p.type.column = selfTok.column;
            return p;
        }
        return parseParam();
    }

    // Phase 42: parse one-or-more `#[derive(Trait, ...)]` attributes into
    // pendingDerives_, which the next struct/enum decl consumes. Unknown
    // attributes are tolerated (skipped to the closing `]`).
    std::vector<std::string> pendingDerives_;
    void parseAttributes() {
        while (check(TokenKind::Pound)) {
            consume(); // '#'
            expect(TokenKind::LBracket, "[");
            Token attr = expect(TokenKind::Identifier, "attribute name");
            if (attr.lexeme == "derive") {
                expect(TokenKind::LParen, "(");
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        Token t = expect(TokenKind::Identifier, "trait name");
                        pendingDerives_.push_back(t.lexeme);
                        if (!accept(TokenKind::Comma)) break;
                        if (check(TokenKind::RParen)) break; // trailing comma
                    }
                }
                expect(TokenKind::RParen, ")");
            } else {
                while (!check(TokenKind::RBracket) &&
                       !check(TokenKind::EndOfInput))
                    consume();
            }
            expect(TokenKind::RBracket, "]");
        }
    }

    ast::ImplDecl parseImplDecl() {
        Token implTok = expect(TokenKind::KwImpl, "impl");
        ast::ImplDecl decl;
        decl.line = implTok.line;
        decl.column = implTok.column;
        // Phase 40: the impl's own generic params, `impl<T: Bound> ...`. The
        // `<` here is unambiguous (it directly follows `impl`). These are in
        // scope for forType + every method.
        decl.genericParams = parseOptionalGenericParams();
        // v31 Phase 167: a NEGATIVE marker impl `impl !Send for T {}`. The `!`
        // (a standalone Bang here, since it is not followed by `=`) opts a type
        // OUT of the auto-derived Send/Sync membership. Only legal as a trait
        // impl (`for` form) of a marker trait, with an empty body — both
        // enforced below / in typecheck.
        bool isNegative = accept(TokenKind::Bang);
        // Three forms:
        //   impl Trait for Type { ... }        -- trait impl (since Phase 3.3)
        //   impl Trait<Args...> for Type { ... } -- generic trait impl (21a)
        //   impl Type { ... } / impl Type<T>{}  -- inherent impl (Phase 15)
        // Parse the leading name, then an optional `<...>` arg list, then
        // disambiguate on the following token: `for` => the name (and any
        // parsed `<...>`) was the trait + its type args (parse the real
        // forType next); otherwise (`{`) it was the implementing type itself
        // and this is an inherent impl (empty traitName).
        Token nameTok = expect(TokenKind::Identifier, "trait or type name");
        std::vector<ast::TypeRef> leadingArgs;
        if (accept(TokenKind::Lt)) {
            if (!check(TokenKind::Gt)) {
                while (true) {
                    leadingArgs.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::Gt)) break; // trailing comma
                }
            }
            expect(TokenKind::Gt, ">");
        }
        if (accept(TokenKind::KwFor)) {
            // Trait impl: the leading name is the trait, `leadingArgs` are the
            // trait's type args (Phase 21a), and the forType follows `for`.
            decl.traitName = nameTok.lexeme;
            decl.traitTypeArgs = std::move(leadingArgs);
            decl.forType = parseTypeRef();
            decl.isNegative = isNegative; // v31 Phase 167
        } else {
            // v31 Phase 167: a negative impl MUST name a trait (`impl !Send for
            // T`); `impl !Type {}` is meaningless.
            if (isNegative) {
                errorAt("a negative impl `impl !Trait` must be `for` a type",
                        nameTok.line, nameTok.column);
            }
            // Inherent impl: `nameTok` + `leadingArgs` form the implementing
            // type's TypeRef (e.g. `impl Pair<T> { ... }`). traitName stays
            // empty.
            ast::TypeRef tr;
            tr.name = nameTok.lexeme;
            tr.line = nameTok.line;
            tr.column = nameTok.column;
            tr.typeArgs = std::move(leadingArgs);
            decl.forType = std::move(tr);
        }
        // Phase 40: a `where` clause adds bounds onto the impl's generic params.
        parseOptionalWhereClause(decl.genericParams);
        expect(TokenKind::LBrace, "{");
        // v31 Phase 167: a negative marker impl carries no methods.
        if (decl.isNegative && !check(TokenKind::RBrace)) {
            errorAt("a negative impl `impl !Trait for T` must have an empty "
                    "body `{}`",
                    decl.line, decl.column);
        }
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            // Phase 21b: an associated-type definition `type Item = i64;`.
            if (check(TokenKind::Identifier) && peek().lexeme == "type") {
                Token typeTok = consume();
                ast::AssocTypeDef at;
                at.line = typeTok.line;
                at.column = typeTok.column;
                Token nameTok =
                    expect(TokenKind::Identifier, "associated type name");
                at.name = nameTok.lexeme;
                // v28 Phase 155 (GATs): the binding's own params `type Out<T> =
                // Pair<T, T>;` — those names are in scope while parsing the RHS.
                at.typeParams = parseOptionalGenericParams();
                expect(TokenKind::Eq, "=");
                at.type = parseTypeRef();
                expect(TokenKind::Semi, ";");
                decl.assocTypes.push_back(std::move(at));
                continue;
            }
            // v25 Phase 139: an associated const `const N: T = expr;` desugars
            // to a no-self method `fn N() -> T { expr }`, read as `Type::N()`.
            // (`const fn` is a function — disambiguate on the token after.)
            if (check(TokenKind::KwConst) && peek(1).kind != TokenKind::KwFn) {
                decl.methods.push_back(parseImplAssocConst());
                continue;
            }
            decl.methods.push_back(parseImplFnDecl());
        }
        expect(TokenKind::RBrace, "}");
        return decl;
    }

    // Like parseFnDecl but with parseSelfOrParam for the first param so
    // the `self` shorthand works.
    ast::FnDecl parseImplFnDecl() {
        bool isAsync = false;
        if (peek().kind == TokenKind::Identifier &&
            peek().lexeme == "async" &&
            peek(1).kind == TokenKind::KwFn) {
            advance();
            isAsync = true;
        }
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::FnDecl decl;
        decl.isAsync = isAsync;
        decl.line = fnTok.line;
        decl.column = fnTok.column;
        Token nameTok = expect(TokenKind::Identifier, "function name");
        decl.name = nameTok.lexeme;
        decl.genericParams = parseOptionalGenericParams();
        expect(TokenKind::LParen, "(");
        if (!check(TokenKind::RParen)) {
            while (true) {
                decl.params.push_back(parseSelfOrParam());
                if (!accept(TokenKind::Comma)) break;
            }
        }
        expect(TokenKind::RParen, ")");
        decl.returnType = parseOptionalReturnType();
        decl.effects = parseOptionalEffectRow();
        // Phase 21b: `where` clause on an impl method, desugared as for free fns.
        parseOptionalWhereClause(decl.genericParams);
        decl.body = parseBlockExpr();
        return decl;
    }

    // --- Block & statements ---

    // v24 Phase 131: panic-mode recovery. Skip the malformed remainder of a
    // statement — advance to and consume the next `;`, or stop just before a
    // `}` / statement keyword / EOF — so the next statement parses cleanly
    // instead of the parser cascading off the leftover tokens.
    void synchronizeStmt() {
        while (!check(TokenKind::EndOfInput)) {
            if (check(TokenKind::Semi)) {
                consume();
                return;
            }
            if (check(TokenKind::RBrace) || check(TokenKind::KwLet) ||
                check(TokenKind::KwReturn))
                return;
            consume();
        }
    }

    // v24 Phase 131: terminate a statement. If the value parsed cleanly, require
    // the `;`; if it already reported an error, resynchronize instead of adding
    // a spurious "expected ;" on top of the real diagnostic.
    void expectSemiOrSync(std::size_t errBefore) {
        if (errors_.size() > errBefore)
            synchronizeStmt();
        else
            expect(TokenKind::Semi, ";");
    }

    std::unique_ptr<ast::BlockExpr> parseBlockExpr() {
        Token lbrace = expect(TokenKind::LBrace, "{");
        auto block = std::make_unique<ast::BlockExpr>();
        block->line = lbrace.line;
        block->column = lbrace.column;
        bool prevBlockRestrict = restrictStructLit_;
        restrictStructLit_ = false;

        std::size_t syncErrCount = errors_.size();
        std::size_t syncPos = static_cast<std::size_t>(-1);
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            // v24 Phase 131: if the previous statement errored (or made no
            // progress), resynchronize before parsing the next one.
            if (errors_.size() > syncErrCount || pos_ == syncPos) {
                synchronizeStmt();
                if (check(TokenKind::RBrace) || check(TokenKind::EndOfInput))
                    break;
            }
            syncErrCount = errors_.size();
            syncPos = pos_;

            if (check(TokenKind::KwLet)) {
                block->stmts.push_back(parseLetStmt());
                continue;
            }
            if (check(TokenKind::KwReturn)) {
                block->stmts.push_back(parseReturnStmt());
                continue;
            }
            // Bare expression — either a stmt (terminated by ';'), an
            // assignment (`lhs = rhs;`, Phase 9), or the block's tail value
            // (no ';' before '}').
            auto expr = parseExpr();
            std::size_t line = expr->line;
            std::size_t col = expr->column;
            // Phase 9: `lhs = rhs;` assignment. The single `=` is only an
            // assignment here (comparison uses `==`), so seeing it after a
            // primary expression unambiguously starts an assignment.
            if (check(TokenKind::Eq)) {
                Token eqTok = consume();
                auto rhs = parseExpr();
                expect(TokenKind::Semi, ";");
                auto as = std::make_unique<ast::AssignStmt>();
                as->line = eqTok.line;
                as->column = eqTok.column;
                as->target = std::move(expr);
                as->value = std::move(rhs);
                block->stmts.push_back(std::move(as));
                continue;
            }
            if (accept(TokenKind::Semi)) {
                auto es = std::make_unique<ast::ExprStmt>();
                es->line = line;
                es->column = col;
                es->expr = std::move(expr);
                block->stmts.push_back(std::move(es));
            } else if (isBlockLikeExpr(*expr) && !check(TokenKind::RBrace)) {
                // Phase 9: block-like expressions (`if`/`match`/`while`/
                // `loop`/`for`/`{...}`) used in statement position don't
                // require a trailing `;`. If more statements follow (the
                // next token isn't `}`), treat this as a statement; the
                // tail case below still applies when it's the block's last
                // element.
                auto es = std::make_unique<ast::ExprStmt>();
                es->line = line;
                es->column = col;
                es->expr = std::move(expr);
                block->stmts.push_back(std::move(es));
            } else {
                block->tail = std::move(expr);
                break; // tail must be the last thing in the block
            }
        }
        expect(TokenKind::RBrace, "}");
        restrictStructLit_ = prevBlockRestrict;
        return block;
    }

    // Phase 9: block-like expressions can appear as statements without a
    // trailing `;` (Rust's statement/expression distinction). Used by
    // parseBlockExpr to decide whether a tail-position expression is
    // actually a statement followed by more code.
    static bool isBlockLikeExpr(const ast::Expr& e) {
        return dynamic_cast<const ast::IfExpr*>(&e) ||
               dynamic_cast<const ast::MatchExpr*>(&e) ||
               dynamic_cast<const ast::WhileExpr*>(&e) ||
               dynamic_cast<const ast::LoopExpr*>(&e) ||
               dynamic_cast<const ast::ForExpr*>(&e) ||
               dynamic_cast<const ast::BlockExpr*>(&e);
    }

    ast::StmtPtr parseLetStmt() {
        Token letTok = expect(TokenKind::KwLet, "let");
        // Phase 9: `let mut x = ...`. `mut` stays an Identifier at the
        // lexer level (reserved by convention), so match by lexeme.
        bool isMut = false;
        if (check(TokenKind::Identifier) && peek().lexeme == "mut") {
            consume();
            isMut = true;
        }
        // Phase 22: tuple-destructuring `let (x, y) = t;`. A `(` here begins a
        // tuple pattern; we collect the element binding names (a `_` drops
        // that position). No `: Type` annotation on this form (element types
        // come from the RHS). The single-binding form is unchanged below.
        auto stmt = std::make_unique<ast::LetStmt>();
        stmt->line = letTok.line;
        stmt->column = letTok.column;
        stmt->isMut = isMut;
        if (check(TokenKind::LParen)) {
            consume(); // (
            if (!check(TokenKind::RParen)) {
                while (true) {
                    if (check(TokenKind::Underscore)) {
                        consume();
                        stmt->tupleNames.push_back("_");
                    } else {
                        Token nm = expect(TokenKind::Identifier,
                                          "binding name in tuple pattern");
                        stmt->tupleNames.push_back(nm.lexeme);
                    }
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break; // trailing comma
                }
            }
            expect(TokenKind::RParen, ")");
            // Phase 57: optional `: (T, ...)` annotation on a tuple-pattern let
            // (`let (a, b): (T, T) = ..`) — useful to pin generic element types
            // a multi-value generic call can't infer. The typechecker unifies
            // each binding against its annotated element (and the RHS element).
            if (accept(TokenKind::Colon)) {
                stmt->annotation =
                    std::make_shared<ast::TypeRef>(parseTypeRef());
            }
            expect(TokenKind::Eq, "=");
            std::size_t eb = errors_.size();
            stmt->value = parseExpr();
            expectSemiOrSync(eb);
            return stmt;
        }
        Token nameTok = expect(TokenKind::Identifier, "identifier after 'let'");
        // Phase 11: optional `: Type` annotation (needed to spell out a
        // coercion target like `let b: Box<dyn Shape> = Box::new(...)`).
        std::shared_ptr<ast::TypeRef> annotation;
        if (accept(TokenKind::Colon)) {
            annotation = std::make_shared<ast::TypeRef>(parseTypeRef());
        }
        expect(TokenKind::Eq, "=");
        std::size_t eb = errors_.size();
        auto value = parseExpr();
        expectSemiOrSync(eb);
        stmt->name = nameTok.lexeme;
        stmt->value = std::move(value);
        stmt->annotation = std::move(annotation);
        return stmt;
    }

    ast::StmtPtr parseReturnStmt() {
        Token retTok = expect(TokenKind::KwReturn, "return");
        auto stmt = std::make_unique<ast::ReturnStmt>();
        stmt->line = retTok.line;
        stmt->column = retTok.column;
        std::size_t eb = errors_.size();
        if (!check(TokenKind::Semi)) {
            stmt->value = parseExpr();
        }
        expectSemiOrSync(eb);
        return stmt;
    }

    // --- Expressions ---

    // Precedence tiers (tighter = larger). Phase 124 adds `||` as the loosest
    // tier (below `&&`), matching Rust: `||` < `&&` < comparison < `|` < `^` <
    // `&` < shift < `+ -` < `* / %`. Shift (`<< >>`) is a two-token operator
    // handled by adjacency in parseExprPrec (kShiftPrec). `||` is the PipePipe
    // token: in INFIX position (after an operand) it is logical-or, while in
    // PRIMARY position it is still a zero-param closure — parsePrimary matches
    // it there before this loop ever consults binPrec.
    static constexpr int kShiftPrec = 7;
    static int binPrec(TokenKind k) {
        switch (k) {
        case TokenKind::PipePipe: // Phase 124: `||` binds loosest (below `&&`)
            return 1;
        case TokenKind::AmpAmp: // `&&` (below comparisons, above `||`)
            return 2;
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::Lt:
        case TokenKind::Le:
        case TokenKind::Gt:
        case TokenKind::Ge:
            return 3;
        case TokenKind::Pipe: // Phase 66: infix bitwise-or
            return 4;
        case TokenKind::Caret: // Phase 66: bitwise-xor
            return 5;
        case TokenKind::Ampersand: // Phase 66: infix bitwise-and
            return 6;
        // kShiftPrec == 7 (handled by adjacency, not a single token)
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 8;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent: // `%` at the multiplicative tier
            return 9;
        default:
            return 0; // not a binop
        }
    }

    static ast::BinOp tokenToBinOp(TokenKind k) {
        switch (k) {
        case TokenKind::Plus: return ast::BinOp::Add;
        case TokenKind::Minus: return ast::BinOp::Sub;
        case TokenKind::Star: return ast::BinOp::Mul;
        case TokenKind::Slash: return ast::BinOp::Div;
        case TokenKind::Percent: return ast::BinOp::Mod;
        case TokenKind::AmpAmp: return ast::BinOp::And;
        case TokenKind::PipePipe: return ast::BinOp::Or;    // Phase 124
        case TokenKind::Lt: return ast::BinOp::Lt;
        case TokenKind::Le: return ast::BinOp::Le;
        case TokenKind::Gt: return ast::BinOp::Gt;
        case TokenKind::Ge: return ast::BinOp::Ge;
        case TokenKind::EqEq: return ast::BinOp::Eq;
        case TokenKind::NotEq: return ast::BinOp::NotEq;
        case TokenKind::Pipe: return ast::BinOp::BitOr;      // Phase 66
        case TokenKind::Caret: return ast::BinOp::BitXor;    // Phase 66
        case TokenKind::Ampersand: return ast::BinOp::BitAnd; // Phase 66
        default: return ast::BinOp::Add; // unreachable
        }
    }

    ast::ExprPtr parseExpr() {
        auto lhs = parseExprPrec(1);
        // Phase 9: range operators bind looser than every binary operator,
        // so `a + 1 .. b * 2` parses the arithmetic on each side first.
        // A range is non-associative: `a..b..c` is a parse error we don't
        // bother diagnosing precisely — the trailing `..c` just won't parse.
        if (check(TokenKind::DotDot) || check(TokenKind::DotDotEq)) {
            bool inclusive = check(TokenKind::DotDotEq);
            Token opTok = consume();
            auto rhs = parseExprPrec(1);
            auto re = std::make_unique<ast::RangeExpr>();
            re->line = opTok.line;
            re->column = opTok.column;
            re->start = std::move(lhs);
            re->end = std::move(rhs);
            re->inclusive = inclusive;
            return re;
        }
        return lhs;
    }

    // Phase 65: `operand as Type` casts. `as` binds tighter than every binary
    // operator (so it sits below the precedence loop) but looser than a prefix
    // unary (so the operand is a full parseUnary): `-x as i32` is `(-x) as i32`
    // and `a as i32 * 2` is `(a as i32) * 2`. Casts chain left-to-right
    // (`x as i32 as i64`).
    ast::ExprPtr parseCast() {
        auto expr = parseUnary();
        while (check(TokenKind::KwAs)) {
            Token asTok = consume();
            // The cast target is a bare numeric type NAME (`i8`..`u64`, `f32`,
            // `f64`). We deliberately do NOT call parseTypeRef here: in
            // expression position a trailing `<` / `<<` is a comparison / shift,
            // not a generic-argument list, but parseTypeRef would greedily eat
            // it as `Name<...>` (it assumes a type only ever appears after
            // `:` / `->` / `(` / `,`). So `x as i32 << 2` must parse as
            // `(x as i32) << 2`, not `x as (i32 << ...)`. Reading just the name
            // leaves the operator for the precedence loop. (Casts are
            // numeric-only — typecheck rejects a non-numeric target — so a bare
            // scalar name covers every valid target.)
            Token nameTok = expect(TokenKind::Identifier, "a type name after `as`");
            ast::TypeRef ty;
            ty.name = nameTok.lexeme;
            ty.line = nameTok.line;
            ty.column = nameTok.column;
            auto ce = std::make_unique<ast::CastExpr>();
            ce->line = asTok.line;
            ce->column = asTok.column;
            ce->operand = std::move(expr);
            ce->targetType = std::move(ty);
            expr = std::move(ce);
        }
        return expr;
    }

    ast::ExprPtr parseExprPrec(int minPrec) {
        auto lhs = parseCast();
        while (true) {
            // Phase 66: a shift operator is two column-adjacent `<`/`>` tokens.
            // Detecting it by adjacency (not a dedicated lexer token) keeps a
            // nested-generic close `Vec<Vec<T>>` — which is also two adjacent
            // `>` but only ever parsed in TYPE context — unambiguous, since the
            // expression parser never parses a type.
            bool isShl = peek().kind == TokenKind::Lt &&
                         peek(1).kind == TokenKind::Lt &&
                         peek(1).column == peek().column + 1;
            bool isShr = peek().kind == TokenKind::Gt &&
                         peek(1).kind == TokenKind::Gt &&
                         peek(1).column == peek().column + 1;
            if (isShl || isShr) {
                if (kShiftPrec < minPrec) break;
                Token opTok = consume(); // first '<' / '>'
                consume();               // second '<' / '>'
                auto rhs = parseExprPrec(kShiftPrec + 1); // left-associative
                auto bin = std::make_unique<ast::BinaryExpr>();
                bin->line = opTok.line;
                bin->column = opTok.column;
                bin->op = isShl ? ast::BinOp::Shl : ast::BinOp::Shr;
                bin->lhs = std::move(lhs);
                bin->rhs = std::move(rhs);
                lhs = std::move(bin);
                continue;
            }
            int prec = binPrec(peek().kind);
            if (prec == 0 || prec < minPrec) break;
            Token opTok = consume();
            auto rhs = parseExprPrec(prec + 1); // left-associative
            auto bin = std::make_unique<ast::BinaryExpr>();
            bin->line = opTok.line;
            bin->column = opTok.column;
            bin->op = tokenToBinOp(opTok.kind);
            bin->lhs = std::move(lhs);
            bin->rhs = std::move(rhs);
            lhs = std::move(bin);
        }
        return lhs;
    }

    // Phase 15: prefix unary operators `-` (integer negation) and `!`
    // (logical not). They bind tighter than every binary operator but
    // looser than postfix `.`/`?`/`[..]`, matching Rust: `-a * b` parses
    // as `(-a) * b`, `-a.b` as `-(a.b)`, and `!a == b` as `(!a) == b`.
    // Unary operators stack and right-recurse (`- -3`, `!!b`), so the
    // operand is itself parsed via parseUnary. A bare `&`/`&mut` prefix
    // and the closure `|` are still handled by parsePrimary, so `-&x` and
    // `&-x` both flow through correctly.
    ast::ExprPtr parseUnary() {
        // Phase 34: a leading `*` is the deref operator (an infix `*` is
        // multiplication, handled in the precedence parser — position
        // disambiguates, the standard prefix/infix split).
        if (check(TokenKind::Minus) || check(TokenKind::Bang) ||
            check(TokenKind::Star) || check(TokenKind::Tilde)) {
            Token opTok = consume();
            auto operand = parseUnary();
            auto ue = std::make_unique<ast::UnaryExpr>();
            ue->line = opTok.line;
            ue->column = opTok.column;
            ue->op = opTok.kind == TokenKind::Minus  ? ast::UnaryOp::Neg
                     : opTok.kind == TokenKind::Bang ? ast::UnaryOp::Not
                     : opTok.kind == TokenKind::Tilde ? ast::UnaryOp::BitNot
                                                      : ast::UnaryOp::Deref;
            ue->operand = std::move(operand);
            return ue;
        }
        return parsePostfix();
    }

    ast::ExprPtr parsePostfix() {
        auto expr = parsePrimary();
        while (true) {
            if (check(TokenKind::Dot)) {
                Token dotTok = consume();
                // `.await` is a postfix operator (the suspend point of an
                // async fn; lowered to a real poll loop in Phase 12). We
                // recognise `await` by lexeme (it stays an Identifier
                // so the same word can appear as an effect label).
                // Distinguish from a field literally named `await` by
                // the fact that AwaitExpr is consumed without args; if
                // a future syntax wants `obj.await(x)` we'll branch on
                // a following `(`.
                if (peek().kind == TokenKind::Identifier &&
                    peek().lexeme == "await" &&
                    peek(1).kind != TokenKind::LParen) {
                    Token awTok = consume();
                    auto ae = std::make_unique<ast::AwaitExpr>();
                    ae->line = dotTok.line;
                    ae->column = dotTok.column;
                    ae->operand = std::move(expr);
                    (void)awTok;
                    expr = std::move(ae);
                    continue;
                }
                // Phase 22: a numeric field after `.` is tuple-element access
                // `t.0`, `t.1`. (There are no float literals, so an Integer
                // here is unambiguously a tuple index — never part of a
                // method/field name.)
                if (check(TokenKind::Integer)) {
                    Token idxTok = consume();
                    auto tf = std::make_unique<ast::TupleFieldExpr>();
                    tf->line = dotTok.line;
                    tf->column = dotTok.column;
                    tf->object = std::move(expr);
                    try {
                        tf->index =
                            static_cast<std::size_t>(std::stoull(idxTok.lexeme));
                    } catch (const std::exception&) {
                        errorHere("tuple index out of range: " + idxTok.lexeme);
                        tf->index = 0;
                    }
                    expr = std::move(tf);
                    continue;
                }
                Token nameTok = expect(TokenKind::Identifier, "field name after '.'");
                // `.name(args)` is a method call; `.name` alone is field
                // access. The distinction matters: method calls route
                // through trait/impl resolution at typecheck time.
                if (check(TokenKind::LParen)) {
                    advance(); // consume '('
                    auto mc = std::make_unique<ast::MethodCallExpr>();
                    mc->line = dotTok.line;
                    mc->column = dotTok.column;
                    mc->receiver = std::move(expr);
                    mc->methodName = nameTok.lexeme;
                    bool prevCallRestrict = restrictStructLit_;
                    restrictStructLit_ = false;
                    if (!check(TokenKind::RParen)) {
                        while (true) {
                            mc->args.push_back(parseExpr());
                            if (!accept(TokenKind::Comma)) break;
                        }
                    }
                    restrictStructLit_ = prevCallRestrict;
                    expect(TokenKind::RParen, ")");
                    expr = std::move(mc);
                    continue;
                }
                auto fe = std::make_unique<ast::FieldExpr>();
                fe->line = dotTok.line;
                fe->column = dotTok.column;
                fe->object = std::move(expr);
                fe->fieldName = nameTok.lexeme;
                expr = std::move(fe);
                continue;
            }
            if (check(TokenKind::Question)) {
                Token qTok = consume();
                auto te = std::make_unique<ast::TryExpr>();
                te->line = qTok.line;
                te->column = qTok.column;
                te->operand = std::move(expr);
                expr = std::move(te);
                continue;
            }
            // Phase 13b / 22: `expr[...]` postfix. Two forms, disambiguated by
            // whether a `..` follows the first sub-expression:
            //   - `v[a..b]` is a slice of a Vec (SliceExpr) — half-open range.
            //   - `arr[i]`  is a fixed-size-array index (IndexExpr) — single
            //     i64 index, no range.
            // The leading `&` of a slice (`&v[a..b]`) is absorbed in
            // parsePrimary; a bare `arr[i]` index needs no `&`.
            if (check(TokenKind::LBracket)) {
                Token lb = consume();
                // Parse the first part as a precedence-1 expression so a
                // trailing `..` (slice range) is NOT swallowed as a RangeExpr.
                ast::ExprPtr first = parseExprPrec(1);
                if (accept(TokenKind::DotDot)) {
                    auto se = std::make_unique<ast::SliceExpr>();
                    se->line = lb.line;
                    se->column = lb.column;
                    se->operand = std::move(expr);
                    se->start = std::move(first);
                    se->end = parseExprPrec(1);
                    expect(TokenKind::RBracket, "]");
                    expr = std::move(se);
                    continue;
                }
                auto ix = std::make_unique<ast::IndexExpr>();
                ix->line = lb.line;
                ix->column = lb.column;
                ix->object = std::move(expr);
                ix->index = std::move(first);
                expect(TokenKind::RBracket, "]");
                expr = std::move(ix);
                continue;
            }
            // Phase 17a: a `(arglist)` in postfix position calls the fn VALUE
            // produced by the preceding expression — e.g. `(s.f)(x)` or
            // `(getCallback())(args)`. Bare `ident(args)` / `path::f(args)`
            // were already consumed as a CallExpr in parsePrimary, and
            // `recv.method(args)` as a MethodCallExpr in the `.` arm above, so
            // reaching here means the callee is a parenthesized expr, a field
            // access, or the result of another call — all dispatched at
            // runtime through the fat pointer.
            if (check(TokenKind::LParen)) {
                Token lp = consume();
                auto cv = std::make_unique<ast::CallValueExpr>();
                cv->line = lp.line;
                cv->column = lp.column;
                cv->callee = std::move(expr);
                bool prevCallRestrict = restrictStructLit_;
                restrictStructLit_ = false;
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        cv->args.push_back(parseExpr());
                        if (!accept(TokenKind::Comma)) break;
                    }
                }
                restrictStructLit_ = prevCallRestrict;
                expect(TokenKind::RParen, ")");
                expr = std::move(cv);
                continue;
            }
            break;
        }
        return expr;
    }

    ast::ExprPtr parsePrimary() {
        const Token& t = peek();

        // Phase 2.4b: `&` prefix produces a RefExpr; `&mut` flips isMut.
        if (t.kind == TokenKind::Ampersand) {
            Token amp = consume();
            bool isMut = false;
            if (check(TokenKind::Identifier) && peek().lexeme == "mut") {
                consume();
                isMut = true;
            }
            auto inner = parsePostfix();
            // Phase 13b: `&v[a..b]` — the postfix `[a..b]` already produced a
            // SliceExpr; the leading `&` is part of the slice syntax (a slice
            // is itself a borrow), so absorb it and hand back the SliceExpr
            // directly rather than wrapping it in a RefExpr.
            if (dynamic_cast<ast::SliceExpr*>(inner.get())) {
                return inner;
            }
            auto re = std::make_unique<ast::RefExpr>();
            re->line = amp.line;
            re->column = amp.column;
            re->operand = std::move(inner);
            re->isMut = isMut;
            return re;
        }

        // Phase 22: an array literal `[a, b, c]` in primary position. (A `[`
        // in *postfix* position is an index / slice, handled in parsePostfix;
        // here it begins a fresh value.) Element struct-literals are allowed
        // inside the brackets regardless of the surrounding restriction.
        if (t.kind == TokenKind::LBracket) {
            Token lb = consume();
            auto arr = std::make_unique<ast::ArrayLitExpr>();
            arr->line = lb.line;
            arr->column = lb.column;
            bool prev = restrictStructLit_;
            restrictStructLit_ = false;
            if (!check(TokenKind::RBracket)) {
                arr->elements.push_back(parseExpr());
                // Phase 62: array-REPEAT `[value; count]`. A `;` after the first
                // element (instead of `,`) means "this value, `count` times".
                if (accept(TokenKind::Semi)) {
                    arr->repeatCount = parseExpr();
                } else {
                    while (accept(TokenKind::Comma)) {
                        if (check(TokenKind::RBracket)) break; // trailing comma
                        arr->elements.push_back(parseExpr());
                    }
                }
            }
            restrictStructLit_ = prev;
            expect(TokenKind::RBracket, "]");
            return arr;
        }

        if (t.kind == TokenKind::Integer) {
            Token tok = consume();
            auto e = std::make_unique<ast::IntLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            fillIntLit(tok, *e);
            return e;
        }

        // Phase 39: f64 literal.
        if (t.kind == TokenKind::Float) {
            Token tok = consume();
            auto e = std::make_unique<ast::FloatLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            // Phase 67: strip a trailing `f32`/`f64` suffix (the lexeme may be
            // `5f32` with no decimal point — still a float). std::stod parses
            // the numeric prefix and ignores the suffix, but we record the
            // width and keep a clean lexeme for the formatter.
            std::string lex = tok.lexeme;
            if (lex.size() > 3) {
                std::string tail = lex.substr(lex.size() - 3);
                if (tail == "f32") { e->suffixWidth = 32; lex.resize(lex.size() - 3); }
                else if (tail == "f64") { e->suffixWidth = 64; lex.resize(lex.size() - 3); }
            }
            e->lexeme = lex;
            try {
                e->value = std::stod(lex);
            } catch (const std::exception&) {
                errorHere("float literal out of range: " + tok.lexeme);
                e->value = 0.0;
            }
            return e;
        }

        if (t.kind == TokenKind::StringLit) {
            Token tok = consume();
            auto e = std::make_unique<ast::StringLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            e->value = tok.lexeme;
            return e;
        }

        // v27 Phase 147: char literal `'c'`. The lexer already decoded the
        // scalar to a codepoint stored as a decimal string in the lexeme.
        if (t.kind == TokenKind::CharLit) {
            Token tok = consume();
            auto e = std::make_unique<ast::CharLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            e->codepoint =
                static_cast<std::uint32_t>(std::stoul(tok.lexeme));
            return e;
        }

        // Phase 15: boolean literals `true` / `false`.
        if (t.kind == TokenKind::KwTrue || t.kind == TokenKind::KwFalse) {
            Token tok = consume();
            auto e = std::make_unique<ast::BoolLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            e->value = (tok.kind == TokenKind::KwTrue);
            return e;
        }

        if (t.kind == TokenKind::Identifier) {
            // Phase 7.2: paths (`foo::bar`) collapse to their last
            // segment because modules currently flat-merge into one
            // namespace. Phase 7.3b additionally marks `wasPath` on
            // CallExpr so the typechecker can enforce `pub` against
            // path-qualified references.
            Token first = consume();
            // v27 Phase 149: the built-in formatting "macros" `format!(...)`,
            // `print!(...)`, `println!(...)`. There is no general macro system
            // yet (that's a later roadmap), so these are recognized here and
            // desugared to string-building over `string_new`/`string_push_str`
            // and `Display::to_string`. Detected as `ident ! (` with one of the
            // three names.
            if (check(TokenKind::Bang) && peek(1).kind == TokenKind::LParen &&
                (first.lexeme == "format" || first.lexeme == "print" ||
                 first.lexeme == "println")) {
                return parseFormatMacro(first);
            }
            Token tok = first;
            Token prevSeg = first; // Phase 48: the segment just before `tok`
            bool wasPath = false;
            while (check(TokenKind::DoubleColon)) {
                consume();
                prevSeg = tok;
                tok = expect(TokenKind::Identifier,
                              "identifier after `::`");
                wasPath = true;
            }
            // Preserve the first-segment's position for diagnostics
            // since that's where the user's path begins.
            tok.line = first.line;
            tok.column = first.column;
            // Phase 11: `Box::new(value)` is a built-in heap allocation, not
            // a user fn. Recognize the full path here (before it collapses to
            // the bare `new` segment) and produce a dedicated BoxNewExpr.
            if (wasPath && first.lexeme == "Box" && tok.lexeme == "new") {
                expect(TokenKind::LParen, "(");
                auto bn = std::make_unique<ast::BoxNewExpr>();
                bn->line = first.line;
                bn->column = first.column;
                bool prevRestrict = restrictStructLit_;
                restrictStructLit_ = false;
                bn->value = parseExpr();
                restrictStructLit_ = prevRestrict;
                expect(TokenKind::RParen, ")");
                return bn;
            }
            if (check(TokenKind::LParen)) {
                advance(); // consume '('
                auto call = std::make_unique<ast::CallExpr>();
                call->line = tok.line;
                call->column = tok.column;
                call->callee = tok.lexeme;
                call->wasPath = wasPath;
                if (wasPath) call->pathQualifier = prevSeg.lexeme; // Phase 48
                bool prevCallRestrict = restrictStructLit_;
                restrictStructLit_ = false;
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        call->args.push_back(parseExpr());
                        if (!accept(TokenKind::Comma)) break;
                    }
                }
                restrictStructLit_ = prevCallRestrict;
                expect(TokenKind::RParen, ")");
                return call;
            }
            if (!restrictStructLit_ && check(TokenKind::LBrace)) {
                return parseStructLit(tok);
            }
            auto e = std::make_unique<ast::IdentExpr>();
            e->line = tok.line;
            e->column = tok.column;
            e->name = tok.lexeme;
            return e;
        }

        if (t.kind == TokenKind::LParen) {
            Token lp = consume();
            bool prev = restrictStructLit_;
            restrictStructLit_ = false;
            // Phase 22: disambiguate a parenthesized expr `(e)` from a tuple
            // literal `(a, b, ...)` by the comma. `()` is unit; a single `(e)`
            // (no comma) is just the parenthesized expression; two-or-more
            // comma-separated exprs form a tuple. Note `()` is currently never
            // produced as a value elsewhere — but accepting it here keeps the
            // grammar regular (it lowers to the unit value).
            if (check(TokenKind::RParen)) {
                consume();
                restrictStructLit_ = prev;
                auto tup = std::make_unique<ast::TupleLitExpr>();
                tup->line = lp.line;
                tup->column = lp.column;
                return tup; // 0-tuple == unit
            }
            auto first = parseExpr();
            if (check(TokenKind::Comma)) {
                auto tup = std::make_unique<ast::TupleLitExpr>();
                tup->line = lp.line;
                tup->column = lp.column;
                tup->elements.push_back(std::move(first));
                while (accept(TokenKind::Comma)) {
                    if (check(TokenKind::RParen)) break; // trailing comma
                    tup->elements.push_back(parseExpr());
                }
                restrictStructLit_ = prev;
                expect(TokenKind::RParen, ")");
                return tup;
            }
            restrictStructLit_ = prev;
            expect(TokenKind::RParen, ")");
            return first;
        }

        if (t.kind == TokenKind::KwIf) {
            return parseIfExpr();
        }

        if (t.kind == TokenKind::KwMatch) {
            return parseMatchExpr();
        }

        if (t.kind == TokenKind::KwWhile) {
            return parseWhileExpr();
        }

        if (t.kind == TokenKind::KwLoop) {
            return parseLoopExpr();
        }

        if (t.kind == TokenKind::KwFor) {
            return parseForExpr();
        }

        if (t.kind == TokenKind::KwBreak) {
            Token breakTok = consume();
            auto be = std::make_unique<ast::BreakExpr>();
            be->line = breakTok.line;
            be->column = breakTok.column;
            // `break` may carry a value (`break 42`). A bare `break` is
            // followed by `;`, `}` or (rarely) the start of another stmt.
            // We treat the absence of a value-starting token as bare.
            if (!check(TokenKind::Semi) && !check(TokenKind::RBrace)) {
                be->value = parseExpr();
            }
            return be;
        }

        if (t.kind == TokenKind::KwContinue) {
            Token contTok = consume();
            auto ce = std::make_unique<ast::ContinueExpr>();
            ce->line = contTok.line;
            ce->column = contTok.column;
            return ce;
        }

        if (t.kind == TokenKind::LBrace) {
            return parseBlockExpr();
        }

        // Phase 10b: a `|` (or `||`) in primary position begins a closure.
        // There is no bitwise/logical-or operator, so a leading pipe is
        // unambiguously the start of a closure's parameter list.
        if (t.kind == TokenKind::Pipe || t.kind == TokenKind::PipePipe) {
            return parseClosureExpr();
        }

        errorHere(std::string("expected expression, got ") +
                  std::string(tokenKindName(t.kind)));
        // Synthesize a dummy node so callers don't have to null-check.
        auto e = std::make_unique<ast::IntLitExpr>();
        e->line = t.line;
        e->column = t.column;
        advance();
        return e;
    }

    // Phase 10b: `|p1, p2: T, ...| body` or `|| body`. Params may carry an
    // optional `: Type` annotation. The body is a single expression — for
    // `|x| x + n` it is the BinaryExpr; for `|x| { ... }` it is a BlockExpr.
    // We restrict struct-literal parsing inside the body's leading position
    // is unnecessary here because the body is a full expr; a `{` directly
    // after the `|...|` is parsed as a block (parsePrimary sees LBrace).
    ast::ExprPtr parseClosureExpr() {
        auto cl = std::make_unique<ast::ClosureExpr>();
        if (check(TokenKind::PipePipe)) {
            // Zero-param closure `|| body`.
            Token pipes = consume();
            cl->line = pipes.line;
            cl->column = pipes.column;
        } else {
            Token openPipe = expect(TokenKind::Pipe, "|");
            cl->line = openPipe.line;
            cl->column = openPipe.column;
            if (!check(TokenKind::Pipe)) {
                while (true) {
                    Token nameTok =
                        expect(TokenKind::Identifier, "closure parameter name");
                    ast::ClosureParam p;
                    p.name = nameTok.lexeme;
                    p.line = nameTok.line;
                    p.column = nameTok.column;
                    if (accept(TokenKind::Colon)) {
                        p.type = parseTypeRef();
                        p.hasAnnotation = true;
                    }
                    cl->params.push_back(std::move(p));
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::Pipe)) break; // trailing comma
                }
            }
            expect(TokenKind::Pipe, "|");
        }
        // The closure body is a full expression; struct literals inside it
        // are fine even when the closure itself sits in a restricted
        // position (e.g. an `if` condition), mirroring call-arg parsing.
        bool prevRestrict = restrictStructLit_;
        restrictStructLit_ = false;
        cl->body = parseExpr();
        restrictStructLit_ = prevRestrict;
        return cl;
    }

    ast::ExprPtr parseIfExpr() {
        Token ifTok = expect(TokenKind::KwIf, "if");
        bool prev = restrictStructLit_;
        restrictStructLit_ = true;
        auto cond = parseExpr();
        restrictStructLit_ = prev;
        auto thenBlock = parseBlockExpr();
        expect(TokenKind::KwElse, "else");
        // Phase 15: `else if C { ... }` chains. When the token after `else`
        // is `if`, the else branch is itself an if-expression (rather than
        // requiring `else { ... }`). This desugars `else if C { ... }` to
        // `else { if C { ... } }` — we store the nested IfExpr directly as
        // the else branch (checkIf / emitIf accept any expr there, and the
        // formatter prints the inline ladder). Arbitrarily long chains with
        // a final bare `else { ... }` fall out of the recursion.
        ast::ExprPtr elseBranch;
        if (check(TokenKind::KwIf)) {
            elseBranch = parseIfExpr();
        } else {
            elseBranch = parseBlockExpr();
        }
        auto ie = std::make_unique<ast::IfExpr>();
        ie->line = ifTok.line;
        ie->column = ifTok.column;
        ie->cond = std::move(cond);
        ie->thenBranch = std::move(thenBlock);
        ie->elseBranch = std::move(elseBranch);
        return ie;
    }

    ast::ExprPtr parseWhileExpr() {
        Token whileTok = expect(TokenKind::KwWhile, "while");
        // Restrict struct-literal parsing in the condition so the `{` that
        // opens the body isn't swallowed as a struct literal (same trick
        // as `if` / `match`).
        bool prev = restrictStructLit_;
        restrictStructLit_ = true;
        auto cond = parseExpr();
        restrictStructLit_ = prev;
        auto body = parseBlockExpr();
        auto we = std::make_unique<ast::WhileExpr>();
        we->line = whileTok.line;
        we->column = whileTok.column;
        we->cond = std::move(cond);
        we->body = std::move(body);
        return we;
    }

    ast::ExprPtr parseLoopExpr() {
        Token loopTok = expect(TokenKind::KwLoop, "loop");
        auto body = parseBlockExpr();
        auto le = std::make_unique<ast::LoopExpr>();
        le->line = loopTok.line;
        le->column = loopTok.column;
        le->body = std::move(body);
        return le;
    }

    ast::ExprPtr parseForExpr() {
        Token forTok = expect(TokenKind::KwFor, "for");
        auto pat = parsePattern();
        // `in` is not a keyword; match it by lexeme.
        if (check(TokenKind::Identifier) && peek().lexeme == "in") {
            consume();
        } else {
            errorHere("expected 'in' after for-loop pattern");
        }
        bool prev = restrictStructLit_;
        restrictStructLit_ = true;
        auto iter = parseExpr();
        restrictStructLit_ = prev;
        auto body = parseBlockExpr();
        auto fe = std::make_unique<ast::ForExpr>();
        fe->line = forTok.line;
        fe->column = forTok.column;
        fe->pattern = std::move(pat);
        fe->iter = std::move(iter);
        fe->body = std::move(body);
        return fe;
    }

    ast::ExprPtr parseMatchExpr() {
        Token matchTok = expect(TokenKind::KwMatch, "match");
        bool prev = restrictStructLit_;
        restrictStructLit_ = true;
        auto scrutinee = parseExpr();
        restrictStructLit_ = prev;
        auto me = std::make_unique<ast::MatchExpr>();
        me->line = matchTok.line;
        me->column = matchTok.column;
        me->scrutinee = std::move(scrutinee);
        expect(TokenKind::LBrace, "{");
        bool prevArm = restrictStructLit_;
        restrictStructLit_ = false;
        if (!check(TokenKind::RBrace)) {
            while (true) {
                me->arms.push_back(parseMatchArm());
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::RBrace)) break; // trailing comma
            }
        }
        restrictStructLit_ = prevArm;
        expect(TokenKind::RBrace, "}");
        // v26 Phase 141: split each or-pattern arm `p1 | p2 => e` into one arm
        // per alternative (`p1 => e, p2 => e`), deep-cloning the body so the
        // match compiler / typechecker only ever see single-pattern arms.
        std::vector<ast::MatchArm> expanded;
        for (auto& arm : me->arms) {
            auto* orp = dynamic_cast<ast::OrPat*>(arm.pattern.get());
            if (!orp) {
                expanded.push_back(std::move(arm));
                continue;
            }
            for (std::size_t i = 0; i < orp->alternatives.size(); ++i) {
                ast::MatchArm a;
                a.line = arm.line;
                a.column = arm.column;
                a.pattern = std::move(orp->alternatives[i]);
                a.body = (i + 1 == orp->alternatives.size())
                             ? std::move(arm.body)
                             : ast::cloneExpr(*arm.body);
                expanded.push_back(std::move(a));
            }
        }
        me->arms = std::move(expanded);
        // v26 Phase 143: a match with any slice-pattern arm desugars to a
        // length-checked if/else chain.
        for (auto& arm : me->arms) {
            if (dynamic_cast<ast::SlicePat*>(arm.pattern.get()))
                return desugarSliceMatch(std::move(me));
        }
        return me;
    }

    // v26 Phase 143: lower `match s { [a,b] => e1, [x] => e2, _ => e3 }` to
    //   { let __sl = s; if slice_len(__sl)==2 { let a=slice_get(__sl,0); … e1 }
    //     else if slice_len(__sl)==1 { … e2 } else e3 }
    // `[a, ..]` uses `>=` (prefix). The last arm must be a non-slice catch-all.
    ast::ExprPtr desugarSliceMatch(std::unique_ptr<ast::MatchExpr> me) {
        std::size_t ln = me->line, cn = me->column;
        if (me->arms.empty() ||
            dynamic_cast<ast::SlicePat*>(me->arms.back().pattern.get())) {
            errorHere("a slice-pattern match needs a non-slice catch-all as its "
                      "last arm");
            return std::move(me->scrutinee);
        }
        const std::string sl = "__sl" + std::to_string(structPatCounter_++);
        auto mkId = [&](const std::string& n) {
            auto e = std::make_unique<ast::IdentExpr>();
            e->name = n;
            e->line = ln;
            e->column = cn;
            return e;
        };
        auto mkInt = [&](long long v) {
            auto e = std::make_unique<ast::IntLitExpr>();
            e->value = v;
            e->line = ln;
            e->column = cn;
            return e;
        };
        auto mkCall = [&](const char* callee, ast::ExprPtr a0,
                          ast::ExprPtr a1) {
            auto c = std::make_unique<ast::CallExpr>();
            c->callee = callee;
            c->line = ln;
            c->column = cn;
            c->args.push_back(std::move(a0));
            if (a1) c->args.push_back(std::move(a1));
            return c;
        };
        ast::ExprPtr chain;
        for (int i = (int)me->arms.size() - 1; i >= 0; --i) {
            auto& arm = me->arms[i];
            auto* sp = dynamic_cast<ast::SlicePat*>(arm.pattern.get());
            if (!sp) {
                auto blk = std::make_unique<ast::BlockExpr>();
                blk->line = arm.line;
                blk->column = arm.column;
                if (auto* vp = dynamic_cast<ast::VarPat*>(arm.pattern.get())) {
                    if (vp->name != "_") {
                        auto let = std::make_unique<ast::LetStmt>();
                        let->name = vp->name;
                        let->line = arm.line;
                        let->column = arm.column;
                        let->value = mkId(sl);
                        blk->stmts.push_back(std::move(let));
                    }
                }
                blk->tail = std::move(arm.body);
                chain = std::move(blk);
                continue;
            }
            auto thenBlk = std::make_unique<ast::BlockExpr>();
            thenBlk->line = arm.line;
            thenBlk->column = arm.column;
            for (std::size_t j = 0; j < sp->elements.size(); ++j) {
                if (sp->elements[j] == "_") continue;
                auto let = std::make_unique<ast::LetStmt>();
                let->name = sp->elements[j];
                let->line = arm.line;
                let->column = arm.column;
                let->value = mkCall("slice_get", mkId(sl), mkInt((long long)j));
                thenBlk->stmts.push_back(std::move(let));
            }
            thenBlk->tail = std::move(arm.body);
            auto cmp = std::make_unique<ast::BinaryExpr>();
            cmp->line = arm.line;
            cmp->column = arm.column;
            cmp->op = sp->hasRest ? ast::BinOp::Ge : ast::BinOp::Eq;
            cmp->lhs = mkCall("slice_len", mkId(sl), nullptr);
            cmp->rhs = mkInt((long long)sp->elements.size());
            auto iff = std::make_unique<ast::IfExpr>();
            iff->line = arm.line;
            iff->column = arm.column;
            iff->cond = std::move(cmp);
            iff->thenBranch = std::move(thenBlk);
            iff->elseBranch = std::move(chain);
            chain = std::move(iff);
        }
        auto outer = std::make_unique<ast::BlockExpr>();
        outer->line = ln;
        outer->column = cn;
        auto letS = std::make_unique<ast::LetStmt>();
        letS->name = sl;
        letS->line = ln;
        letS->column = cn;
        letS->value = std::move(me->scrutinee);
        outer->stmts.push_back(std::move(letS));
        outer->tail = std::move(chain);
        return outer;
    }

    // v26 Phase 142: a struct pattern `P { f1, f2: b, _, .. }` as a match arm,
    // desugared to an irrefutable `__sp` binding + a block that field-binds —
    // so the match compiler / typechecker only ever see a VarPat + a block.
    // (`P { .. }` field-list shorthand; `f: _` / `..` skip a field.)
    ast::MatchArm parseStructPatternArm() {
        Token nameTok = consume(); // struct name (informational)
        expect(TokenKind::LBrace, "{");
        std::string sp = "__sp" + std::to_string(structPatCounter_++);
        auto block = std::make_unique<ast::BlockExpr>();
        block->line = nameTok.line;
        block->column = nameTok.column;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (check(TokenKind::DotDot)) {
                consume();
                break;
            }
            Token field = expect(TokenKind::Identifier, "field name");
            std::string bind = field.lexeme;
            bool skip = false;
            if (accept(TokenKind::Colon)) {
                if (check(TokenKind::Underscore)) {
                    consume();
                    skip = true;
                } else {
                    bind = expect(TokenKind::Identifier, "a binding").lexeme;
                }
            }
            if (!skip) {
                auto let = std::make_unique<ast::LetStmt>();
                let->name = bind;
                let->line = field.line;
                let->column = field.column;
                auto id = std::make_unique<ast::IdentExpr>();
                id->name = sp;
                id->line = field.line;
                id->column = field.column;
                auto fe = std::make_unique<ast::FieldExpr>();
                fe->line = field.line;
                fe->column = field.column;
                fe->object = std::move(id);
                fe->fieldName = field.lexeme;
                let->value = std::move(fe);
                block->stmts.push_back(std::move(let));
            }
            if (!accept(TokenKind::Comma)) break;
        }
        expect(TokenKind::RBrace, "}");
        expect(TokenKind::FatArrow, "=>");
        block->tail = parseExpr();
        auto vp = std::make_unique<ast::VarPat>();
        vp->name = sp;
        vp->line = nameTok.line;
        vp->column = nameTok.column;
        ast::MatchArm arm;
        arm.line = nameTok.line;
        arm.column = nameTok.column;
        arm.pattern = std::move(vp);
        arm.body = std::move(block);
        return arm;
    }

    // v26 Phase 143: a slice pattern `[a, b, _, ..]` as a match arm.
    ast::PatternPtr parseSlicePat() {
        Token lb = expect(TokenKind::LBracket, "[");
        auto sp = std::make_unique<ast::SlicePat>();
        sp->line = lb.line;
        sp->column = lb.column;
        while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfInput)) {
            if (check(TokenKind::DotDot)) {
                consume();
                sp->hasRest = true;
                break;
            }
            if (check(TokenKind::Underscore)) {
                consume();
                sp->elements.push_back("_");
            } else {
                sp->elements.push_back(
                    expect(TokenKind::Identifier, "a binding").lexeme);
            }
            if (!accept(TokenKind::Comma)) break;
        }
        expect(TokenKind::RBracket, "]");
        return sp;
    }

    ast::MatchArm parseMatchArm() {
        // v26 Phase 143: `[ … ]` in pattern position is a slice pattern.
        if (check(TokenKind::LBracket)) {
            auto sp = parseSlicePat();
            std::size_t l = sp->line, c = sp->column;
            expect(TokenKind::FatArrow, "=>");
            ast::MatchArm arm;
            arm.line = l;
            arm.column = c;
            arm.pattern = std::move(sp);
            arm.body = parseExpr();
            return arm;
        }
        // v26 Phase 142: `P { … }` in pattern position is a struct pattern.
        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::LBrace) {
            return parseStructPatternArm();
        }
        auto pat = parsePattern();
        std::size_t line = pat ? pat->line : peek().line;
        std::size_t col = pat ? pat->column : peek().column;
        // v26 Phase 141: an or-pattern `p1 | p2 | …`. In pattern position the
        // `|` is unambiguous (no bitwise-or / closure here). Collect the
        // alternatives into an OrPat; the expandOrPatterns pass splits the arm.
        if (check(TokenKind::Pipe) || check(TokenKind::PipePipe)) {
            auto orp = std::make_unique<ast::OrPat>();
            orp->line = line;
            orp->column = col;
            orp->alternatives.push_back(std::move(pat));
            while (check(TokenKind::Pipe) || check(TokenKind::PipePipe)) {
                // `||` here is two pattern separators (an empty alternative is
                // never valid), so treat it as a single `|`.
                consume();
                orp->alternatives.push_back(parsePattern());
            }
            pat = std::move(orp);
        }
        expect(TokenKind::FatArrow, "=>");
        auto body = parseExpr();
        ast::MatchArm arm;
        arm.line = line;
        arm.column = col;
        arm.pattern = std::move(pat);
        arm.body = std::move(body);
        return arm;
    }

    ast::PatternPtr parsePattern() {
        const Token& t = peek();
        if (t.kind == TokenKind::Integer) {
            Token tok = consume();
            auto p = std::make_unique<ast::LitIntPat>();
            p->line = tok.line;
            p->column = tok.column;
            p->value = parseIntLitLexeme(tok, nullptr, nullptr);
            return p;
        }
        // v27 Phase 147: a char-literal pattern `'a'`.
        if (t.kind == TokenKind::CharLit) {
            Token tok = consume();
            auto p = std::make_unique<ast::LitCharPat>();
            p->line = tok.line;
            p->column = tok.column;
            p->codepoint = static_cast<std::uint32_t>(std::stoul(tok.lexeme));
            return p;
        }
        if (t.kind == TokenKind::Underscore) {
            Token tok = consume();
            auto p = std::make_unique<ast::WildPat>();
            p->line = tok.line;
            p->column = tok.column;
            return p;
        }
        // Phase 36: parenthesized / tuple pattern. `(p)` is just grouping;
        // `(p0, p1, ...)` (and the empty `()`) is a tuple destructure.
        if (t.kind == TokenKind::LParen) {
            Token tok = consume();
            std::vector<ast::PatternPtr> elems;
            if (!check(TokenKind::RParen)) {
                while (true) {
                    elems.push_back(parsePattern());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break; // trailing comma
                }
            }
            expect(TokenKind::RParen, ")");
            // A single element with no comma is a grouped pattern, not a tuple.
            if (elems.size() == 1) return std::move(elems[0]);
            auto p = std::make_unique<ast::TuplePat>();
            p->line = tok.line;
            p->column = tok.column;
            p->elements = std::move(elems);
            return p;
        }
        if (t.kind == TokenKind::Identifier) {
            Token tok = consume();
            if (accept(TokenKind::LParen)) {
                auto p = std::make_unique<ast::CtorPat>();
                p->line = tok.line;
                p->column = tok.column;
                p->ctorName = tok.lexeme;
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        p->subpatterns.push_back(parsePattern());
                        if (!accept(TokenKind::Comma)) break;
                        if (check(TokenKind::RParen)) break; // trailing comma
                    }
                }
                expect(TokenKind::RParen, ")");
                return p;
            }
            auto p = std::make_unique<ast::VarPat>();
            p->line = tok.line;
            p->column = tok.column;
            p->name = tok.lexeme;
            return p;
        }
        errorHere(std::string("expected pattern, got ") +
                  std::string(tokenKindName(t.kind)));
        auto p = std::make_unique<ast::WildPat>();
        p->line = t.line;
        p->column = t.column;
        advance();
        return p;
    }

    ast::ExprPtr parseStructLit(const Token& nameTok) {
        expect(TokenKind::LBrace, "{");
        auto lit = std::make_unique<ast::StructLitExpr>();
        lit->line = nameTok.line;
        lit->column = nameTok.column;
        lit->structName = nameTok.lexeme;
        bool prev = restrictStructLit_;
        restrictStructLit_ = false;
        if (!check(TokenKind::RBrace)) {
            while (true) {
                Token fnameTok = expect(TokenKind::Identifier, "field name");
                expect(TokenKind::Colon, ":");
                auto value = parseExpr();
                lit->fields.emplace_back(fnameTok.lexeme, std::move(value));
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::RBrace)) break; // trailing comma
            }
        }
        restrictStructLit_ = prev;
        expect(TokenKind::RBrace, "}");
        return lit;
    }
};

} // namespace

ParseResult parse(std::string_view source) {
    auto tokens = lex(source);
    Parser p(std::move(tokens));
    return p.parseProgram();
}

} // namespace kardashev
