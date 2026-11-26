#include "editor/indent.h"
#include "editor/buffer.h"
#include "editor/utf8.h"
#include <cctype>
#include <cstring>

namespace oh {

IndentMode IndentSettings::parse_mode(const std::string& s) {
    if (s == "auto") return IndentMode::Auto;
    if (s == "virtual") return IndentMode::Virtual;
    return IndentMode::Off;
}

const char* IndentSettings::mode_name(IndentMode m) {
    switch (m) {
        case IndentMode::Auto: return "auto";
        case IndentMode::Virtual: return "virtual";
        default: return "off";
    }
}

std::string indent_string(int cols) {
    if (cols <= 0) return {};
    size_t n = static_cast<size_t>(cols);
    if (!IndentSettings::use_tabs) return std::string(n, ' ');
    return std::string(n / utf8::TAB_WIDTH, '\t') + std::string(n % utf8::TAB_WIDTH, ' ');
}

BraceKind brace_kind_from_tree(const char* parent, const char* grandparent) {
    auto is = [](const char* a, const char* b) { return a && std::strcmp(a, b) == 0; };
    if (is(parent, "compound_statement"))  // C/C++ blocks, function and lambda bodies
        return is(grandparent, "switch_statement") ? BraceKind::Switch : BraceKind::Code;
    if (is(parent, "field_declaration_list"))  // C++ class body; Rust struct definition
        return BraceKind::Class;
    if (is(parent, "declaration_list"))  // C++ namespace / extern "C"; Rust impl, trait, mod
        return is(grandparent, "namespace_definition") || is(grandparent, "linkage_specification")
                   ? BraceKind::Namespace : BraceKind::Code;
    if (is(parent, "enumerator_list") || is(parent, "initializer_list") || is(parent, "match_block") ||
        is(parent, "field_initializer_list") || is(parent, "enum_variant_list") || is(parent, "use_list"))
        return BraceKind::List;
    if (is(parent, "block")) return BraceKind::Code;  // Rust
    return BraceKind::Unknown;  // errors, macros: the lexical rules decide
}

size_t leading_ws(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    return i;
}

size_t leading_ws_cols(const std::string& line) { return utf8::display_col(line, leading_ws(line)); }

namespace {

// Leading keyword of a statement; decides what a following `{` opens when there is no tree.
enum Kw : uint8_t { KW_NONE, KW_MOD, KW_OTHER, KW_SWITCH, KW_ENUM, KW_CLASS, KW_NAMESPACE, KW_MATCH, KW_ATTR };

bool ident_byte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

Kw classify_word(const std::string& w) {
    static const char* mods[] = {"typedef", "template", "static", "inline", "constexpr", "const", "export",
                                 "pub", "unsafe", "async", "virtual", "friend", "volatile", "explicit"};
    if (w == "switch") return KW_SWITCH;
    if (w == "enum") return KW_ENUM;
    if (w == "class" || w == "struct" || w == "union") return KW_CLASS;
    if (w == "namespace" || w == "extern") return KW_NAMESPACE;
    if (w == "match") return KW_MATCH;
    for (const char* m : mods)
        if (w == m) return KW_MOD;
    return KW_OTHER;
}

// Display width of t[from, to) when t[from] sits at display column `start`.
int width_between(const std::string& t, size_t from, size_t to, int start) {
    size_t d = static_cast<size_t>(start);
    for (size_t i = from; i < to && i < t.size(); i = utf8::next_boundary(t, i)) d += utf8::cell_width(t[i], d);
    return static_cast<int>(d) - start;
}

std::string word_at(const std::string& t, size_t i) {
    size_t k = i;
    while (k < t.size() && ident_byte(static_cast<unsigned char>(t[k]))) k++;
    return t.substr(i, k - i);
}

} // namespace

void IndentEngine::set_language(const std::string& lang_id) {
    if (lang_id == lang_id_ && !checkpoints_.empty()) return;
    lang_id_ = lang_id;
    const std::string& l = lang_id;
    if (l == "c" || l == "glsl") lang_ = Lang::C;
    else if (l == "cpp") lang_ = Lang::Cpp;
    else if (l == "rust") lang_ = Lang::Rust;
    else if (l == "javascript" || l == "typescript" || l == "javascriptreact" || l == "typescriptreact" ||
             l == "go" || l == "java" || l == "json" || l == "zig" || l == "csharp" || l == "kotlin" ||
             l == "swift" || l == "dart" || l == "scala" || l == "php")
        lang_ = Lang::Generic;
    else if (l == "css")
        lang_ = Lang::GenericNoSlashComment;
    else
        lang_ = Lang::None;  // Python, YAML, Makefiles, Markdown, plain text...: indentation is content
    reset();
}

void IndentEngine::reset() {
    checkpoints_.assign(1, State{});
    indents_.clear();
    valid_ = 0;
    tail_ = State{};
}

void IndentEngine::invalidate_from(size_t line) {
    if (checkpoints_.empty()) { reset(); return; }
    if (line >= valid_) return;
    size_t k = line / CHECKPOINT;
    tail_ = checkpoints_[k];
    checkpoints_.resize(k + 1);
    valid_ = k * CHECKPOINT;
}

int IndentEngine::indent(const TextBuffer& buf, size_t line) {
    if (generation_ != IndentSettings::generation || checkpoints_.empty()) {
        generation_ = IndentSettings::generation;
        reset();
    }
    if (lang_ == Lang::None) return KEEP;
    constexpr size_t MAX_BYTES = 32u << 20;  // huge files: leave them alone
    size_t lines = buf.line_count();
    if (line >= lines || buf.length() > MAX_BYTES) return KEEP;
    if (valid_ > lines) invalidate_all();
    if (indents_.size() < lines) indents_.resize(lines, KEEP);
    while (valid_ <= line) {
        if (valid_ % CHECKPOINT == 0 && valid_ / CHECKPOINT == checkpoints_.size()) checkpoints_.push_back(tail_);
        indents_[valid_] = process_line(buf, valid_, tail_);
        valid_++;
    }
    return indents_[line];
}

// Computes the indent of one line from the state at its start, then advances the state
// over the line. The file's leading whitespace is never used for structure.
int IndentEngine::process_line(const TextBuffer& buf, size_t line, State& s) {
    scanned_++;
    const std::string t = buf.line_text(line);
    const size_t n = t.size();
    const bool c_like = lang_ == Lang::C || lang_ == Lang::Cpp;
    const bool rust = lang_ == Lang::Rust;
    const bool generic = lang_ == Lang::Generic || lang_ == Lang::GenericNoSlashComment;
    const int W = std::max(1, IndentSettings::width);
    const size_t i = leading_ws(t);
    const bool blank = i == n;
    auto at_brace_level = [&] { return s.stack.empty() || s.stack.back().open == '{'; };

    // ---- 1. This line's indentation -------------------------------------------------
    int I;
    bool pp_line = false;
    if (s.mode == State::String || s.mode == State::RawString) {
        I = KEEP;  // re-indenting would change the string's contents
    } else if (s.mode == State::BlockComment) {
        I = (!blank && t[i] == '*') ? s.comment_col + 1 : KEEP;  // " * text" aligns under "/*"
    } else if (c_like && s.pp_cont) {
        I = W;  // continuation of a multi-line #define
        pp_line = true;
    } else if (c_like && !blank && t[i] == '#') {
        I = 0;
        pp_line = true;
    } else {
        Frame* top = s.stack.empty() ? nullptr : &s.stack.back();
        bool closer = !blank && (t[i] == '}' || t[i] == ')' || t[i] == ']');
        if (top && top->open != '{') {
            I = closer ? top->base : top->inner;  // wrapped argument list
        } else {
            int base = top ? top->inner : 0;
            std::string w = blank ? std::string() : word_at(t, i);
            auto next_after_word = [&] {
                size_t k = leading_ws(t.substr(i + w.size())) + i + w.size();
                return k < n ? t[k] : '\0';
            };
            if (closer) {
                I = top ? top->base : 0;
            } else if (c_like && top && top->kind == BraceKind::Switch && (w == "case" || w == "default")) {
                I = top->inner - W;
            } else if (c_like && top && top->kind == BraceKind::Class &&
                       (w == "public" || w == "private" || w == "protected") &&
                       (next_after_word() == ':' || ident_byte(static_cast<unsigned char>(next_after_word())))) {
                I = top->base;  // access specifiers sit at the class's level
            } else if (rust && w == "where") {
                I = base;
            } else if (!generic && s.s_open && (blank || t[i] != '{')) {
                I = s.s_cont;  // continued statement
            } else {
                I = base;
            }
        }
    }
    const int depth = static_cast<int>(s.stack.size());
    const bool dot = !blank && t[i] == '.' && !(i + 1 < n && (t[i + 1] == '.' || std::isdigit(static_cast<unsigned char>(t[i + 1]))));
    const bool lead_closer = !blank && (t[i] == '}' || t[i] == ')' || t[i] == ']');
    if (I != KEEP && !pp_line && dot && !generic && s.prev_depth == depth && s.last_tok &&
        !std::strchr(",;{(}", s.last_tok))
        I = (s.dot_active && s.dot_depth == depth) ? s.dot_indent : s.prev_I + W;
    // Columns for alignment inside this line: as displayed (computed indent, or the
    // file's own when the line is kept).
    const int I_eff = I == KEEP ? static_cast<int>(leading_ws_cols(t)) : I;

    // ---- 2. Lex the line, updating the nest stack and statement state ----------------
    bool line_has_code = false, attr_line = false;
    const bool s_open_before = s.s_open;
    // A braceless control header on a continuation line (`if (a)` under `for (...)`)
    // nests: braces and continuations after it hang from this line.
    bool ctrl_line = false;
    if (c_like && I != KEEP && !pp_line && !blank && s.s_open && at_brace_level() && t[i] != '{') {
        std::string w = word_at(t, i);
        if (w == "if" || w == "for" || w == "while" || w == "else" || w == "do" || w == "switch") {
            ctrl_line = true;
            s.s_indent = I;
        }
    }
    std::string first_word;
    char last_char = 0;

    auto code_token = [&]() {
        line_has_code = true;
        if (!generic && at_brace_level() && !s.s_open) {
            s.s_indent = I_eff;
            s.s_cont = I_eff + W;
            s.s_kw = KW_NONE;
            s.s_where = false;
            s.s_open = true;
        }
    };
    // Skips a quoted literal starting after the opening quote at j; returns the index
    // after it. An unterminated literal enters String mode when it may span lines.
    auto scan_quoted = [&](size_t j, char q, bool multiline) {
        while (j < n) {
            if (t[j] == '\\') { j += 2; continue; }
            if (t[j] == q) return j + 1;
            j++;
        }
        if (multiline) {
            s.mode = State::String;
            s.quote = q;
        }
        return n;
    };
    auto scan_raw = [&](size_t j) {  // j: first byte of the raw string's body
        size_t end = t.find(s.raw_end, j);
        if (end != std::string::npos) return end + s.raw_end.size();
        s.mode = State::RawString;
        return n;
    };

    size_t j = s.mode == State::Code ? i : 0;
    while (j < n) {
        char c = t[j];
        if (s.mode == State::BlockComment) {
            if (c == '*' && j + 1 < n && t[j + 1] == '/') {
                if (--s.comment_depth <= 0) s.mode = State::Code;
                j += 2;
            } else if (rust && c == '/' && j + 1 < n && t[j + 1] == '*') {
                s.comment_depth++;
                j += 2;
            } else {
                j++;
            }
            continue;
        }
        if (s.mode == State::String) {
            if (c == '\\') { j += 2; continue; }
            if (c == s.quote) s.mode = State::Code;
            j++;
            continue;
        }
        if (s.mode == State::RawString) {
            if (t.compare(j, s.raw_end.size(), s.raw_end) == 0) {
                j += s.raw_end.size();
                s.mode = State::Code;
            } else {
                j++;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { j++; continue; }
        if (c == '/' && j + 1 < n && t[j + 1] == '/' && lang_ != Lang::GenericNoSlashComment) break;
        if (c == '/' && j + 1 < n && t[j + 1] == '*') {
            s.mode = State::BlockComment;
            s.comment_depth = 1;
            s.comment_col = I_eff + width_between(t, i, j, I_eff);
            j += 2;
            continue;
        }
        if (pp_line) {  // only literals and comments matter on preprocessor lines
            if (c == '"' || c == '\'') j = scan_quoted(j + 1, c, false);
            else j++;
            continue;
        }
        if (c == '"') {
            code_token();
            j = scan_quoted(j + 1, '"', true);  // C strings continue only after '\' (see line end)
            s.last_tok = 'w';
            last_char = '"';
            continue;
        }
        if (c == '\'' || (c == '`' && generic)) {
            code_token();
            if (rust && c == '\'') {
                size_t k = j + 1 < n ? utf8::next_boundary(t, j + 1) : n;
                if (j + 1 < n && t[j + 1] == '\\') j = scan_quoted(j + 1, '\'', false);
                else if (k < n && t[k] == '\'') j = k + 1;  // 'x'
                else j++;                                   // lifetime: the name follows
            } else {
                j = scan_quoted(j + 1, c, generic && c == '`');
            }
            s.last_tok = 'w';
            last_char = c;
            continue;
        }
        if (ident_byte(static_cast<unsigned char>(c))) {
            size_t k = j;
            bool number = std::isdigit(static_cast<unsigned char>(c));
            while (k < n && (ident_byte(static_cast<unsigned char>(t[k])) ||
                             (number && c_like && t[k] == '\'' && k + 1 < n && std::isalnum(static_cast<unsigned char>(t[k + 1])))))
                k++;
            std::string w = t.substr(j, k - j);
            code_token();
            if (first_word.empty()) first_word = w;
            // Raw strings: R"delim( ... )delim" (C++), r#" ... "# (Rust).
            if (c_like && k < n && t[k] == '"' && (w == "R" || w == "u8R" || w == "LR" || w == "uR" || w == "UR")) {
                size_t paren = t.find('(', k + 1);
                std::string delim = paren == std::string::npos ? "" : t.substr(k + 1, paren - k - 1);
                s.raw_end = ")" + delim + "\"";
                j = scan_raw(paren == std::string::npos ? n : paren + 1);
                s.last_tok = 'w';
                last_char = '"';
                continue;
            }
            if (rust && k < n && (w == "r" || w == "br" || w == "cr") && (t[k] == '#' || t[k] == '"')) {
                size_t h = k;
                while (h < n && t[h] == '#') h++;
                if (h < n && t[h] == '"') {
                    s.raw_end = "\"" + std::string(h - k, '#');
                    j = scan_raw(h + 1);
                    s.last_tok = 'w';
                    last_char = '"';
                    continue;
                }
            }
            if (at_brace_level() && (s.s_kw == KW_NONE || s.s_kw == KW_MOD)) {
                Kw kw = classify_word(w);
                if (kw == KW_MATCH && !rust) kw = KW_OTHER;
                s.s_kw = kw;
            }
            if (rust && w == "where" && at_brace_level()) s.s_where = true;
            s.last_tok = w == "return" ? 'r' : 'w';
            last_char = t[k - 1];
            j = k;
            continue;
        }
        switch (c) {
            case '{': {
                code_token();
                BraceKind kind = BraceKind::Unknown;
                if (classify_ && (c_like || rust)) kind = classify_(buf.line_start(line) + j);
                if (kind == BraceKind::Unknown) {
                    if (c_like && std::strchr("=,([{r", s.last_tok) && s.last_tok) kind = BraceKind::List;
                    else if (c_like && s.s_kw == KW_SWITCH) kind = BraceKind::Switch;
                    else if (c_like && s.s_kw == KW_ENUM) kind = BraceKind::List;
                    else if (c_like && s.s_kw == KW_CLASS) kind = BraceKind::Class;
                    else if (lang_ == Lang::Cpp && s.s_kw == KW_NAMESPACE) kind = BraceKind::Namespace;
                    else if (rust && s.s_kw == KW_MATCH) kind = BraceKind::List;
                    else kind = BraceKind::Code;
                }
                int base = (!at_brace_level() || generic) ? I_eff : s.s_indent;
                int inner = base + W;
                if (kind == BraceKind::Namespace && !IndentSettings::indent_namespaces) inner = base;
                if (kind == BraceKind::Switch && IndentSettings::indent_case_labels) inner = base + 2 * W;
                if (kind == BraceKind::List && c_like) {  // {a, b,\n c}: align like an argument list
                    size_t k = j + 1;
                    while (k < n && (t[k] == ' ' || t[k] == '\t')) k++;
                    bool comment = k + 1 < n && t[k] == '/' && (t[k + 1] == '/' || t[k + 1] == '*');
                    if (k < n && !comment && t[k] != '}') inner = I_eff + width_between(t, i, k, I_eff);
                }
                s.stack.push_back({'{', kind, base, inner, s.s_indent, s.s_cont, s.s_open, s.s_where, s.s_kw});
                s.s_open = false;
                s.s_where = false;
                s.s_kw = KW_NONE;
                s.last_tok = '{';
                last_char = c;
                j++;
                continue;
            }
            case '(':
            case '[': {
                code_token();
                int inner = I_eff + W;
                if (c_like) {  // C/C++: align to the first argument when it follows the bracket
                    size_t k = j + 1;
                    while (k < n && (t[k] == ' ' || t[k] == '\t')) k++;
                    bool comment = k + 1 < n && t[k] == '/' && (t[k + 1] == '/' || t[k + 1] == '*');
                    if (k < n && !comment) inner = I_eff + width_between(t, i, k, I_eff);
                }
                s.stack.push_back({c, BraceKind::Unknown, I_eff, inner, 0, 0, false, false, 0});
                s.last_tok = '(';
                last_char = c;
                j++;
                continue;
            }
            case '}': {
                line_has_code = true;
                while (!s.stack.empty()) {
                    Frame f = s.stack.back();
                    s.stack.pop_back();
                    if (f.open == '{') {
                        s.s_indent = f.s_indent;
                        s.s_cont = f.s_cont;
                        s.s_open = f.s_open;
                        s.s_where = f.s_where;
                        s.s_kw = f.s_kw;
                        break;
                    }
                }
                s.last_tok = '}';
                last_char = c;
                j++;
                continue;
            }
            case ')':
            case ']': {
                line_has_code = true;
                char want = c == ')' ? '(' : '[';
                while (!s.stack.empty() && s.stack.back().open != '{') {
                    char o = s.stack.back().open;
                    s.stack.pop_back();
                    if (o == want) break;
                }
                s.last_tok = ')';
                last_char = c;
                j++;
                continue;
            }
            case ';':
                code_token();
                if (at_brace_level()) s.s_open = false;
                s.last_tok = ';';
                break;
            case ',':
                code_token();
                s.last_tok = ',';
                break;
            case ':':
                code_token();
                if (j + 1 < n && t[j + 1] == ':') {
                    s.last_tok = 'o';
                    j++;
                } else {
                    s.last_tok = ':';
                }
                break;
            case '=':
                code_token();
                if (j + 1 < n && (t[j + 1] == '=' || t[j + 1] == '>')) {
                    s.last_tok = 'o';
                    j++;
                } else {
                    s.last_tok = '=';
                }
                break;
            case '#': {
                bool fresh = at_brace_level() && !s.s_open;
                code_token();
                // Rust attribute #[...] / #![...], possibly over several lines: not a statement.
                if (rust && fresh && j + 1 < n && (t[j + 1] == '[' || (t[j + 1] == '!' && j + 2 < n && t[j + 2] == '[')))
                    s.s_kw = KW_ATTR;
                s.last_tok = 'o';
                break;
            }
            default:
                code_token();
                if (std::strchr("+-*/%&|^", c) && j + 1 < n && t[j + 1] == '=') {
                    s.last_tok = '=';  // compound assignment
                    j++;
                } else if (std::strchr("<>!", c) && j + 1 < n && t[j + 1] == '=') {
                    s.last_tok = 'o';
                    j++;
                } else {
                    s.last_tok = 'o';
                }
                break;
        }
        last_char = t[j];
        j++;
    }

    // ---- 3. Line end: does the statement continue on the next line? ------------------
    if (line_has_code && I != KEEP && !pp_line) {
        if (dot) {
            s.dot_active = true;
            s.dot_indent = I;
            s.dot_depth = depth;
        } else if (!lead_closer && depth == s.dot_depth) {
            s.dot_active = false;
        }
        s.prev_I = I;
        s.prev_depth = depth - (lead_closer ? 1 : 0);
    }
    if (s.mode == State::String && c_like && !(n && t[n - 1] == '\\')) s.mode = State::Code;
    if (c_like) s.pp_cont = pp_line && n && t[n - 1] == '\\';
    if (!pp_line && line_has_code && !generic && at_brace_level()) {
        if (c_like && t.compare(i, 2, "[[") == 0 && n >= 2 && t.compare(n - 2, 2, "]]") == 0) attr_line = true;
        if (attr_line) {
            s.s_open = s_open_before;  // attributes don't start a statement
        } else {
            switch (s.last_tok) {
                case ';': case '}': case ':': case '{':
                    s.s_open = false;
                    break;
                case ',':
                    if (rust) s.s_open = s.s_where;
                    else s.s_open = !(!s.stack.empty() && s.stack.back().kind == BraceKind::List);
                    break;
                default:
                    s.s_open = true;
                    break;
            }
            if (c_like && first_word == "template" && last_char == '>') s.s_open = false;
            if (ctrl_line && s.s_open) s.s_cont = I + W;
            if (rust && s.s_kw == KW_ATTR && last_char == ']') s.s_open = false;
        }
    }
    return I;
}

} // namespace oh
