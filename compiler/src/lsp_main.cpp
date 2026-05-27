// kard-lsp — minimal Language Server Protocol server for kardashev.
//
// Speaks LSP over stdio. Recognises the four base notifications/requests
// needed to publish diagnostics in an editor:
//   - initialize (request)         — handshake
//   - initialized (notification)   — no-op acknowledge
//   - textDocument/didOpen
//   - textDocument/didChange       — full-sync; we re-run the pipeline
//   - textDocument/didClose
//   - shutdown (request) / exit (notification)
//
// On every open / change we feed the document through the same
// lex → parse → typecheck → borrow_check pipeline kardc uses. Each
// diagnostic in the result becomes an LSP Diagnostic.
//
// JSON is parsed by hand against the narrow shapes LSP sends. The
// parser is intentionally permissive (no schema validation) — it just
// looks for the specific fields we care about.

#include "kardashev/borrow_check.hpp"
#include "kardashev/parser.hpp"
#include "kardashev/typecheck.hpp"

#include <algorithm>
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
            // Tell the client we sync documents as full text (kind=1)
            // and we publish diagnostics. Everything else stays off.
            sendResponse(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"diagnosticProvider\":{\"interFileDependencies\":false,"
                                        "\"workspaceDiagnostics\":false}"
                "},\"serverInfo\":{\"name\":\"kard-lsp\","
                                  "\"version\":\"0.1\"}}");
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
