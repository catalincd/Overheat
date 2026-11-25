#include "editor/syntax.h"
#include "editor/buffer.h"
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>

extern "C" const TSLanguage* tree_sitter_cpp();
#include <algorithm>
#include <cstring>

namespace oh {

// Feeds tree-sitter straight from the piece table, one piece at a time.
static TSInput buffer_input(const TextBuffer& buffer) {
    TSInput input{};
    input.payload = const_cast<TextBuffer*>(&buffer);
    input.read = [](void* payload, uint32_t byte, TSPoint, uint32_t* bytes_read) -> const char* {
        std::string_view chunk = static_cast<const TextBuffer*>(payload)->chunk_at(byte);
        *bytes_read = static_cast<uint32_t>(chunk.size());
        return chunk.data();
    };
    input.encoding = TSInputEncodingUTF8;
    return input;
}

SyntaxHighlighter::SyntaxHighlighter() {
    parser_ = ts_parser_new();
    build_type_map();
}

SyntaxHighlighter::~SyntaxHighlighter() {
    discard_pending();
    if (tree_) ts_tree_delete(tree_);
    if (parser_) ts_parser_delete(parser_);
}

void SyntaxHighlighter::build_type_map() {
    auto& m = type_map_.map;
    // Strings
    m["string_literal"] = TokenType::String;
    m["string_content"] = TokenType::String;
    m["string"] = TokenType::String;
    m["char_literal"] = TokenType::String;
    m["character"] = TokenType::String;
    m["system_lib_string"] = TokenType::String;
    m["escape_sequence"] = TokenType::String;

    // Numbers
    m["number_literal"] = TokenType::Number;
    m["integer_literal"] = TokenType::Number;
    m["float_literal"] = TokenType::Number;
    m["integer"] = TokenType::Number;
    m["float"] = TokenType::Number;

    // Comments
    m["comment"] = TokenType::Comment;
    m["line_comment"] = TokenType::Comment;
    m["block_comment"] = TokenType::Comment;

    // Types
    m["primitive_type"] = TokenType::Type;
    m["type_identifier"] = TokenType::Type;
    m["sized_type_specifier"] = TokenType::Type;
    m["namespace_identifier"] = TokenType::Type;
    m["auto"] = TokenType::Type;
    m["raw_string_literal"] = TokenType::String;
    m["raw_string_content"] = TokenType::String;
    m["\""] = TokenType::String;
    m["'"] = TokenType::String;
    m["::"] = TokenType::Punctuation;

    // Preprocessor
    m["preproc_include"] = TokenType::Preprocessor;
    m["preproc_def"] = TokenType::Preprocessor;
    m["preproc_ifdef"] = TokenType::Preprocessor;
    m["preproc_directive"] = TokenType::Preprocessor;
    m["#include"] = TokenType::Preprocessor;
    m["#define"] = TokenType::Preprocessor;
    m["#ifdef"] = TokenType::Preprocessor;
    m["#ifndef"] = TokenType::Preprocessor;
    m["#endif"] = TokenType::Preprocessor;
    m["#if"] = TokenType::Preprocessor;
    m["#else"] = TokenType::Preprocessor;
    m["#pragma"] = TokenType::Preprocessor;
    m["preproc_arg"] = TokenType::Normal;

    // Operators / punctuation
    m["("] = TokenType::Punctuation;
    m[")"] = TokenType::Punctuation;
    m["{"] = TokenType::Punctuation;
    m["}"] = TokenType::Punctuation;
    m["["] = TokenType::Punctuation;
    m["]"] = TokenType::Punctuation;
    m[";"] = TokenType::Punctuation;
    m[","] = TokenType::Punctuation;
    m["."] = TokenType::Operator;
    m["->"] = TokenType::Operator;
    m["+"] = TokenType::Operator;
    m["-"] = TokenType::Operator;
    m["*"] = TokenType::Operator;
    m["/"] = TokenType::Operator;
    m["%"] = TokenType::Operator;
    m["="] = TokenType::Operator;
    m["=="] = TokenType::Operator;
    m["!="] = TokenType::Operator;
    m["<"] = TokenType::Operator;
    m[">"] = TokenType::Operator;
    m["<="] = TokenType::Operator;
    m[">="] = TokenType::Operator;
    m["&&"] = TokenType::Operator;
    m["||"] = TokenType::Operator;
    m["!"] = TokenType::Operator;
    m["&"] = TokenType::Operator;
    m["|"] = TokenType::Operator;
    m["^"] = TokenType::Operator;
    m["~"] = TokenType::Operator;
    m["<<"] = TokenType::Operator;
    m[">>"] = TokenType::Operator;
    m["++"] = TokenType::Operator;
    m["--"] = TokenType::Operator;
    m["+="] = TokenType::Operator;
    m["-="] = TokenType::Operator;
    m["*="] = TokenType::Operator;
    m["/="] = TokenType::Operator;

    // Keywords
    for (auto& kw : {"if", "else", "for", "while", "do", "switch", "case",
                      "default", "break", "continue", "return", "goto",
                      "struct", "union", "enum", "typedef", "sizeof",
                      "static", "extern", "const", "volatile", "inline",
                      "register", "auto", "void", "signed", "unsigned",
                      "long", "short", "true", "false", "null", "NULL",
                      "#include", "#define",
                      "namespace", "class", "template", "typename", "public", "private",
                      "protected", "virtual", "override", "final", "new", "delete", "using",
                      "constexpr", "consteval", "constinit", "noexcept", "operator", "this",
                      "nullptr", "try", "catch", "throw", "friend", "explicit", "mutable",
                      "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
                      "co_await", "co_return", "co_yield", "concept", "requires", "decltype",
                      "static_assert", "alignas", "alignof", "thread_local"}) {
        m[kw] = TokenType::Keyword;
    }
    // Override the preprocessor directives back
    m["#include"] = TokenType::Preprocessor;
    m["#define"] = TokenType::Preprocessor;
}

void SyntaxHighlighter::discard_pending() {
    if (!pending_.valid()) return;
    if (TSTree* t = pending_.get()) ts_tree_delete(t);
}

void SyntaxHighlighter::parse_async(std::string text) {
    discard_pending();
    const TSLanguage* lang = parser_ ? ts_parser_language(parser_) : nullptr;
    if (!lang) return;
    pending_ = std::async(std::launch::async, [lang, text = std::move(text)]() -> TSTree* {
        TSParser* p = ts_parser_new();
        ts_parser_set_language(p, lang);
        TSTree* t = ts_parser_parse_string(p, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
        ts_parser_delete(p);
        request_redraw();
        return t;
    });
}

bool SyntaxHighlighter::poll() {
    if (!pending_.valid()) return true;
    if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    if (tree_) ts_tree_delete(tree_);
    tree_ = pending_.get();
    return true;
}

void SyntaxHighlighter::set_language(const std::string& lang_id) {
    discard_pending();
    lang_id_ = lang_id;
    if (tree_) {
        ts_tree_delete(tree_);
        tree_ = nullptr;
    }

    if (lang_id == "cpp") {
        ts_parser_set_language(parser_, tree_sitter_cpp());
    } else if (lang_id == "c") {
        ts_parser_set_language(parser_, tree_sitter_c());
    } else {
        // No grammar available — disable parsing
        ts_parser_set_language(parser_, nullptr);
    }
}

std::string SyntaxHighlighter::detect_language(const fs::path& path) {
    auto ext = path.extension().string();
    if (ext.empty()) return "";

    // Normalize to lowercase
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".c") return "c";
    if (ext == ".h") return "cpp";  // C++ grammar also handles C headers
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c++") return "cpp";
    if (ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h++") return "cpp";
    if (ext == ".py" || ext == ".pyw") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return "javascript";
    if (ext == ".ts") return "typescript";
    if (ext == ".tsx") return "typescriptreact";
    if (ext == ".jsx") return "javascriptreact";
    if (ext == ".go") return "go";
    if (ext == ".java") return "java";
    if (ext == ".rb") return "ruby";
    if (ext == ".lua") return "lua";
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh") return "bash";
    if (ext == ".json") return "json";
    if (ext == ".xml") return "xml";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".css") return "css";
    if (ext == ".yaml" || ext == ".yml") return "yaml";
    if (ext == ".toml") return "toml";
    if (ext == ".md" || ext == ".markdown") return "markdown";
    if (ext == ".zig") return "zig";
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag") return "glsl";
    if (ext == ".cmake") return "cmake";
    if (ext == ".sql") return "sql";

    // Check filename
    auto fname = path.filename().string();
    if (fname == "CMakeLists.txt") return "cmake";
    if (fname == "Makefile" || fname == "makefile") return "make";
    if (fname == "Dockerfile") return "dockerfile";

    return "";
}

void SyntaxHighlighter::update(const TextBuffer& buffer) {
    if (!parser_ || !ts_parser_language(parser_)) return;
    if (tree_) ts_tree_delete(tree_);
    tree_ = ts_parser_parse(parser_, nullptr, buffer_input(buffer));
}

void SyntaxHighlighter::apply_changes(const TextBuffer& buffer,
                                      const std::vector<TextBuffer::Change>& changes) {
    if (changes.empty() || !parser_ || !ts_parser_language(parser_)) return;
    if (!tree_) { update(buffer); return; }
    auto pt = [](Position p) { return TSPoint{static_cast<uint32_t>(p.line), static_cast<uint32_t>(p.col)}; };
    for (auto& c : changes) {
        TSInputEdit e;
        e.start_byte    = static_cast<uint32_t>(c.start);
        e.old_end_byte  = static_cast<uint32_t>(c.start + c.old_len);
        e.new_end_byte  = static_cast<uint32_t>(c.start + c.new_len);
        e.start_point   = pt(c.start_pt);
        e.old_end_point = pt(c.old_end_pt);
        e.new_end_point = pt(c.new_end_pt);
        ts_tree_edit(tree_, &e);
    }
    TSTree* next = ts_parser_parse(parser_, tree_, buffer_input(buffer));
    ts_tree_delete(tree_);
    tree_ = next;
}

static std::string node_text(const TextBuffer& buf, TSNode n) {
    std::string out;
    size_t a = ts_node_start_byte(n), b = std::min<size_t>(ts_node_end_byte(n), a + 200);
    while (a < b) {
        std::string_view chunk = buf.chunk_at(a);
        if (chunk.empty()) break;
        size_t take = std::min(chunk.size(), b - a);
        out.append(chunk.data(), take);
        a += take;
    }
    return out;
}

std::vector<SyntaxHighlighter::Scope> SyntaxHighlighter::scope_at(const TextBuffer& buf, size_t offset) const {
    std::vector<Scope> out;
    if (!tree_) return out;
    uint32_t off = static_cast<uint32_t>(offset);
    TSNode n = ts_node_descendant_for_byte_range(ts_tree_root_node(tree_), off, off);
    for (; !ts_node_is_null(n); n = ts_node_parent(n)) {
        const char* type = ts_node_type(n);
        if (std::strcmp(type, "function_definition") == 0) {
            // Unwrap pointer/reference declarators down to the function's name.
            TSNode d = ts_node_child_by_field_name(n, "declarator", 10);
            while (!ts_node_is_null(d) && std::strcmp(ts_node_type(d), "function_declarator") != 0) {
                TSNode inner = ts_node_child_by_field_name(d, "declarator", 10);
                if (ts_node_is_null(inner)) break;
                d = inner;
            }
            if (!ts_node_is_null(d) && std::strcmp(ts_node_type(d), "function_declarator") == 0) {
                TSNode name = ts_node_child_by_field_name(d, "declarator", 10);
                if (!ts_node_is_null(name)) out.push_back({Scope::Function, node_text(buf, name)});
            }
        } else if (std::strcmp(type, "class_specifier") == 0 || std::strcmp(type, "struct_specifier") == 0 ||
                   std::strcmp(type, "union_specifier") == 0 || std::strcmp(type, "enum_specifier") == 0) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            if (!ts_node_is_null(name)) out.push_back({Scope::Class, node_text(buf, name)});
        } else if (std::strcmp(type, "namespace_definition") == 0) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            out.push_back({Scope::Namespace, ts_node_is_null(name) ? "(anonymous)" : node_text(buf, name)});
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

TokenType SyntaxHighlighter::classify_node(TSNode node) const {
    const char* type = ts_node_type(node);

    // Check the type map first
    auto it = type_map_.map.find(type);
    if (it != type_map_.map.end()) {
        return it->second;
    }

    // Special case: identifiers may be function names
    if (std::strcmp(type, "identifier") == 0) {
        TSNode parent = ts_node_parent(node);
        // `A::b(` / `std::max(`: judge the name by what the qualified identifier is part of.
        while (!ts_node_is_null(parent) && std::strcmp(ts_node_type(parent), "qualified_identifier") == 0)
            parent = ts_node_parent(parent);
        if (!ts_node_is_null(parent)) {
            const char* parent_type = ts_node_type(parent);
            if (std::strcmp(parent_type, "function_declarator") == 0 ||
                std::strcmp(parent_type, "call_expression") == 0) {
                return TokenType::Function;
            }
            if (std::strcmp(parent_type, "field_expression") == 0) {
                // Check if this identifier is the field (second child)
                uint32_t idx = 0;
                for (uint32_t i = 0; i < ts_node_child_count(parent); i++) {
                    if (ts_node_start_byte(ts_node_child(parent, i)) == ts_node_start_byte(node)) {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 2) return TokenType::Property;
            }
        }
        return TokenType::Variable;
    }

    if (std::strcmp(type, "field_identifier") == 0) {
        return TokenType::Property;
    }

    // Named node that wasn't in our map — just Normal
    return TokenType::Normal;
}

std::vector<Token> SyntaxHighlighter::tokens_for_line(size_t /*line*/,
                                                       size_t line_start_byte,
                                                       size_t line_length) const {
    std::vector<Token> tokens;
    if (!tree_ || line_length == 0) return tokens;

    const uint32_t lo = static_cast<uint32_t>(line_start_byte);
    const uint32_t hi = static_cast<uint32_t>(line_start_byte + line_length);

    // Seek straight to the children overlapping [lo, hi) at every level, so the
    // cost is O(depth + tokens on the line), not O(nodes in the file).
    TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree_));
    auto visit = [&](auto& self) -> void {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_child_count(node) == 0) {
            uint32_t s = std::max(ts_node_start_byte(node), lo);
            uint32_t e = std::min(ts_node_end_byte(node), hi);
            if (s < e) tokens.push_back({s - lo, e - s, classify_node(node)});
            return;
        }
        if (ts_tree_cursor_goto_first_child_for_byte(&cursor, lo) < 0) return;
        do {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_start_byte(child) >= hi) break;
            if (ts_node_end_byte(child) > lo) self(self);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
        ts_tree_cursor_goto_parent(&cursor);
    };
    visit(visit);
    ts_tree_cursor_delete(&cursor);
    return tokens;  // already in document order
}

} // namespace oh
