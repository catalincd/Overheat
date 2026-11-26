#pragma once
// 4coder-style structural indentation.
//
// The engine computes each line's indentation (in display columns) from the code
// structure instead of from the whitespace that is in the file: a lexical pass keeps a
// nest stack of braces / parentheses / brackets, tracks unfinished statements
// (continuation lines), switch `case` labels, access specifiers, preprocessor lines,
// wrapped argument lists and closing brackets. When a syntax tree is available it
// classifies what each `{` opens (function body, class body, namespace, switch, list).
// The file's own leading whitespace never influences the result, which is what makes
// "virtual whitespace" possible: the editor can draw every line at its computed indent.
//
// Results are cached per line with state checkpoints every few dozen lines, so after an
// edit only the lines from the edit onwards (and only as far as someone asks) are redone.
#include "types.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oh {

class TextBuffer;

enum class IndentMode : uint8_t {
    Off,      // indentation is typed (today's behaviour + copy-indent Enter)
    Auto,     // every edit re-indents the edited lines in the buffer
    Virtual,  // leading whitespace in the file is ignored; lines are drawn at the computed indent
};

// Global indentation settings (settings.toml: indent_mode, tab_width, insert_spaces,
// indent_write_on_save, indent_namespaces, indent_case_labels). Changing them bumps
// `generation` so every engine drops its cache.
struct IndentSettings {
    static inline IndentMode mode = IndentMode::Off;
    static inline int width = 4;                 // columns per level (tab_width)
    static inline bool use_tabs = false;         // !insert_spaces
    static inline bool write_on_save = false;    // virtual mode: write computed indent on save
    static inline bool indent_namespaces = false;  // C++: indent namespace / extern "C" bodies
    static inline bool indent_case_labels = true;  // C/C++: `case` one level inside `switch`
    static inline uint64_t generation = 1;
    static void changed() { generation++; }
    static IndentMode parse_mode(const std::string& s);
    static const char* mode_name(IndentMode m);
};

// What a `{` opens; decides how its body is indented.
enum class BraceKind : uint8_t { Unknown, Code, Class, Namespace, Switch, List };

class IndentEngine {
public:
    static constexpr int KEEP = -1;  // leave the line's indentation as it is

    // Called with the byte offset of a `{`; may return Unknown (lexical fallback).
    using BraceClassifier = std::function<BraceKind(size_t byte)>;

    void set_language(const std::string& lang_id);
    const std::string& language() const { return lang_id_; }
    // False for languages where indentation is meaningful (Python, YAML...) or unknown.
    bool supported() const { return lang_ != Lang::None; }
    void set_classifier(BraceClassifier c) { classify_ = std::move(c); }
    bool has_classifier() const { return static_cast<bool>(classify_); }

    // Lines >= `line` must be recomputed (their text, or text above them, changed).
    void invalidate_from(size_t line);
    void invalidate_all() { invalidate_from(0); }

    // Computed indentation of `line` in display columns, or KEEP (inside a multi-line
    // string or a free-form block comment line, or an unsupported language).
    int indent(const TextBuffer& buf, size_t line);

    // Lines scanned since construction (tests check the cache is incremental).
    size_t lines_scanned() const { return scanned_; }

    enum class Lang : uint8_t { None, C, Cpp, Rust, Generic, GenericNoSlashComment };

    struct Frame {
        char open;        // '{' '(' '['
        BraceKind kind;
        int base;         // indent of a line that starts with the closer
        int inner;        // indent of lines inside
        // Statement state of the enclosing brace level, restored at the closer.
        int s_indent, s_cont;
        bool s_open, s_where;
        uint8_t s_kw;
    };
    struct State {
        std::vector<Frame> stack;
        enum Mode : uint8_t { Code, BlockComment, String, RawString } mode = Code;
        int comment_depth = 0;   // Rust block comments nest
        int comment_col = 0;     // display column of the `/*`
        char quote = 0;          // String mode: the closing quote
        std::string raw_end;     // RawString mode: the terminator (`)delim"` or `"##`)
        bool pp_cont = false;    // the previous line is a preprocessor line ending in '\'
        // The statement in progress at the innermost brace level.
        int s_indent = 0;        // indent of its first line (or of its latest braceless
                                 // control header: `if (a)` / `for (...)` on a continuation line)
        int s_cont = 0;          // indent of its continuation lines
        bool s_open = false;     // it continues on the next line
        bool s_where = false;    // Rust: inside a `where` clause (',' does not end it)
        uint8_t s_kw = 0;        // its leading keyword (see indent.cpp)
        char last_tok = 0;       // class of the last code token
        // Method chains: a line starting with `.` hangs one level under the chain's root
        // line; later links of the same chain align with the first one.
        int prev_I = 0, prev_depth = -1;  // indent / nest depth of the previous code line
        int dot_indent = 0, dot_depth = -1;
        bool dot_active = false;
    };

private:
    std::string lang_id_;
    Lang lang_ = Lang::None;
    BraceClassifier classify_;
    uint64_t generation_ = 0;

    static constexpr size_t CHECKPOINT = 64;
    std::vector<State> checkpoints_;  // [k] = state at the start of line k*CHECKPOINT
    std::vector<int> indents_;        // valid for lines < valid_
    size_t valid_ = 0;
    State tail_;                      // state at the start of line valid_
    size_t scanned_ = 0;

    void reset();
    int process_line(const TextBuffer& buf, size_t line, State& s);
};

// What a `{` opens, from tree-sitter node types (its parent and grandparent).
BraceKind brace_kind_from_tree(const char* parent, const char* grandparent);

// Whitespace for `cols` columns with the current settings (tabs + spaces, or spaces).
std::string indent_string(int cols);
// Bytes of leading spaces/tabs.
size_t leading_ws(const std::string& line);
// Display width of the leading whitespace.
size_t leading_ws_cols(const std::string& line);

} // namespace oh
