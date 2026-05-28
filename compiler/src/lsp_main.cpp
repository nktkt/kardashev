// kard-lsp — Language Server Protocol server for kardashev.
//
// Speaks LSP over stdio. Recognises the base notifications/requests needed
// to publish diagnostics in an editor PLUS the three "rich" requests added
// in Phase 14b:
//   - initialize (request)         — handshake (advertises capabilities)
//   - initialized (notification)   — no-op acknowledge
//   - textDocument/didOpen
//   - textDocument/didChange       — full-sync; we re-run the pipeline
//   - textDocument/didClose
//   - textDocument/hover           — Phase 14b: type / signature under cursor
//   - textDocument/completion      — Phase 14b: scope-aware completion items
//   - textDocument/definition      — Phase 14b: go-to-definition
//   - shutdown (request) / exit (notification)
//
// On every open / change we feed the document through the same
// lex → parse → typecheck → borrow_check pipeline kardc uses. Each
// diagnostic in the result becomes an LSP Diagnostic. For the rich requests
// we re-parse + typecheck the stored document text on demand and walk the
// AST to build a position→symbol index (and a by-name definition table),
// then answer against the cursor position.
//
// JSON is parsed by hand against the narrow shapes LSP sends. The parser is
// intentionally permissive (no schema validation) — it just looks for the
// specific fields we care about. No JSON library is linked; we reuse the
// real lexer / parser / typechecker libraries for the index.

#include "kardashev/borrow_check.hpp"
#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"
#include "kardashev/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// --- Tiny JSON helpers (consume only what we need) ---

// Find a string-valued top-level field. Returns "" if missing.
// Does naive scanning so nested objects with the same field name might
// shadow the top-level one; for LSP messages the few fields we need
// (method, uri, text) are top-level enough that this works in practice.
std::string findStringField(const std::string& json,
                             const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                   json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return "";
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char e = json[pos + 1];
            if (e == 'n') out.push_back('\n');
            else if (e == 't') out.push_back('\t');
            else if (e == '\\') out.push_back('\\');
            else if (e == '"') out.push_back('"');
            else if (e == 'r') out.push_back('\r');
            else if (e == '/') out.push_back('/');
            else if (e == 'u' && pos + 5 < json.size()) {
                // Decode \uXXXX as a single ASCII char if XXXX < 0x80;
                // otherwise just keep the bytes raw to avoid a UTF-8
                // round-trip. Editors rarely emit non-ASCII this way.
                unsigned code = 0;
                for (int k = 0; k < 4; ++k) {
                    char c = json[pos + 2 + k];
                    code <<= 4;
                    if (c >= '0' && c <= '9') code |= c - '0';
                    else if (c >= 'a' && c <= 'f') code |= 10 + c - 'a';
                    else if (c >= 'A' && c <= 'F') code |= 10 + c - 'A';
                }
                if (code < 0x80) out.push_back(static_cast<char>(code));
                pos += 4;
            } else out.push_back(e);
            pos += 2;
            continue;
        }
        out.push_back(json[pos]);
        ++pos;
    }
    return out;
}

// Find an integer-valued field (used for `id` on requests). -1 = missing.
long long findIntField(const std::string& json,
                        const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                   json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return -1;
    bool neg = false;
    if (json[pos] == '-') { neg = true; ++pos; }
    if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(
                                  json[pos]))) {
        return -1;
    }
    long long v = 0;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos]))) {
        v = v * 10 + (json[pos] - '0');
        ++pos;
    }
    return neg ? -v : v;
}

// Find an integer field that occurs *after* a given anchor key. LSP nests
// the cursor position as `"position":{"line":L,"character":C}`; a plain
// top-level search for "line" would otherwise also match a "line" elsewhere
// in the message. We anchor on "position" then read the following key.
long long findIntFieldAfter(const std::string& json, const std::string& anchor,
                            const std::string& key) {
    std::string needle = "\"" + anchor + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    return findIntField(json.substr(pos), key);
}

// Find a boolean-valued field. Returns `dflt` if the key is missing or not a
// recognizable true/false literal. Used for references' includeDeclaration.
bool findBoolField(const std::string& json, const std::string& key,
                   bool dflt) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return dflt;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                   json[pos] == '\t')) ++pos;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return dflt;
}

// Escape a string for JSON output (covers the chars LSP cares about).
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

// --- LSP framing ---

std::string readMessage() {
    std::string content;
    // Read headers up to the blank line.
    std::size_t contentLength = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const std::string prefix = "Content-Length:";
        if (line.rfind(prefix, 0) == 0) {
            try {
                contentLength =
                    static_cast<std::size_t>(std::stoll(line.substr(prefix.size())));
            } catch (...) {}
        }
        // Ignore other headers (Content-Type etc.)
    }
    if (!std::cin.good() || contentLength == 0) return "";
    content.resize(contentLength);
    std::cin.read(&content[0], static_cast<std::streamsize>(contentLength));
    return content;
}

void sendMessage(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

void sendResponse(long long id, const std::string& resultJson) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << id
       << ",\"result\":" << resultJson << "}";
    sendMessage(ss.str());
}

void sendNotification(const std::string& method,
                       const std::string& paramsJson) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"method\":\"" << method
       << "\",\"params\":" << paramsJson << "}";
    sendMessage(ss.str());
}

// --- Diagnostic conversion ---

struct Diag {
    std::size_t line;
    std::size_t column;
    std::string message;
    std::string severity; // "error"
};

void runPipeline(const std::string& src, std::vector<Diag>& out) {
    auto pr = kardashev::parse(src);
    for (const auto& e : pr.errors) {
        out.push_back({e.line, e.column, e.message, "error"});
    }
    if (!pr.ok()) return;
    auto tcr = kardashev::typecheck(pr.program);
    for (const auto& e : tcr.errors) {
        out.push_back({e.line, e.column, e.message, "error"});
    }
    if (!tcr.ok()) return;
    auto bcr = kardashev::borrow_check(pr.program, tcr);
    for (const auto& e : bcr.errors) {
        out.push_back({e.line, e.column, e.message, "error"});
    }
}

std::string buildDiagnosticsJson(const std::string& uri,
                                  const std::vector<Diag>& diags) {
    std::ostringstream ss;
    ss << "{\"uri\":\"" << jsonEscape(uri) << "\",\"diagnostics\":[";
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        // LSP positions are 0-based; our compiler uses 1-based lines
        // and columns. Translate by subtracting 1 (and guard against
        // 0 to keep the JSON well-formed).
        std::size_t line0 = d.line > 0 ? d.line - 1 : 0;
        std::size_t col0 = d.column > 0 ? d.column - 1 : 0;
        if (i > 0) ss << ',';
        ss << "{\"range\":{"
           << "\"start\":{\"line\":" << line0 << ",\"character\":" << col0 << "},"
           << "\"end\":{\"line\":" << line0 << ",\"character\":" << col0 + 1 << "}"
           << "},\"severity\":1,\"source\":\"kardc\",\"message\":\""
           << jsonEscape(d.message) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

void publishDiagnostics(const std::string& uri,
                         const std::vector<Diag>& diags) {
    sendNotification("textDocument/publishDiagnostics",
                      buildDiagnosticsJson(uri, diags));
}

// ====================================================================
// Phase 14b: rich-feature index (hover / completion / definition)
// ====================================================================
//
// Positions throughout this section are stored 1-based (matching the AST /
// lexer) and converted to 0-based only at the JSON boundary. A "range" is a
// single-line [startCol, endCol) span — every symbol we care about is an
// identifier token that never spans lines.

namespace ast = kardashev::ast;

// A definition we can jump to: a top-level decl or a local binding/param.
struct DefInfo {
    std::string name;
    std::size_t line = 1;   // 1-based, anchor of the name
    std::size_t column = 1; // 1-based
    std::size_t nameLen = 0;
    std::string hover;      // markdown body (without code fences)
};

// One identifier occurrence in the source, with what it resolves to.
struct Occurrence {
    std::size_t line = 1;     // 1-based
    std::size_t startCol = 1; // 1-based, inclusive
    std::size_t endCol = 1;   // 1-based, exclusive
    std::string name;
    std::string hover;        // markdown body for this occurrence
    // Definition location (0 if unknown — e.g. a builtin / unresolved name).
    std::size_t defLine = 0;
    std::size_t defCol = 0;
    std::size_t defLen = 0;
};

// Completion item: a label + LSP CompletionItemKind + optional detail.
struct CompletionEntry {
    std::string label;
    int kind = 0;
    std::string detail;
};

struct DocIndex {
    std::vector<Occurrence> occurrences;
    // Top-level definitions keyed by name (functions, structs, enums,
    // traits, enum variants). Locals are resolved per-function via localsByFn.
    std::unordered_map<std::string, DefInfo> globals;
    // Top-level completion entries (functions carry their effect-aware
    // signature as `detail`).
    std::vector<CompletionEntry> globalCompletions;
    // Per-function local bindings (params + lets + loop/closure binders), in
    // source order. Used for both completion and definition of locals.
    std::unordered_map<const ast::FnDecl*, std::vector<DefInfo>> localsByFn;
};

// Forward decl: renderFnSignature's AST fallback path renders param/return
// TypeRefs (defined just below).
std::string renderTypeRef(const ast::TypeRef& t);

// Render a function's full signature INCLUDING its effect row. We build the
// printable form from the resolved schema if available (so effect-row
// variables that unification solved show up), otherwise fall back to the AST
// decl. The effect row is this language's headline signature feature, so
// hover must surface it (e.g. `fn f(i64) -> i64 ! { io }`).
std::string renderFnSignature(const ast::FnDecl& fn,
                              const kardashev::TypeCheckResult& tc) {
    auto it = tc.fnSchemas.find(fn.name);
    if (it != tc.fnSchemas.end() && it->second.signature) {
        // `typeToString` prints the `fn(args) -> ret` skeleton. The
        // typechecker stores a user fn's declared effects in the SCHEMA's
        // `declaredEffects` (an EffectSet), NOT on the signature Type's own
        // effect row (which it builds with the 2-arg, pure `makeFunction`).
        // So we render the skeleton from the type, then append the effect row
        // from `declaredEffects` — this is what surfaces `! { io }` on hover,
        // the language's headline signature feature.
        std::string skel = kardashev::typeToString(it->second.signature);
        std::string sig = skel.rfind("fn", 0) == 0
                              ? "fn " + fn.name + skel.substr(2)
                              : "fn " + fn.name + skel;
        const auto& labels = it->second.declaredEffects.labels;
        if (!labels.empty()) {
            sig += " ! { ";
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (i > 0) sig += ", ";
                sig += labels[i];
            }
            sig += " }";
        }
        return sig;
    }
    // Fallback: reconstruct from the AST (used when typecheck failed before
    // recording a schema for this fn).
    std::string sig = "fn " + fn.name + "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += fn.params[i].name + ": " + renderTypeRef(fn.params[i].type);
    }
    sig += ") -> " + renderTypeRef(fn.returnType);
    if (!fn.effects.labels.empty()) {
        sig += " ! { ";
        for (std::size_t i = 0; i < fn.effects.labels.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += fn.effects.labels[i];
        }
        sig += " }";
    }
    return sig;
}

// Render a TypeRef to a short string (for param / let hover when no inferred
// type is available).
std::string renderTypeRef(const ast::TypeRef& t) {
    if (t.isFn) {
        std::string s = "fn(";
        for (std::size_t i = 0; i < t.fnParams.size(); ++i) {
            if (i > 0) s += ", ";
            s += renderTypeRef(t.fnParams[i]);
        }
        s += ")";
        if (t.fnRet) s += " -> " + renderTypeRef(*t.fnRet);
        if (!t.fnEffects.empty()) {
            s += " ! { ";
            for (std::size_t i = 0; i < t.fnEffects.size(); ++i) {
                if (i > 0) s += ", ";
                s += t.fnEffects[i];
            }
            s += " }";
        }
        return s;
    }
    std::string s;
    if (t.isRef) s += t.refIsMut ? "&mut " : "&";
    if (t.isDyn) s += "dyn ";
    s += t.name;
    if (!t.typeArgs.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.typeArgs.size(); ++i) {
            if (i > 0) s += ", ";
            s += renderTypeRef(t.typeArgs[i]);
        }
        s += ">";
    }
    return s;
}

std::string renderStructDecl(const ast::StructDecl& s) {
    std::string out = "struct " + s.name + " { ";
    for (std::size_t i = 0; i < s.fields.size(); ++i) {
        if (i > 0) out += ", ";
        out += s.fields[i].name + ": " + renderTypeRef(s.fields[i].type);
    }
    out += " }";
    return out;
}

std::string renderEnumDecl(const ast::EnumDecl& e) {
    std::string out = "enum " + e.name + " { ";
    for (std::size_t i = 0; i < e.variants.size(); ++i) {
        if (i > 0) out += ", ";
        out += e.variants[i].name;
        if (!e.variants[i].payloadTypes.empty()) {
            out += "(";
            for (std::size_t j = 0; j < e.variants[i].payloadTypes.size(); ++j) {
                if (j > 0) out += ", ";
                out += renderTypeRef(e.variants[i].payloadTypes[j]);
            }
            out += ")";
        }
    }
    out += " }";
    return out;
}

std::string renderTraitDecl(const ast::TraitDecl& t) {
    std::string out = "trait " + t.name + " { ";
    for (std::size_t i = 0; i < t.methods.size(); ++i) {
        if (i > 0) out += "; ";
        out += "fn " + t.methods[i].name + "(...) -> " +
               renderTypeRef(t.methods[i].returnType);
    }
    out += " }";
    return out;
}

// Walks the AST collecting identifier occurrences (with hover + def site) and
// each function's local bindings.
class IndexBuilder {
public:
    IndexBuilder(const ast::Program& prog,
                 const kardashev::TypeCheckResult& tc, DocIndex& out,
                 const std::string& src)
        : prog_(prog), tc_(tc), out_(out) {
        splitLines(src);
    }

    void build() {
        collectGlobals();
        for (const auto& fn : prog_.functions) walkFn(fn);
        for (const auto& impl : prog_.impls)
            for (const auto& m : impl.methods) walkFn(m);
    }

private:
    // The AST records a decl's anchor at its KEYWORD token (`fn`/`let`/
    // `struct`/…), not at the bound NAME. For go-to-definition that's fine
    // (it lands on the right line), but references/rename must point a range
    // at the name's exact columns — otherwise a rename would corrupt the
    // keyword. We recover the name's precise 1-based column by scanning the
    // stored source line for `name` as a whole word at/after the anchor
    // column. Falls back to the anchor column if the line isn't available or
    // the name isn't found (keeps behavior safe, never worse than before).
    void splitLines(const std::string& src) {
        std::string cur;
        for (char c : src) {
            if (c == '\n') { lines_.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(c);
        }
        lines_.push_back(cur);
    }

    static bool isIdentChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // Resolve the precise 1-based column of `name` on 1-based `line`, searching
    // at/after the 1-based `fromCol`. Returns `fromCol` unchanged on any miss.
    std::size_t resolveNameColumn(std::size_t line, std::size_t fromCol,
                                  const std::string& name) const {
        if (name.empty() || line == 0 || line > lines_.size()) return fromCol;
        const std::string& text = lines_[line - 1];
        std::size_t start = fromCol > 0 ? fromCol - 1 : 0; // -> 0-based
        if (start > text.size()) return fromCol;
        std::size_t at = start;
        while ((at = text.find(name, at)) != std::string::npos) {
            bool leftOk = at == 0 || !isIdentChar(text[at - 1]);
            std::size_t after = at + name.size();
            bool rightOk = after >= text.size() || !isIdentChar(text[after]);
            if (leftOk && rightOk) return at + 1; // -> 1-based
            ++at;
        }
        return fromCol;
    }
    void collectGlobals() {
        for (const auto& fn : prog_.functions) {
            std::size_t col = resolveNameColumn(fn.line, fn.column, fn.name);
            DefInfo d{fn.name, fn.line, col, fn.name.size(),
                      renderFnSignature(fn, tc_)};
            out_.globals[fn.name] = d;
            out_.globalCompletions.push_back({fn.name, /*Function*/ 3, d.hover});
        }
        for (const auto& s : prog_.structs) {
            std::size_t col = resolveNameColumn(s.line, s.column, s.name);
            DefInfo d{s.name, s.line, col, s.name.size(),
                      renderStructDecl(s)};
            out_.globals[s.name] = d;
            out_.globalCompletions.push_back({s.name, /*Struct*/ 22, d.hover});
        }
        for (const auto& e : prog_.enums) {
            std::size_t col = resolveNameColumn(e.line, e.column, e.name);
            DefInfo d{e.name, e.line, col, e.name.size(),
                      renderEnumDecl(e)};
            out_.globals[e.name] = d;
            out_.globalCompletions.push_back({e.name, /*Enum*/ 13, d.hover});
            for (const auto& v : e.variants) {
                if (out_.globals.find(v.name) == out_.globals.end()) {
                    std::size_t vcol =
                        resolveNameColumn(v.line, v.column, v.name);
                    out_.globals[v.name] = DefInfo{v.name, v.line, vcol,
                                                   v.name.size(),
                                                   e.name + "::" + v.name};
                }
                out_.globalCompletions.push_back(
                    {v.name, /*EnumMember*/ 20, e.name});
            }
        }
        for (const auto& t : prog_.traits) {
            std::size_t col = resolveNameColumn(t.line, t.column, t.name);
            DefInfo d{t.name, t.line, col, t.name.size(),
                      renderTraitDecl(t)};
            out_.globals[t.name] = d;
            out_.globalCompletions.push_back({t.name, /*Interface*/ 8, d.hover});
        }
    }

    void walkFn(const ast::FnDecl& fn) {
        curFn_ = &fn;
        auto& locals = out_.localsByFn[&fn];
        locals.clear();
        // Seed params FIRST so a use anywhere in the body resolves to them.
        // The parser doesn't store a per-param position, so we anchor the def
        // at the fn-decl line/column then scan forward for the param name's
        // precise column on that line (so references/rename land on the param,
        // not the `fn` keyword). Hover shows the declared type.
        for (const auto& p : fn.params) {
            std::size_t col = resolveNameColumn(fn.line, fn.column, p.name);
            locals.push_back(DefInfo{p.name, fn.line, col, p.name.size(),
                                     p.name + ": " + renderTypeRef(p.type)});
        }
        if (fn.body) walkExpr(*fn.body);
    }

    void addLocal(const std::string& name, std::size_t line, std::size_t col,
                  const std::string& hover) {
        // Anchor columns from a binding keyword (`let`/`for`) or binder token
        // may precede the bound name; scan forward to the name's real column
        // so references/rename target the identifier, not the keyword.
        std::size_t ncol = resolveNameColumn(line, col, name);
        out_.localsByFn[curFn_].push_back(
            DefInfo{name, line, ncol, name.size(), hover});
    }

    // Inferred type of an expression, printable (empty if unknown).
    std::string inferredType(const ast::Expr* e) const {
        auto it = tc_.exprTypes.find(e);
        if (it == tc_.exprTypes.end() || !it->second) return "";
        return kardashev::typeToString(it->second);
    }

    // Resolve a use of `name` as seen at (line,col): nearest preceding local
    // binding in the current fn wins; else a top-level decl.
    const DefInfo* lookupDef(const std::string& name, std::size_t line,
                             std::size_t col) const {
        const DefInfo* best = nullptr;
        if (curFn_) {
            auto it = out_.localsByFn.find(curFn_);
            if (it != out_.localsByFn.end()) {
                for (const auto& d : it->second) {
                    if (d.name != name) continue;
                    bool before = d.line < line ||
                                  (d.line == line && d.column <= col);
                    if (before) best = &d; // last one wins
                }
            }
        }
        if (best) return best;
        auto g = out_.globals.find(name);
        if (g != out_.globals.end()) return &g->second;
        return nullptr;
    }

    void addOccurrence(std::size_t line, std::size_t col,
                       const std::string& name, const std::string& hover) {
        if (name.empty()) return;
        Occurrence occ;
        occ.line = line;
        occ.startCol = col;
        occ.endCol = col + name.size();
        occ.name = name;
        occ.hover = hover;
        const DefInfo* def = lookupDef(name, line, col);
        if (def) {
            occ.defLine = def->line;
            occ.defCol = def->column;
            occ.defLen = def->nameLen > 0 ? def->nameLen : name.size();
            if (occ.hover.empty()) occ.hover = def->hover;
        }
        out_.occurrences.push_back(std::move(occ));
    }

    void walkExpr(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
            std::string hov = inferredType(&e);
            std::string body = hov.empty() ? "" : (id->name + ": " + hov);
            addOccurrence(id->line, id->column, id->name, body);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // Callee identifier — prefer the fn signature (with effect row).
            std::string body;
            auto git = out_.globals.find(call->callee);
            if (git != out_.globals.end()) body = git->second.hover;
            addOccurrence(call->line, call->column, call->callee, body);
            for (const auto& a : call->args) walkExpr(*a);
            return;
        }
        if (auto* cv = dynamic_cast<const ast::CallValueExpr*>(&e)) {
            // Phase 17a: indirect call through a fn-value expression.
            if (cv->callee) walkExpr(*cv->callee);
            for (const auto& a : cv->args) walkExpr(*a);
            return;
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (bin->lhs) walkExpr(*bin->lhs);
            if (bin->rhs) walkExpr(*bin->rhs);
            return;
        }
        if (auto* sl = dynamic_cast<const ast::StructLitExpr*>(&e)) {
            std::string body;
            auto git = out_.globals.find(sl->structName);
            if (git != out_.globals.end()) body = git->second.hover;
            addOccurrence(sl->line, sl->column, sl->structName, body);
            for (const auto& f : sl->fields)
                if (f.second) walkExpr(*f.second);
            return;
        }
        if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e)) {
            if (fe->object) walkExpr(*fe->object);
            return;
        }
        if (auto* mc = dynamic_cast<const ast::MethodCallExpr*>(&e)) {
            if (mc->receiver) walkExpr(*mc->receiver);
            for (const auto& a : mc->args) walkExpr(*a);
            return;
        }
        if (auto* ie = dynamic_cast<const ast::IfExpr*>(&e)) {
            if (ie->cond) walkExpr(*ie->cond);
            if (ie->thenBranch) walkExpr(*ie->thenBranch);
            if (ie->elseBranch) walkExpr(*ie->elseBranch);
            return;
        }
        if (auto* blk = dynamic_cast<const ast::BlockExpr*>(&e)) {
            for (const auto& s : blk->stmts) walkStmt(*s);
            if (blk->tail) walkExpr(*blk->tail);
            return;
        }
        if (auto* me = dynamic_cast<const ast::MatchExpr*>(&e)) {
            if (me->scrutinee) walkExpr(*me->scrutinee);
            for (const auto& arm : me->arms) {
                // Bind any variable subpatterns in the arm pattern.
                bindPattern(arm.pattern.get());
                if (arm.body) walkExpr(*arm.body);
            }
            return;
        }
        if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e)) {
            if (we->cond) walkExpr(*we->cond);
            if (we->body) walkExpr(*we->body);
            return;
        }
        if (auto* le = dynamic_cast<const ast::LoopExpr*>(&e)) {
            if (le->body) walkExpr(*le->body);
            return;
        }
        if (auto* fr = dynamic_cast<const ast::ForExpr*>(&e)) {
            if (auto* vp = dynamic_cast<const ast::VarPat*>(fr->pattern.get()))
                addLocal(vp->name, vp->line, vp->column, vp->name + ": i64");
            if (fr->iter) walkExpr(*fr->iter);
            if (fr->body) walkExpr(*fr->body);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            if (re->start) walkExpr(*re->start);
            if (re->end) walkExpr(*re->end);
            return;
        }
        if (auto* re = dynamic_cast<const ast::RefExpr*>(&e)) {
            if (re->operand) walkExpr(*re->operand);
            return;
        }
        if (auto* te = dynamic_cast<const ast::TryExpr*>(&e)) {
            if (te->operand) walkExpr(*te->operand);
            return;
        }
        if (auto* ae = dynamic_cast<const ast::AwaitExpr*>(&e)) {
            if (ae->operand) walkExpr(*ae->operand);
            return;
        }
        if (auto* se = dynamic_cast<const ast::SliceExpr*>(&e)) {
            if (se->operand) walkExpr(*se->operand);
            if (se->start) walkExpr(*se->start);
            if (se->end) walkExpr(*se->end);
            return;
        }
        // Phase 22: array / tuple literals + index / tuple-field access.
        if (auto* al = dynamic_cast<const ast::ArrayLitExpr*>(&e)) {
            for (const auto& el : al->elements) walkExpr(*el);
            return;
        }
        if (auto* tl = dynamic_cast<const ast::TupleLitExpr*>(&e)) {
            for (const auto& el : tl->elements) walkExpr(*el);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->object) walkExpr(*ix->object);
            if (ix->index) walkExpr(*ix->index);
            return;
        }
        if (auto* tf = dynamic_cast<const ast::TupleFieldExpr*>(&e)) {
            if (tf->object) walkExpr(*tf->object);
            return;
        }
        if (auto* be = dynamic_cast<const ast::BreakExpr*>(&e)) {
            if (be->value) walkExpr(*be->value);
            return;
        }
        if (auto* bn = dynamic_cast<const ast::BoxNewExpr*>(&e)) {
            if (bn->value) walkExpr(*bn->value);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) {
            for (const auto& p : cl->params)
                addLocal(p.name, p.line, p.column, p.name);
            if (cl->body) walkExpr(*cl->body);
            return;
        }
        // IntLitExpr / StringLitExpr / ContinueExpr: nothing to index.
    }

    // Bind variable patterns (and ctor subpatterns) in a match arm as locals
    // so their uses in the arm body resolve / hover.
    void bindPattern(const ast::Pattern* p) {
        if (!p) return;
        if (auto* vp = dynamic_cast<const ast::VarPat*>(p)) {
            addLocal(vp->name, vp->line, vp->column, vp->name);
        } else if (auto* cp = dynamic_cast<const ast::CtorPat*>(p)) {
            for (const auto& sp : cp->subpatterns) bindPattern(sp.get());
        }
    }

    void walkStmt(const ast::Stmt& s) {
        if (auto* ls = dynamic_cast<const ast::LetStmt*>(&s)) {
            if (ls->value) walkExpr(*ls->value); // RHS sees prior bindings
            std::string kw = ls->isMut ? "let mut " : "let ";
            // Phase 22: tuple-destructuring binds each element name as a local.
            if (!ls->tupleNames.empty()) {
                for (const auto& nm : ls->tupleNames) {
                    if (nm == "_") continue;
                    addLocal(nm, ls->line, ls->column, kw + nm);
                }
                return;
            }
            std::string ty = ls->value ? inferredType(ls->value.get()) : "";
            std::string hov = ls->name;
            if (!ty.empty()) hov += ": " + ty;
            else if (ls->annotation) hov += ": " + renderTypeRef(*ls->annotation);
            addLocal(ls->name, ls->line, ls->column, kw + hov);
            return;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (rs->value) walkExpr(*rs->value);
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
            if (es->expr) walkExpr(*es->expr);
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignStmt*>(&s)) {
            if (as->target) walkExpr(*as->target);
            if (as->value) walkExpr(*as->value);
            return;
        }
    }

    const ast::Program& prog_;
    const kardashev::TypeCheckResult& tc_;
    DocIndex& out_;
    const ast::FnDecl* curFn_ = nullptr;
    std::vector<std::string> lines_; // source split for precise name columns
};

// Build the document index from raw source text. We keep the ParseResult and
// TypeCheckResult alive in the caller so exprTypes (keyed by Expr*) stay valid
// for the duration of the request.
void buildIndex(const std::string& src, DocIndex& out,
                kardashev::ParseResult& prKeep,
                kardashev::TypeCheckResult& tcKeep) {
    prKeep = kardashev::parse(src);
    // typecheck populates exprTypes / fnSchemas. Even on type errors it
    // records what it could resolve, enough for best-effort hover.
    tcKeep = kardashev::typecheck(prKeep.program);
    IndexBuilder b(prKeep.program, tcKeep, out, src);
    b.build();
}

// Find the narrowest occurrence whose range contains the 1-based cursor.
const Occurrence* occurrenceAt(const DocIndex& idx, std::size_t line,
                               std::size_t col) {
    const Occurrence* best = nullptr;
    for (const auto& o : idx.occurrences) {
        if (o.line != line) continue;
        if (col < o.startCol || col >= o.endCol) continue;
        if (!best || (o.endCol - o.startCol) < (best->endCol - best->startCol))
            best = &o;
    }
    return best;
}

const std::vector<std::string>& keywords() {
    static const std::vector<std::string> kw = {
        "fn", "let", "mut", "if", "else", "match", "while", "loop", "for",
        "break", "continue", "return", "async", "await", "impl", "trait",
        "struct", "enum", "dyn", "mod", "pub", "true", "false"};
    return kw;
}

// ====================================================================
// Phase 20b: references / rename — symbol resolution over the occurrence index
// ====================================================================
//
// A symbol's identity is its DEFINITION SITE: the (line,column) of the decl it
// resolves to. Each Occurrence already carries that as (defLine, defCol) — the
// Phase 14b index computed it via lookupDef (nearest-preceding local wins, else
// a top-level decl). So "find all references to the symbol under the cursor"
// reduces to: (1) find the symbol's canonical def site, then (2) collect every
// occurrence that resolves to the same site.
//
// Note the declaration site itself is generally NOT in idx.occurrences — a
// `let x = …` / parameter / `for x in …` binder is recorded only as a DefInfo
// (via addLocal), never as an Occurrence. So we synthesize the declaration's
// Location separately (and gate it on includeDeclaration / the rename always
// rewriting it).

// A resolved symbol identity: its definition coordinates (1-based) plus the
// name + the def length (for the declaration range). `defLine == 0` means the
// occurrence had no resolvable definition (a builtin / unresolved name); we
// then fall back to identity-by-name-at-that-occurrence so a local rename still
// does something sensible rather than nothing.
struct SymbolKey {
    bool valid = false;
    std::string name;
    std::size_t defLine = 0; // 0 = unresolved (builtin / unknown)
    std::size_t defCol = 0;
    std::size_t defLen = 0;
};

// Resolve the symbol under the 1-based cursor. Works whether the cursor is on a
// USE (an occurrence with a def site) or directly on a DECLARATION (which isn't
// an occurrence — we scan the occurrence list for any use whose def site spans
// the cursor, then adopt that def site).
SymbolKey symbolAt(const DocIndex& idx, std::size_t line, std::size_t col) {
    SymbolKey key;
    // Case 1: cursor is on an occurrence (a use, or a global-decl name that the
    // walker also records as an occurrence — e.g. a struct name at a literal).
    if (const Occurrence* o = occurrenceAt(idx, line, col)) {
        key.valid = true;
        key.name = o->name;
        if (o->defLine != 0) {
            key.defLine = o->defLine;
            key.defCol = o->defCol;
            key.defLen = o->defLen > 0 ? o->defLen : o->name.size();
        } else {
            // Unresolved: treat the occurrence position itself as the anchor so
            // we can still group same-named occurrences with no def.
            key.defLine = 0;
            key.defCol = 0;
            key.defLen = o->name.size();
        }
        return key;
    }
    // Case 2: cursor sits on a declaration name that is not itself an
    // occurrence (a `let` / param / match-binder, or a top-level decl whose
    // name token the walker didn't emit as an occurrence). Find an occurrence
    // whose resolved DEF range contains the cursor and adopt its def site.
    for (const auto& o : idx.occurrences) {
        if (o.defLine != line) continue;
        std::size_t dStart = o.defCol;
        std::size_t dEnd = o.defCol + (o.defLen > 0 ? o.defLen : o.name.size());
        if (col < dStart || col >= dEnd) continue;
        key.valid = true;
        key.name = o.name;
        key.defLine = o.defLine;
        key.defCol = o.defCol;
        key.defLen = o.defLen > 0 ? o.defLen : o.name.size();
        return key;
    }
    return key; // invalid
}

// Does occurrence `o` refer to the symbol identified by `key`?
bool occurrenceMatches(const Occurrence& o, const SymbolKey& key) {
    if (o.name != key.name) return false;
    if (key.defLine != 0) {
        // Resolved symbol: match by def site (this is what disambiguates two
        // distinct locals that happen to share a name across functions).
        return o.defLine == key.defLine && o.defCol == key.defCol;
    }
    // Unresolved symbol: best-effort match by name + also-unresolved.
    return o.defLine == 0;
}

// Emit a single-line LSP Range [startCol0, endCol0) on `line0` (all 0-based).
void appendRange(std::ostringstream& ss, std::size_t line0,
                 std::size_t startCol0, std::size_t endCol0) {
    ss << "{\"start\":{\"line\":" << line0 << ",\"character\":" << startCol0
       << "},\"end\":{\"line\":" << line0 << ",\"character\":" << endCol0
       << "}}";
}

// Collect the 1-based reference ranges for `key`: every occurrence that matches
// it, plus (when includeDecl is set and the def site is known) the declaration
// site itself. Returned as (line, startCol, endCol) triples, deduplicated, so a
// declaration that is ALSO an occurrence isn't listed twice.
struct RefRange {
    std::size_t line;
    std::size_t startCol;
    std::size_t endCol;
};

std::vector<RefRange> collectReferences(const DocIndex& idx,
                                        const SymbolKey& key,
                                        bool includeDecl) {
    std::vector<RefRange> ranges;
    auto already = [&](std::size_t l, std::size_t s) {
        for (const auto& r : ranges)
            if (r.line == l && r.startCol == s) return true;
        return false;
    };
    if (includeDecl && key.defLine != 0) {
        std::size_t len = key.defLen > 0 ? key.defLen : key.name.size();
        ranges.push_back({key.defLine, key.defCol, key.defCol + len});
    }
    for (const auto& o : idx.occurrences) {
        if (!occurrenceMatches(o, key)) continue;
        if (already(o.line, o.startCol)) continue;
        ranges.push_back({o.line, o.startCol, o.endCol});
    }
    return ranges;
}

std::string buildReferencesResult(const DocIndex& idx, const std::string& uri,
                                  std::size_t line, std::size_t col,
                                  bool includeDecl) {
    SymbolKey key = symbolAt(idx, line, col);
    if (!key.valid) return "[]";
    auto ranges = collectReferences(idx, key, includeDecl);
    if (ranges.empty()) return "[]";
    std::ostringstream ss;
    ss << "[";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0) ss << ',';
        ss << "{\"uri\":\"" << jsonEscape(uri) << "\",\"range\":";
        appendRange(ss, ranges[i].line - 1, ranges[i].startCol - 1,
                    ranges[i].endCol - 1);
        ss << "}";
    }
    ss << "]";
    return ss.str();
}

// Rename is single-document scope (MVP): we return a WorkspaceEdit whose only
// keyed URI is the active document. Every occurrence + the declaration site is
// rewritten to `newName` (the declaration is ALWAYS included in a rename —
// unlike references, where it's gated on includeDeclaration).
std::string buildRenameResult(const DocIndex& idx, const std::string& uri,
                              std::size_t line, std::size_t col,
                              const std::string& newName) {
    SymbolKey key = symbolAt(idx, line, col);
    if (!key.valid) return "null";
    auto ranges = collectReferences(idx, key, /*includeDecl=*/true);
    if (ranges.empty()) return "null";
    std::ostringstream ss;
    ss << "{\"changes\":{\"" << jsonEscape(uri) << "\":[";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0) ss << ',';
        ss << "{\"range\":";
        appendRange(ss, ranges[i].line - 1, ranges[i].startCol - 1,
                    ranges[i].endCol - 1);
        ss << ",\"newText\":\"" << jsonEscape(newName) << "\"}";
    }
    ss << "]}}";
    return ss.str();
}

// --- Request handlers ---

std::string buildHoverResult(const DocIndex& idx, std::size_t line,
                             std::size_t col) {
    const Occurrence* o = occurrenceAt(idx, line, col);
    if (!o || o->hover.empty()) return "null";
    std::string md = "```kardashev\n" + o->hover + "\n```";
    std::ostringstream ss;
    ss << "{\"contents\":{\"kind\":\"markdown\",\"value\":\""
       << jsonEscape(md) << "\"},"
       << "\"range\":{\"start\":{\"line\":" << (o->line - 1)
       << ",\"character\":" << (o->startCol - 1) << "},"
       << "\"end\":{\"line\":" << (o->line - 1)
       << ",\"character\":" << (o->endCol - 1) << "}}}";
    return ss.str();
}

std::string buildDefinitionResult(const DocIndex& idx, const std::string& uri,
                                  std::size_t line, std::size_t col) {
    const Occurrence* o = occurrenceAt(idx, line, col);
    if (!o || o->defLine == 0) return "null";
    std::size_t l0 = o->defLine - 1;
    std::size_t c0 = o->defCol > 0 ? o->defCol - 1 : 0;
    std::size_t len = o->defLen > 0 ? o->defLen : o->name.size();
    std::ostringstream ss;
    ss << "{\"uri\":\"" << jsonEscape(uri) << "\",\"range\":{"
       << "\"start\":{\"line\":" << l0 << ",\"character\":" << c0 << "},"
       << "\"end\":{\"line\":" << l0 << ",\"character\":" << (c0 + len)
       << "}}}";
    return ss.str();
}

std::string buildCompletionResult(const DocIndex& idx,
                                  const ast::Program& prog, std::size_t line) {
    std::ostringstream ss;
    ss << "{\"isIncomplete\":false,\"items\":[";
    bool first = true;
    auto emit = [&](const std::string& label, int kind,
                    const std::string& detail) {
        if (label.empty()) return;
        if (!first) ss << ',';
        first = false;
        ss << "{\"label\":\"" << jsonEscape(label) << "\",\"kind\":" << kind;
        if (!detail.empty())
            ss << ",\"detail\":\"" << jsonEscape(detail) << "\"";
        ss << "}";
    };

    // Locals + params of the enclosing function (best-effort: the last decl
    // whose line is at/before the cursor).
    const ast::FnDecl* enclosing = nullptr;
    for (const auto& fn : prog.functions) {
        if (fn.line <= line && (!enclosing || fn.line > enclosing->line))
            enclosing = &fn;
    }
    if (enclosing) {
        auto it = idx.localsByFn.find(enclosing);
        if (it != idx.localsByFn.end())
            for (const auto& d : it->second)
                emit(d.name, /*Variable*/ 6, d.hover);
    }

    // Top-level names (functions carry their effect-aware signature detail).
    for (const auto& c : idx.globalCompletions) emit(c.label, c.kind, c.detail);
    // Keywords.
    for (const auto& k : keywords()) emit(k, /*Keyword*/ 14, "");
    ss << "]}";
    return ss.str();
}

} // namespace

int main() {
    // Per-URI buffered text (full-sync model).
    std::unordered_map<std::string, std::string> docs;
    bool shutdownRequested = false;

    while (true) {
        std::string msg = readMessage();
        if (msg.empty()) break;
        std::string method = findStringField(msg, "method");
        long long id = findIntField(msg, "id");

        if (method == "initialize") {
            // Advertise document sync (full text, kind=1), diagnostics, and
            // the Phase 14b rich providers (hover / completion / definition).
            sendResponse(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"renameProvider\":true,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                "\"diagnosticProvider\":{\"interFileDependencies\":false,"
                                        "\"workspaceDiagnostics\":false}"
                "},\"serverInfo\":{\"name\":\"kard-lsp\","
                                  "\"version\":\"0.3\"}}");
        } else if (method == "initialized") {
            // no-op
        } else if (method == "textDocument/didOpen" ||
                   method == "textDocument/didChange") {
            // didOpen: params.textDocument.text
            // didChange: params.contentChanges[0].text (full-sync)
            std::string uri = findStringField(msg, "uri");
            std::string text = findStringField(msg, "text");
            if (uri.empty()) continue;
            docs[uri] = text;
            std::vector<Diag> diags;
            runPipeline(text, diags);
            publishDiagnostics(uri, diags);
        } else if (method == "textDocument/didClose") {
            std::string uri = findStringField(msg, "uri");
            docs.erase(uri);
            // Clear diagnostics for the now-closed file.
            publishDiagnostics(uri, {});
        } else if (method == "textDocument/hover" ||
                   method == "textDocument/completion" ||
                   method == "textDocument/definition" ||
                   method == "textDocument/references" ||
                   method == "textDocument/rename") {
            // All five share: params.textDocument.uri + params.position.
            std::string uri = findStringField(msg, "uri");
            // LSP positions are 0-based; convert to the 1-based scheme our
            // index uses. Anchor the line/character search on "position".
            long long pline = findIntFieldAfter(msg, "position", "line");
            long long pchar = findIntFieldAfter(msg, "position", "character");
            if (pline < 0) pline = 0;
            if (pchar < 0) pchar = 0;
            std::size_t line1 = static_cast<std::size_t>(pline) + 1;
            std::size_t col1 = static_cast<std::size_t>(pchar) + 1;

            auto dit = docs.find(uri);
            if (dit == docs.end()) {
                sendResponse(id, "null");
                continue;
            }
            DocIndex idx;
            kardashev::ParseResult pr;
            kardashev::TypeCheckResult tc;
            buildIndex(dit->second, idx, pr, tc);

            if (method == "textDocument/hover") {
                sendResponse(id, buildHoverResult(idx, line1, col1));
            } else if (method == "textDocument/completion") {
                sendResponse(id, buildCompletionResult(idx, pr.program, line1));
            } else if (method == "textDocument/definition") {
                sendResponse(id, buildDefinitionResult(idx, uri, line1, col1));
            } else if (method == "textDocument/references") {
                // params.context.includeDeclaration controls whether the
                // declaration site is part of the result (default true if the
                // client omitted the context, which is lenient but harmless).
                bool includeDecl =
                    findBoolField(msg, "includeDeclaration", true);
                sendResponse(id, buildReferencesResult(idx, uri, line1, col1,
                                                       includeDecl));
            } else { // textDocument/rename
                // params.newName is a top-level string under params. The
                // declaration is always rewritten (rename has no
                // includeDeclaration knob).
                std::string newName = findStringField(msg, "newName");
                if (newName.empty()) {
                    sendResponse(id, "null");
                } else {
                    sendResponse(id, buildRenameResult(idx, uri, line1, col1,
                                                       newName));
                }
            }
        } else if (method == "shutdown") {
            shutdownRequested = true;
            sendResponse(id, "null");
        } else if (method == "exit") {
            return shutdownRequested ? 0 : 1;
        } else {
            // Unknown method. If it's a request, respond with a method-
            // not-found error so the client doesn't hang.
            if (id >= 0) {
                std::ostringstream ss;
                ss << "{\"jsonrpc\":\"2.0\",\"id\":" << id
                   << ",\"error\":{\"code\":-32601,\"message\":\""
                   << "method not found: " << jsonEscape(method) << "\"}}";
                sendMessage(ss.str());
            }
        }
    }
    return 0;
}
