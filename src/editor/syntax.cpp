#include "editor/syntax.h"
#include "editor/buffer.h"
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>
#include <algorithm>
#include <cstring>

namespace oh {

SyntaxHighlighter::SyntaxHighlighter() {
    parser_ = ts_parser_new();
    build_type_map();
}

SyntaxHighlighter::~SyntaxHighlighter() {
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
                      "#include", "#define"}) {
        m[kw] = TokenType::Keyword;
    }
    // Override the preprocessor directives back
    m["#include"] = TokenType::Preprocessor;
    m["#define"] = TokenType::Preprocessor;
}

void SyntaxHighlighter::set_language(const std::string& lang_id) {
    lang_id_ = lang_id;
    if (tree_) {
        ts_tree_delete(tree_);
        tree_ = nullptr;
    }

    if (lang_id == "c" || lang_id == "cpp" || lang_id == "h") {
        // Use C grammar for C/C++ and headers
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
    if (ext == ".h") return "c";
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
    if (!parser_) return;

    // Check if a language is set
    if (!ts_parser_language(parser_)) return;

    std::string src = buffer.text();

    TSTree* old_tree = tree_;
    tree_ = ts_parser_parse_string(parser_, old_tree, src.c_str(),
                                   static_cast<uint32_t>(src.size()));
    if (old_tree) ts_tree_delete(old_tree);
}

void SyntaxHighlighter::update_incremental(const TextBuffer& buffer,
                                           size_t start_byte,
                                           size_t old_end,
                                           size_t new_end) {
    if (!parser_ || !tree_) {
        update(buffer);
        return;
    }

    // Build the edit description
    Position start_pos = buffer.position_of(start_byte);
    Position old_end_pos = buffer.position_of(std::min(old_end, buffer.length()));
    Position new_end_pos = buffer.position_of(std::min(new_end, buffer.length()));

    TSInputEdit edit;
    edit.start_byte = static_cast<uint32_t>(start_byte);
    edit.old_end_byte = static_cast<uint32_t>(old_end);
    edit.new_end_byte = static_cast<uint32_t>(new_end);
    edit.start_point = {static_cast<uint32_t>(start_pos.line),
                        static_cast<uint32_t>(start_pos.col)};
    edit.old_end_point = {static_cast<uint32_t>(old_end_pos.line),
                          static_cast<uint32_t>(old_end_pos.col)};
    edit.new_end_point = {static_cast<uint32_t>(new_end_pos.line),
                          static_cast<uint32_t>(new_end_pos.col)};

    ts_tree_edit(tree_, &edit);

    std::string src = buffer.text();
    TSTree* new_tree = ts_parser_parse_string(parser_, tree_, src.c_str(),
                                              static_cast<uint32_t>(src.size()));
    ts_tree_delete(tree_);
    tree_ = new_tree;
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

std::vector<Token> SyntaxHighlighter::tokens_for_line(size_t line,
                                                       size_t line_start_byte,
                                                       size_t line_length) const {
    std::vector<Token> tokens;
    if (!tree_ || line_length == 0) return tokens;

    TSNode root = ts_tree_root_node(tree_);

    // Create a cursor to walk descendants that intersect this line
    TSTreeCursor cursor = ts_tree_cursor_new(root);

    uint32_t line_start = static_cast<uint32_t>(line_start_byte);
    uint32_t line_end = static_cast<uint32_t>(line_start_byte + line_length);

    // We'll collect leaf tokens
    struct LeafToken {
        uint32_t start;
        uint32_t end;
        TokenType type;
    };
    std::vector<LeafToken> leaves;

    // Walk the tree using a stack-based approach for better coverage
    std::vector<TSNode> stack;
    stack.push_back(root);

    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();

        uint32_t node_start = ts_node_start_byte(node);
        uint32_t node_end = ts_node_end_byte(node);

        // Skip nodes entirely outside our line
        if (node_end <= line_start || node_start >= line_end) continue;

        uint32_t child_count = ts_node_child_count(node);
        if (child_count == 0) {
            // Leaf node — classify it
            TokenType tt = classify_node(node);
            uint32_t s = std::max(node_start, line_start);
            uint32_t e = std::min(node_end, line_end);
            if (s < e) {
                leaves.push_back({s, e, tt});
            }
        } else {
            // Push children in reverse order so we process left-to-right
            for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
                stack.push_back(ts_node_child(node, static_cast<uint32_t>(i)));
            }
        }
    }

    ts_tree_cursor_delete(&cursor);

    // Sort leaves by start byte
    std::sort(leaves.begin(), leaves.end(),
              [](const LeafToken& a, const LeafToken& b) { return a.start < b.start; });

    // Convert to Token structs relative to line start
    for (auto& lf : leaves) {
        Token t;
        t.start = lf.start - line_start;
        t.length = lf.end - lf.start;
        t.type = lf.type;
        tokens.push_back(t);
    }

    return tokens;
}

} // namespace oh
