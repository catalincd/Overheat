#include "lsp/workspace_edit.h"
#include "editor/utf8.h"
#include <algorithm>
#include <cctype>

namespace oh {

fs::path lsp_uri_to_path(const std::string& uri) {
    if (uri.compare(0, 7, "file://") != 0) return fs::path(uri);
    std::string p = uri.substr(7);
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size() && std::isxdigit(static_cast<unsigned char>(p[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(p[i + 2]))) {
            out += static_cast<char>(std::stoi(p.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += p[i];
        }
    }
#ifdef _WIN32
    // file:///C:/x -> C:/x
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
#endif
    return fs::path(out);
}

void lsp_parse_range(const lsp_json::Value& range, Position& start, Position& end) {
    start.line = static_cast<size_t>(std::max(0, range["start"]["line"].as_int()));
    start.col = static_cast<size_t>(std::max(0, range["start"]["character"].as_int()));
    end.line = static_cast<size_t>(std::max(0, range["end"]["line"].as_int()));
    end.col = static_cast<size_t>(std::max(0, range["end"]["character"].as_int()));
}

std::vector<LSPTextEdit> parse_text_edits(const lsp_json::Value& arr) {
    std::vector<LSPTextEdit> out;
    for (auto& e : arr.arr) {
        if (!e.has("range")) continue;  // SnippetTextEdit etc. still carry range + newText
        LSPTextEdit te;
        lsp_parse_range(e["range"], te.start, te.end);
        te.new_text = e["newText"].str();
        out.push_back(std::move(te));
    }
    return out;
}

LSPWorkspaceEdit parse_workspace_edit(const lsp_json::Value& edit) {
    LSPWorkspaceEdit we;
    auto add = [&](const fs::path& p, std::vector<LSPTextEdit> edits) {
        for (auto& f : we.files)
            if (f.path == p) {
                f.edits.insert(f.edits.end(), edits.begin(), edits.end());
                return;
            }
        we.files.push_back({p, std::move(edits)});
    };
    if (edit["documentChanges"].is_array()) {
        for (auto& dc : edit["documentChanges"].arr) {
            if (dc.has("kind")) {  // create / rename / delete
                we.skipped_ops++;
                continue;
            }
            add(lsp_uri_to_path(dc["textDocument"]["uri"].str()), parse_text_edits(dc["edits"]));
        }
    } else if (edit["changes"].is_object()) {
        for (auto& [uri, edits] : edit["changes"].obj) add(lsp_uri_to_path(uri), parse_text_edits(edits));
    }
    return we;
}

bool resolve_text_edits(const std::string& text, const std::vector<LSPTextEdit>& edits, bool utf16,
                        std::vector<ByteEdit>& out, std::string* error) {
    out.clear();
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') starts.push_back(i + 1);
    auto offset_of = [&](Position p) -> size_t {
        if (p.line >= starts.size()) return text.size();
        size_t ls = starts[p.line];
        size_t le = p.line + 1 < starts.size() ? starts[p.line + 1] - 1 : text.size();
        if (le > ls && text[le - 1] == '\r') le--;  // CRLF: the \r is not part of the line
        std::string line = text.substr(ls, le - ls);
        size_t col = utf16 ? utf8::byte_col_from_utf16(line, p.col) : p.col;
        return ls + std::min(col, line.size());
    };
    for (size_t k = 0; k < edits.size(); k++) {
        size_t a = offset_of(edits[k].start), b = offset_of(edits[k].end);
        if (b < a) std::swap(a, b);
        out.push_back({a, b - a, edits[k].new_text});
    }
    // Stable: inserts at the same offset keep their server order.
    std::stable_sort(out.begin(), out.end(), [](const ByteEdit& x, const ByteEdit& y) { return x.offset < y.offset; });
    for (size_t k = 1; k < out.size(); k++) {
        if (out[k - 1].offset + out[k - 1].length > out[k].offset) {
            if (error) *error = "overlapping edits";
            out.clear();
            return false;
        }
    }
    return true;
}

bool apply_text_edits(std::string& text, const std::vector<LSPTextEdit>& edits, bool utf16, std::string* error) {
    std::vector<ByteEdit> resolved;
    if (!resolve_text_edits(text, edits, utf16, resolved, error)) return false;
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    for (auto& e : resolved) {
        out.append(text, pos, e.offset - pos);
        out += e.text;
        pos = e.offset + e.length;
    }
    out.append(text, pos, std::string::npos);
    text = std::move(out);
    return true;
}

size_t map_offset_through(size_t offset, const std::vector<ByteEdit>& sorted_edits) {
    long long delta = 0;
    for (auto& e : sorted_edits) {
        if (e.offset + e.length <= offset) {
            delta += static_cast<long long>(e.text.size()) - static_cast<long long>(e.length);
        } else if (e.offset < offset) {  // inside a replaced range: snap to its end
            return static_cast<size_t>(static_cast<long long>(e.offset) + delta) + e.text.size();
        } else {
            break;
        }
    }
    return static_cast<size_t>(static_cast<long long>(offset) + delta);
}

} // namespace oh
