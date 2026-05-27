#include "kardashev/parser.hpp"

#include "kardashev/lexer.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace kardashev {
namespace {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    ParseResult parseProgram() {
        ast::Program prog;
        while (!check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            if (check(TokenKind::KwFn)) {
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
            } else {
                errorHere(std::string("expected 'fn', 'struct', 'enum', "
                                      "'trait', 'impl' or 'mod' at top "
                                      "level, got ") +
                          std::string(tokenKindName(peek().kind)));
                advance();
            }
        }
        return {std::move(prog), std::move(errors_)};
    }

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::vector<ParseError> errors_;
    bool restrictStructLit_ = false;

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

    ast::FnDecl parseFnDecl() {
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::FnDecl decl;
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
        expect(TokenKind::Arrow, "->");
        decl.returnType = parseTypeRef();
        decl.effects = parseOptionalEffectRow();
        decl.body = parseBlockExpr();
        return decl;
    }

    // Phase 4: parse an optional `! { e1, e2, ... }` effect row. Returns
    // an EffectRow with empty `labels` when the `!` is absent — meaning
    // the function is pure. Effect labels are stored as raw strings; the
    // typechecker validates them against the built-in set + any
    // declared effect-row variables.
    ast::EffectRow parseOptionalEffectRow() {
        ast::EffectRow row;
        if (!check(TokenKind::Bang)) return row;
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
        decl.line = structTok.line;
        decl.column = structTok.column;

        Token nameTok = expect(TokenKind::Identifier, "struct name");
        decl.name = nameTok.lexeme;
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
        decl.line = enumTok.line;
        decl.column = enumTok.column;

        Token nameTok = expect(TokenKind::Identifier, "enum name");
        decl.name = nameTok.lexeme;
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
        Token t = expect(TokenKind::Identifier, "type name");
        ast::TypeRef tr;
        tr.name = t.lexeme;
        tr.isRef = isRef;
        tr.refIsMut = refIsMut;
        tr.line = isRef ? ampTok.line : t.line;
        tr.column = isRef ? ampTok.column : t.column;
        // Optional type-args: `Name<T1, T2>`. Position is unambiguous because
        // `<` immediately after an Ident in a type-ref slot can only be the
        // start of a type-arg list (the alternative — comparison — never
        // appears in a type-annotation slot, which is always preceded by
        // `:` / `->` / `(` / `,`).
        if (accept(TokenKind::Lt)) {
            if (!check(TokenKind::Gt)) {
                while (true) {
                    tr.typeArgs.push_back(parseTypeRef());
                    if (!accept(TokenKind::Comma)) break;
                    if (check(TokenKind::Gt)) break; // trailing comma
                }
            }
            expect(TokenKind::Gt, ">");
        }
        return tr;
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
                Token tpTok = expect(TokenKind::Identifier,
                                     "generic type parameter name");
                ast::TypeParam tp;
                tp.name = tpTok.lexeme;
                tp.line = tpTok.line;
                tp.column = tpTok.column;
                if (accept(TokenKind::Colon)) {
                    Token boundTok = expect(TokenKind::Identifier,
                                             "trait name after ':'");
                    tp.bound = boundTok.lexeme;
                }
                result.push_back(std::move(tp));
                if (!accept(TokenKind::Comma)) break;
                if (check(TokenKind::Gt)) break; // trailing comma
            }
        }
        expect(TokenKind::Gt, ">");
        return result;
    }

    ast::TraitDecl parseTraitDecl() {
        Token traitTok = expect(TokenKind::KwTrait, "trait");
        ast::TraitDecl decl;
        decl.line = traitTok.line;
        decl.column = traitTok.column;
        Token nameTok = expect(TokenKind::Identifier, "trait name");
        decl.name = nameTok.lexeme;
        expect(TokenKind::LBrace, "{");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            decl.methods.push_back(parseMethodSig());
        }
        expect(TokenKind::RBrace, "}");
        return decl;
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
        expect(TokenKind::Arrow, "->");
        sig.returnType = parseTypeRef();
        sig.effects = parseOptionalEffectRow();
        expect(TokenKind::Semi, ";");
        return sig;
    }

    // Trait/impl method first param is conventionally `self` (lowercase).
    // We accept either `self` (without an explicit type — it implicitly
    // becomes `Self`) or `name: Type` for additional params.
    ast::Param parseSelfOrParam() {
        const Token& t = peek();
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

    ast::ImplDecl parseImplDecl() {
        Token implTok = expect(TokenKind::KwImpl, "impl");
        ast::ImplDecl decl;
        decl.line = implTok.line;
        decl.column = implTok.column;
        Token traitTok = expect(TokenKind::Identifier, "trait name");
        decl.traitName = traitTok.lexeme;
        expect(TokenKind::KwFor, "for");
        decl.forType = parseTypeRef();
        expect(TokenKind::LBrace, "{");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;
            decl.methods.push_back(parseImplFnDecl());
        }
        expect(TokenKind::RBrace, "}");
        return decl;
    }

    // Like parseFnDecl but with parseSelfOrParam for the first param so
    // the `self` shorthand works.
    ast::FnDecl parseImplFnDecl() {
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::FnDecl decl;
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
        expect(TokenKind::Arrow, "->");
        decl.returnType = parseTypeRef();
        decl.effects = parseOptionalEffectRow();
        decl.body = parseBlockExpr();
        return decl;
    }

    // --- Block & statements ---

    std::unique_ptr<ast::BlockExpr> parseBlockExpr() {
        Token lbrace = expect(TokenKind::LBrace, "{");
        auto block = std::make_unique<ast::BlockExpr>();
        block->line = lbrace.line;
        block->column = lbrace.column;
        bool prevBlockRestrict = restrictStructLit_;
        restrictStructLit_ = false;

        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfInput)) {
            if (errors_.size() > 20) break;

            if (check(TokenKind::KwLet)) {
                block->stmts.push_back(parseLetStmt());
                continue;
            }
            if (check(TokenKind::KwReturn)) {
                block->stmts.push_back(parseReturnStmt());
                continue;
            }
            // Bare expression — either a stmt (terminated by ';') or the
            // block's tail value (no ';' before '}').
            auto expr = parseExpr();
            std::size_t line = expr->line;
            std::size_t col = expr->column;
            if (accept(TokenKind::Semi)) {
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

    ast::StmtPtr parseLetStmt() {
        Token letTok = expect(TokenKind::KwLet, "let");
        Token nameTok = expect(TokenKind::Identifier, "identifier after 'let'");
        expect(TokenKind::Eq, "=");
        auto value = parseExpr();
        expect(TokenKind::Semi, ";");
        auto stmt = std::make_unique<ast::LetStmt>();
        stmt->line = letTok.line;
        stmt->column = letTok.column;
        stmt->name = nameTok.lexeme;
        stmt->value = std::move(value);
        return stmt;
    }

    ast::StmtPtr parseReturnStmt() {
        Token retTok = expect(TokenKind::KwReturn, "return");
        auto stmt = std::make_unique<ast::ReturnStmt>();
        stmt->line = retTok.line;
        stmt->column = retTok.column;
        if (!check(TokenKind::Semi)) {
            stmt->value = parseExpr();
        }
        expect(TokenKind::Semi, ";");
        return stmt;
    }

    // --- Expressions ---

    static int binPrec(TokenKind k) {
        switch (k) {
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::Lt:
        case TokenKind::Le:
        case TokenKind::Gt:
        case TokenKind::Ge:
            return 1;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 2;
        case TokenKind::Star:
        case TokenKind::Slash:
            return 3;
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
        case TokenKind::Lt: return ast::BinOp::Lt;
        case TokenKind::Le: return ast::BinOp::Le;
        case TokenKind::Gt: return ast::BinOp::Gt;
        case TokenKind::Ge: return ast::BinOp::Ge;
        case TokenKind::EqEq: return ast::BinOp::Eq;
        case TokenKind::NotEq: return ast::BinOp::NotEq;
        default: return ast::BinOp::Add; // unreachable
        }
    }

    ast::ExprPtr parseExpr() { return parseExprPrec(1); }

    ast::ExprPtr parseExprPrec(int minPrec) {
        auto lhs = parsePostfix();
        while (true) {
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

    ast::ExprPtr parsePostfix() {
        auto expr = parsePrimary();
        while (true) {
            if (check(TokenKind::Dot)) {
                Token dotTok = consume();
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
            auto re = std::make_unique<ast::RefExpr>();
            re->line = amp.line;
            re->column = amp.column;
            re->operand = std::move(inner);
            re->isMut = isMut;
            return re;
        }

        if (t.kind == TokenKind::Integer) {
            Token tok = consume();
            auto e = std::make_unique<ast::IntLitExpr>();
            e->line = tok.line;
            e->column = tok.column;
            try {
                e->value = std::stoll(tok.lexeme);
            } catch (const std::exception&) {
                errorHere("integer literal out of range: " + tok.lexeme);
                e->value = 0;
            }
            return e;
        }

        if (t.kind == TokenKind::Identifier) {
            Token tok = consume();
            if (check(TokenKind::LParen)) {
                advance(); // consume '('
                auto call = std::make_unique<ast::CallExpr>();
                call->line = tok.line;
                call->column = tok.column;
                call->callee = tok.lexeme;
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
            advance();
            bool prev = restrictStructLit_;
            restrictStructLit_ = false;
            auto e = parseExpr();
            restrictStructLit_ = prev;
            expect(TokenKind::RParen, ")");
            return e;
        }

        if (t.kind == TokenKind::KwIf) {
            return parseIfExpr();
        }

        if (t.kind == TokenKind::KwMatch) {
            return parseMatchExpr();
        }

        if (t.kind == TokenKind::LBrace) {
            return parseBlockExpr();
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

    ast::ExprPtr parseIfExpr() {
        Token ifTok = expect(TokenKind::KwIf, "if");
        bool prev = restrictStructLit_;
        restrictStructLit_ = true;
        auto cond = parseExpr();
        restrictStructLit_ = prev;
        auto thenBlock = parseBlockExpr();
        expect(TokenKind::KwElse, "else");
        auto elseBlock = parseBlockExpr();
        auto ie = std::make_unique<ast::IfExpr>();
        ie->line = ifTok.line;
        ie->column = ifTok.column;
        ie->cond = std::move(cond);
        ie->thenBranch = std::move(thenBlock);
        ie->elseBranch = std::move(elseBlock);
        return ie;
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
        return me;
    }

    ast::MatchArm parseMatchArm() {
        auto pat = parsePattern();
        std::size_t line = pat ? pat->line : peek().line;
        std::size_t col = pat ? pat->column : peek().column;
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
            try {
                p->value = std::stoll(tok.lexeme);
            } catch (const std::exception&) {
                errorHere("integer literal out of range: " + tok.lexeme);
                p->value = 0;
            }
            return p;
        }
        if (t.kind == TokenKind::Underscore) {
            Token tok = consume();
            auto p = std::make_unique<ast::WildPat>();
            p->line = tok.line;
            p->column = tok.column;
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
