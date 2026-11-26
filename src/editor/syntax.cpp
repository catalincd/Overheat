#include "editor/syntax.h"
#include "editor/buffer.h"
#include <tree_sitter/api.h>
// Grammars are vendored in third_party/ and built into ts_grammars (see GRAMMARS below).
extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();
extern "C" const TSLanguage* tree_sitter_rust();
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

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

// ---------------------------------------------------------------------------
// Per-language highlighting rules. A leaf node is coloured by its node type
// through `map`; the language's classifier refines what the type alone cannot
// tell (an identifier naming a function, a type, a constant...).
// ---------------------------------------------------------------------------

namespace {

struct Rules {
    std::unordered_map<std::string, TokenType> map;
    // Nodes coloured as one token, without descending (comments, lifetimes).
    std::unordered_map<std::string, TokenType> whole;
    // Nodes whose children are coloured as usual; the bytes no child covers
    // (hidden tokens such as a raw string's r#" delimiters) get this colour.
    std::unordered_map<std::string, TokenType> underlay;
};

using Classifier = TokenType (*)(TSNode node, const char* type, const TextBuffer& buf, const Rules& r);

bool is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
const char* type_of(TSNode n) { return ts_node_is_null(n) ? "" : ts_node_type(n); }
bool is_field(TSNode parent, const char* field, TSNode node) {
    TSNode f = ts_node_child_by_field_name(parent, field, static_cast<uint32_t>(std::strlen(field)));
    return !ts_node_is_null(f) && ts_node_eq(f, node);
}

std::string node_text(const TextBuffer& buf, TSNode n) {
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

TokenType lookup(const Rules& r, const char* type) {
    auto it = r.map.find(type);
    return it == r.map.end() ? TokenType::Normal : it->second;
}

// --- C / C++ ---------------------------------------------------------------

Rules build_c_family() {
    Rules rules;
    auto& m = rules.map;
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

    // Finer classes so themes can tell control flow, literals and built-in
    // types apart (they fall back to the keyword / type colours).
    for (auto& kw : {"if", "else", "for", "while", "do", "switch", "case", "default", "break",
                     "continue", "return", "goto", "try", "catch", "throw",
                     "co_await", "co_return", "co_yield"})
        m[kw] = TokenType::KeywordControl;
    for (auto& kw : {"true", "false", "null", "NULL", "nullptr", "this"})
        m[kw] = TokenType::Constant;
    for (auto& kw : {"primitive_type", "auto", "void", "signed", "unsigned", "long", "short"})
        m[kw] = TokenType::BuiltinType;
    return rules;
}

TokenType classify_c(TSNode node, const char* type, const TextBuffer&, const Rules& rules) {
    // Check the type map first
    auto it = rules.map.find(type);
    if (it != rules.map.end()) {
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
        // Method declarations (`bool empty() const`) and calls (`v.empty()`).
        TSNode parent = ts_node_parent(node);
        while (!ts_node_is_null(parent) && std::strcmp(ts_node_type(parent), "qualified_identifier") == 0)
            parent = ts_node_parent(parent);
        if (!ts_node_is_null(parent)) {
            const char* pt = ts_node_type(parent);
            if (std::strcmp(pt, "function_declarator") == 0) return TokenType::Function;
            if (std::strcmp(pt, "field_expression") == 0) {
                TSNode gp = ts_node_parent(parent);
                if (!ts_node_is_null(gp) && std::strcmp(ts_node_type(gp), "call_expression") == 0)
                    return TokenType::Function;
            }
        }
        return TokenType::Property;
    }

    // Named node that wasn't in our map — just Normal
    return TokenType::Normal;
}

const Rules& c_rules() {
    static const Rules rules = build_c_family();
    return rules;
}

// --- Rust ------------------------------------------------------------------
// Colours follow VS Code with rust-analyzer: macros like functions, lifetimes
// like storage modifiers, `self`/`Self` like language constants, attributes
// in the preprocessor colour, and the usual naming conventions (CamelCase is a
// type or enum variant, SCREAMING_CASE a constant).

Rules build_rust() {
    Rules rules;
    auto& m = rules.map;
    for (auto& kw : {"as", "async", "const", "crate", "default", "dyn", "enum", "extern", "fn",
                     "impl", "let", "mod", "move", "pub", "raw", "ref", "static", "struct", "super",
                     "trait", "type", "union", "unsafe", "use", "where", "in", "gen",
                     "mutable_specifier"})
        m[kw] = TokenType::Keyword;
    for (auto& kw : {"if", "else", "match", "loop", "while", "for", "break", "continue", "return",
                     "yield", "await", "try"})
        m[kw] = TokenType::KeywordControl;
    m["macro_rules!"] = TokenType::Function;
    m["true"] = m["false"] = m["self"] = TokenType::Constant;

    m["primitive_type"] = TokenType::BuiltinType;
    m["type_identifier"] = TokenType::Type;
    m["field_identifier"] = TokenType::Property;
    m["shorthand_field_identifier"] = TokenType::Property;
    m["metavariable"] = TokenType::Variable;

    m["integer_literal"] = m["float_literal"] = TokenType::Number;
    m["string_content"] = m["escape_sequence"] = m["\""] = TokenType::String;  // `"` also covers b" and c"

    for (auto& p : {"(", ")", "[", "]", "{", "}", ";", ",", ":", "::"}) m[p] = TokenType::Punctuation;
    for (auto& op : {".", "..", "..=", "...", "->", "=>", "?", "@", "+", "-", "*", "/", "%", "=", "==",
                     "!=", "<", ">", "<=", ">=", "&&", "||", "!", "&", "|", "^", "<<", ">>", "+=",
                     "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="})
        m[op] = TokenType::Operator;

    rules.whole["line_comment"] = TokenType::Comment;  // includes /// and //! doc comments
    rules.whole["block_comment"] = TokenType::Comment;
    rules.whole["shebang"] = TokenType::Comment;
    rules.whole["char_literal"] = TokenType::String;    // 'x', b'\n'
    rules.whole["lifetime"] = TokenType::Keyword;       // 'a, 'static, '_
    rules.whole["label"] = TokenType::Keyword;          // 'outer: loop / break 'outer
    rules.underlay["string_literal"] = TokenType::String;
    rules.underlay["raw_string_literal"] = TokenType::String;  // r#"..."#, br"..."
    return rules;
}

const Rules& rust_rules() {
    static const Rules rules = build_rust();
    return rules;
}

// Rust naming conventions: SCREAMING_CASE constants, CamelCase types and variants.
TokenType rust_name_class(const std::string& s) {
    bool upper = !s.empty() && std::isupper(static_cast<unsigned char>(s[0]));
    if (!upper) return TokenType::Variable;
    bool has_lower = std::any_of(s.begin(), s.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
    return !has_lower && s.size() > 1 ? TokenType::Constant : TokenType::Type;
}

TokenType classify_rust(TSNode node, const char* type, const TextBuffer& buf, const Rules& rules) {
    TSNode parent = ts_node_parent(node);
    const char* pt = type_of(parent);
    bool ident = is(type, "identifier");

    // #[derive(...)] / #![allow(...)]: the brackets and the attribute's path.
    if (is(pt, "attribute_item") || is(pt, "inner_attribute_item")) return TokenType::Preprocessor;
    if (ident && is(pt, "attribute")) return TokenType::Preprocessor;
    if (ident && is(pt, "scoped_identifier") && is(type_of(ts_node_parent(parent)), "attribute"))
        return TokenType::Preprocessor;

    // println!(...), std::println!(...), macro_rules! name
    if (is(pt, "macro_invocation") && (ident || is(type, "!"))) return TokenType::Function;
    if (ident && is(pt, "scoped_identifier") && is_field(parent, "name", node) &&
        is(type_of(ts_node_parent(parent)), "macro_invocation"))
        return TokenType::Function;
    if (ident && is(pt, "macro_definition")) return TokenType::Function;
    if (is(pt, "fragment_specifier")) return TokenType::BuiltinType;  // $e:expr

    if (is(type, "for") && is(pt, "impl_item")) return TokenType::Keyword;  // impl Trait for T
    if (is(type, "type_identifier") && ts_node_end_byte(node) - ts_node_start_byte(node) == 4 &&
        node_text(buf, node) == "Self")
        return TokenType::Constant;

    // A callee position: f(..), f::<T>(..), or the same with `path::` in front.
    auto is_callee = [&](TSNode callee) {
        TSNode call = ts_node_parent(callee);
        const char* ct = type_of(call);
        return (is(ct, "call_expression") || is(ct, "generic_function")) && is_field(call, "function", callee);
    };

    if (is(type, "field_identifier")) {
        // v.len(), s.parse::<u32>()
        if (is(pt, "field_expression") && is_field(parent, "field", node) && is_callee(parent))
            return TokenType::Function;
        return TokenType::Property;
    }
    if (!ident) return lookup(rules, type);

    if (is(pt, "function_item") || is(pt, "function_signature_item")) return TokenType::Function;
    // Module paths (std::, fmt::) and module names take the type colour, as VS Code's namespaces do.
    if ((is(pt, "scoped_identifier") || is(pt, "scoped_type_identifier") || is(pt, "scoped_use_list")) &&
        is_field(parent, "path", node))
        return TokenType::Type;
    if (is(pt, "mod_item")) return TokenType::Type;
    TokenType conv = rust_name_class(node_text(buf, node));
    // Some(x) / Ok(x) are tuple-variant constructors: they keep the type colour.
    auto call_class = [&] { return conv == TokenType::Type ? TokenType::Type : TokenType::Function; };
    if (is_callee(node)) return call_class();
    if (is(pt, "scoped_identifier") && is_field(parent, "name", node) && is_callee(parent)) return call_class();
    if (is(pt, "token_tree") && conv == TokenType::Variable) {
        // Macro arguments are bare token trees: `name(` reads as a call, `.name` as a field.
        TSNode next = ts_node_next_sibling(node);
        if (is(type_of(next), "token_tree") && ts_node_child_count(next) > 0 &&
            is(type_of(ts_node_child(next, 0)), "("))
            return TokenType::Function;
        if (is(type_of(ts_node_prev_sibling(node)), ".")) return TokenType::Property;
    }
    return conv;
}

} // namespace

struct SyntaxHighlighter::Grammar {
    const char* id;  // as returned by detect_language()
    const TSLanguage* (*language)();
    const Rules& (*rules)();
    Classifier classify;
};

// The language table. Adding a grammar: vendor it under third_party/, list it in
// TS_GRAMMARS in CMakeLists.txt, declare its tree_sitter_<name>() above, add a
// row here (and its extensions to detect_language() if missing).
static const SyntaxHighlighter::Grammar GRAMMARS[] = {
    {"c", tree_sitter_c, c_rules, classify_c},
    {"cpp", tree_sitter_cpp, c_rules, classify_c},
    {"rust", tree_sitter_rust, rust_rules, classify_rust},
};

static const SyntaxHighlighter::Grammar* find_grammar(const std::string& id) {
    for (auto& g : GRAMMARS)
        if (id == g.id) return &g;
    return nullptr;
}

bool SyntaxHighlighter::has_grammar(const std::string& lang_id) { return find_grammar(lang_id) != nullptr; }

SyntaxHighlighter::SyntaxHighlighter() {
    parser_ = ts_parser_new();
}

SyntaxHighlighter::~SyntaxHighlighter() {
    discard_pending();
    if (tree_) ts_tree_delete(tree_);
    if (parser_) ts_parser_delete(parser_);
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

    // No grammar available: a null language disables parsing.
    grammar_ = find_grammar(lang_id);
    ts_parser_set_language(parser_, grammar_ ? grammar_->language() : nullptr);
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
        // Rust
        } else if (is(type, "function_item")) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            if (!ts_node_is_null(name)) out.push_back({Scope::Function, node_text(buf, name)});
        } else if (is(type, "struct_item") || is(type, "enum_item") || is(type, "union_item") ||
                   is(type, "trait_item")) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            if (!ts_node_is_null(name)) out.push_back({Scope::Class, node_text(buf, name)});
        } else if (is(type, "impl_item")) {
            TSNode ty = ts_node_child_by_field_name(n, "type", 4);
            if (!ts_node_is_null(ty)) out.push_back({Scope::Class, node_text(buf, ty)});
        } else if (is(type, "mod_item")) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            if (!ts_node_is_null(name)) out.push_back({Scope::Namespace, node_text(buf, name)});
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<Token> SyntaxHighlighter::tokens_for_line(const TextBuffer& buffer, size_t line_start_byte,
                                                       size_t line_length) const {
    std::vector<Token> tokens;
    if (!tree_ || !grammar_ || line_length == 0) return tokens;
    const Rules& rules = grammar_->rules();

    const uint32_t lo = static_cast<uint32_t>(line_start_byte);
    const uint32_t hi = static_cast<uint32_t>(line_start_byte + line_length);
    auto emit = [&](TSNode node, TokenType type) {
        uint32_t s = std::max(ts_node_start_byte(node), lo);
        uint32_t e = std::min(ts_node_end_byte(node), hi);
        if (s < e) tokens.push_back({s - lo, e - s, type});
    };

    // Seek straight to the children overlapping [lo, hi) at every level, so the
    // cost is O(depth + tokens on the line), not O(nodes in the file).
    TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree_));
    auto visit = [&](auto& self) -> void {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const char* type = ts_node_type(node);
        if (ts_node_child_count(node) == 0) {
            emit(node, grammar_->classify(node, type, buffer, rules));
            return;
        }
        if (!rules.whole.empty()) {
            auto it = rules.whole.find(type);
            if (it != rules.whole.end()) {
                emit(node, it->second);
                return;
            }
        }
        size_t first_token = tokens.size();
        if (ts_tree_cursor_goto_first_child_for_byte(&cursor, lo) >= 0) {
            do {
                TSNode child = ts_tree_cursor_current_node(&cursor);
                if (ts_node_start_byte(child) >= hi) break;
                if (ts_node_end_byte(child) > lo) self(self);
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_goto_parent(&cursor);
        }
        if (rules.underlay.empty()) return;
        auto it = rules.underlay.find(type);
        if (it == rules.underlay.end()) return;
        // Give the bytes between the children's tokens the node's colour, in order.
        uint32_t s = std::max(ts_node_start_byte(node), lo) - lo;
        uint32_t e = std::min(ts_node_end_byte(node), hi) - lo;
        std::vector<Token> merged;
        uint32_t at = s;
        for (size_t i = first_token; i < tokens.size(); i++) {
            if (tokens[i].start > at) merged.push_back({at, tokens[i].start - at, it->second});
            merged.push_back(tokens[i]);
            at = std::max(at, tokens[i].start + tokens[i].length);
        }
        if (at < e) merged.push_back({at, e - at, it->second});
        tokens.resize(first_token);
        tokens.insert(tokens.end(), merged.begin(), merged.end());
    };
    visit(visit);
    ts_tree_cursor_delete(&cursor);
    return tokens;  // already in document order
}

} // namespace oh
