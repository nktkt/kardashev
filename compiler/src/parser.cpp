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
            } else {
                errorHere(std::string("expected 'fn' or 'struct' at top level, got ") +
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

    ast::FnDecl parseFnDecl() {
        Token fnTok = expect(TokenKind::KwFn, "fn");
        ast::FnDecl decl;
        decl.line = fnTok.line;
        decl.column = fnTok.column;

        Token nameTok = expect(TokenKind::Identifier, "function name");
        decl.name = nameTok.lexeme;

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
        decl.body = parseBlockExpr();
        return decl;
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

    ast::TypeRef parseTypeRef() {
        Token t = expect(TokenKind::Identifier, "type name");
        return {t.lexeme, t.line, t.column};
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
        while (check(TokenKind::Dot)) {
            Token dotTok = consume();
            Token nameTok = expect(TokenKind::Identifier, "field name after '.'");
            auto fe = std::make_unique<ast::FieldExpr>();
            fe->line = dotTok.line;
            fe->column = dotTok.column;
            fe->object = std::move(expr);
            fe->fieldName = nameTok.lexeme;
            expr = std::move(fe);
        }
        return expr;
    }

    ast::ExprPtr parsePrimary() {
        const Token& t = peek();

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
