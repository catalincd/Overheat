#include "ui/scope_highlight.h"
#include "editor/editor.h"
#include "ui/panel.h"
#include "renderer/renderer2d.h"
#include "editor/utf8.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace oh {

namespace {

// How a language spells comments and strings, as far as bracket matching cares.
struct Profile {
    const char* line_comment = nullptr;   // e.g. "//"
    bool hash_comment = false;            // '#' at a word start
    bool c_block = false;                 // /* */
    bool nested_block = false;            // Rust: /* /* */ */
    bool dq = false;                      // "..."
    bool dq_multiline = false;            // Rust strings may span lines
    char sq = 0;                          // 'c' char literal, 's' string, 'r' Rust char or lifetime
    bool backtick = false;                // `...` (JS templates, Go raw strings), multi-line
    bool triple = false;                  // Python """...""" / '''...'''
    bool cpp_raw = false;                 // R"delim(...)delim"
    bool rust_raw = false;                // r#"..."#
    bool lua_long = false;                // [[...]] and --[[...]]
};

Profile profile_for(const std::string& lang) {
    Profile p;
    auto is = [&](std::initializer_list<const char*> ids) {
        for (const char* id : ids) if (lang == id) return true;
        return false;
    };
    if (is({"c", "cpp", "glsl", "java", "go", "zig", "javascript", "javascriptreact", "typescript",
            "typescriptreact", "rust"})) {
        p.line_comment = "//";
        p.c_block = lang != "zig";
        p.dq = true;
        p.sq = 'c';
    }
    if (lang == "cpp") p.cpp_raw = true;
    if (lang == "go") p.backtick = true;
    if (is({"javascript", "javascriptreact", "typescript", "typescriptreact"})) { p.sq = 's'; p.backtick = true; }
    if (lang == "rust") { p.nested_block = true; p.dq_multiline = true; p.sq = 'r'; p.rust_raw = true; }
    if (is({"python", "bash", "toml", "yaml", "ruby"})) { p.hash_comment = true; p.dq = true; p.sq = 's'; }
    if (lang == "python") p.triple = true;
    if (is({"cmake", "make", "makefile", "dockerfile"})) { p.hash_comment = true; p.dq = true; }
    if (lang == "css") { p.c_block = true; p.dq = true; p.sq = 's'; }
    if (lang == "json") p.dq = true;
    if (lang == "sql") { p.line_comment = "--"; p.c_block = true; p.dq = true; p.sq = 's'; }
    if (lang == "lua") { p.line_comment = "--"; p.dq = true; p.sq = 's'; p.lua_long = true; }
    return p;
}

// Bytes that can start a comment, string or bracket in some language.
struct SpecialTable {
    bool v[256] = {};
    constexpr SpecialTable() {
        for (const char* p = "/#-\"'`()[]{}Rr"; *p; p++) v[static_cast<unsigned char>(*p)] = true;
    }
    bool operator[](unsigned char c) const { return v[c]; }
};
constexpr SpecialTable kSpecial;

enum Mode : uint8_t { Code = 0, BlockComment = 1, Until = 2 };  // Until: aux = 'e' when escapes apply

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80);
}

bool starts(std::string_view s, size_t i, const char* lit) {
    size_t n = std::strlen(lit);
    return i + n <= s.size() && s.compare(i, n, lit) == 0;
}

std::string text_range(const TextBuffer& buf, size_t a, size_t b) {
    std::string out;
    out.reserve(b - a);
    while (a < b) {
        std::string_view ch = buf.chunk_at(a);
        if (ch.empty()) break;
        size_t n = std::min(ch.size(), b - a);
        out.append(ch.data(), n);
        a += n;
    }
    return out;
}

// Lua long bracket at i ("[[", "[==[" ...): returns the closing delimiter or "".
std::string lua_long_close(std::string_view s, size_t i) {
    if (i >= s.size() || s[i] != '[') return "";
    size_t j = i + 1, eq = 0;
    while (j < s.size() && s[j] == '=') { j++; eq++; }
    if (j >= s.size() || s[j] != '[') return "";
    return "]" + std::string(eq, '=') + "]";
}

} // namespace

bool ScopeHighlight::enabled() const {
    return buf_ && buf_->length() <= MAX_BYTES;
}

// Lexes lines [from_line, to_line) starting in state `st`. Stops early (st = state
// just before that byte) when reaching absolute offset `stop_off`. Records brackets
// into `out` when given, and checkpoints when `make_checkpoints`.
void ScopeHighlight::scan(LexState& st, size_t from_line, size_t to_line, size_t stop_off,
                          std::vector<Bracket>* out, bool make_checkpoints) {
    const TextBuffer& buf = *buf_;
    Profile pf = profile_for(lang_);
    to_line = std::min(to_line, buf.line_count());
    constexpr size_t BLOCK = 4096;
    for (size_t blk = from_line; blk < to_line; blk += BLOCK) {
        size_t blk_end = std::min(to_line, blk + BLOCK);
        size_t base = buf.line_start(blk);
        size_t end_off = blk_end < buf.line_count() ? buf.line_start(blk_end) : buf.length();
        std::string text = text_range(buf, base, end_off);
        size_t pos = 0;
        for (size_t line = blk; line < blk_end; line++) {
            if (make_checkpoints && line % CHECKPOINT_LINES == 0 && line / CHECKPOINT_LINES == checkpoints_.size())
                checkpoints_.push_back(st);
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string_view s(text.data() + pos, nl - pos);
            size_t line_off = base + pos;
            pos = nl + 1;
            size_t i = 0;
            auto push = [&](char c, size_t col) {
                if (out) out->push_back({line_off + col, line, col, c, static_cast<int>(st.stack.size()), false});
                st.stack.push_back({c, line_off + col});
            };
            auto close = [&](char c, size_t col) {
                char want = c == ')' ? '(' : c == ']' ? '[' : '{';
                size_t n = st.stack.size();
                size_t k = n;
                // Recover from a missing closer: match up to three levels down.
                for (size_t d = 0; d < std::min<size_t>(n, 3); d++)
                    if (st.stack[n - 1 - d].ch == want) { k = n - 1 - d; break; }
                if (k == n) {
                    if (out) out->push_back({line_off + col, line, col, c, static_cast<int>(n), true});
                    return;
                }
                size_t open_off = st.stack[k].off;
                st.stack.resize(k);
                if (out) {
                    out->push_back({line_off + col, line, col, c, static_cast<int>(k), false});
                    pair_[open_off] = line_off + col;
                    pair_[line_off + col] = open_off;
                }
            };
            while (i < s.size()) {
                if (line_off + i >= stop_off) return;
                char c = s[i];
                if (st.mode == BlockComment) {
                    if (pf.nested_block && starts(s, i, "/*")) { st.nest++; i += 2; continue; }
                    if (starts(s, i, "*/")) {
                        i += 2;
                        if (--st.nest <= 0) { st.mode = Code; st.nest = 0; }
                        continue;
                    }
                    // Jump to the next '*' or '/' (brackets inside never matter).
                    size_t n = s.find_first_of("*/", i + 1);
                    i = n == std::string_view::npos ? s.size() : n;
                    continue;
                }
                if (st.mode == Until) {
                    if (st.aux == 'e' && c == '\\') { i += 2; continue; }
                    if (starts(s, i, st.delim.c_str())) { i += st.delim.size(); st.mode = Code; st.delim.clear(); continue; }
                    char stops[3] = {st.delim[0], st.aux == 'e' ? '\\' : st.delim[0], 0};
                    size_t n = s.find_first_of(stops, i + 1);
                    i = n == std::string_view::npos ? s.size() : n;
                    continue;
                }
                // Code. Most bytes are plain text: skip them with one table lookup.
                if (!kSpecial[static_cast<unsigned char>(c)]) { i++; continue; }
                if (pf.lua_long && starts(s, i, "--")) {
                    std::string cl = lua_long_close(s, i + 2);
                    if (!cl.empty()) { st.mode = Until; st.aux = 0; st.delim = cl; i += cl.size() + 2; continue; }
                }
                if (pf.line_comment && starts(s, i, pf.line_comment)) break;
                if (pf.hash_comment && c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])))) break;
                if (pf.c_block && starts(s, i, "/*")) { st.mode = BlockComment; st.nest = 1; i += 2; continue; }
                bool prev_ident = i > 0 && ident_char(s[i - 1]);
                if (pf.lua_long && c == '[') {
                    std::string cl = lua_long_close(s, i);
                    if (!cl.empty()) { st.mode = Until; st.aux = 0; st.delim = cl; i += cl.size(); continue; }
                }
                if (pf.cpp_raw && c == 'R' && i + 1 < s.size() && s[i + 1] == '"' &&
                    (i == 0 || !ident_char(s[i - 1]) || s[i - 1] == '8' || s[i - 1] == 'L' || s[i - 1] == 'u' || s[i - 1] == 'U')) {
                    size_t paren = s.find('(', i + 2);
                    if (paren != std::string_view::npos && paren - (i + 2) <= 16) {
                        st.mode = Until; st.aux = 0;
                        st.delim = ")" + std::string(s.substr(i + 2, paren - (i + 2))) + "\"";
                        i = paren + 1;
                        continue;
                    }
                }
                if (pf.rust_raw && c == 'r' && !prev_ident) {
                    size_t j = i + 1;
                    while (j < s.size() && s[j] == '#') j++;
                    if (j < s.size() && s[j] == '"') {
                        st.mode = Until; st.aux = 0;
                        st.delim = "\"" + std::string(j - i - 1, '#');
                        i = j + 1;
                        continue;
                    }
                }
                if (pf.triple && (c == '"' || c == '\'') && i + 2 < s.size() && s[i + 1] == c && s[i + 2] == c) {
                    st.mode = Until; st.aux = 'e'; st.delim = std::string(3, c);
                    i += 3;
                    continue;
                }
                if (pf.backtick && c == '`') { st.mode = Until; st.aux = 'e'; st.delim = "`"; i++; continue; }
                if (pf.dq && c == '"' && pf.dq_multiline) { st.mode = Until; st.aux = 'e'; st.delim = "\""; i++; continue; }
                if ((pf.dq && c == '"') || (pf.sq == 's' && c == '\'')) {  // single-line string
                    size_t j = i + 1;
                    while (j < s.size() && s[j] != c) j += s[j] == '\\' ? 2 : 1;
                    i = j + 1;
                    continue;
                }
                if (c == '\'' && (pf.sq == 'c' || pf.sq == 'r')) {
                    size_t j = i + 1;
                    bool lit = false;
                    if (pf.sq == 'c' && prev_ident) { i++; continue; }  // 1'000'000 digit separators
                    if (j < s.size() && s[j] == '\\') {
                        j += 2;
                        while (j < s.size() && j < i + 12 && s[j] != '\'') j++;
                        lit = j < s.size() && s[j] == '\'';
                    } else if (j < s.size()) {
                        size_t k = j + 1;
                        while (k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) k++;
                        lit = k < s.size() && s[k] == '\'';
                        j = k;
                    }
                    i = lit ? j + 1 : i + 1;  // otherwise a Rust lifetime / stray quote
                    continue;
                }
                if (c == '(' || c == '[' || c == '{') push(c, i);
                else if (c == ')' || c == ']' || c == '}') close(c, i);
                i++;
            }
            // Escaped newlines aside, single-line constructs end with the line.
        }
    }
}

const ScopeHighlight::LexState& ScopeHighlight::state_at_line(size_t line, size_t& from_line) {
    size_t idx = line / CHECKPOINT_LINES;
    if (checkpoints_.empty()) checkpoints_.push_back(LexState{});
    if (idx >= checkpoints_.size()) {
        size_t last = checkpoints_.size() - 1;
        LexState st = checkpoints_[last];
        scan(st, last * CHECKPOINT_LINES, idx * CHECKPOINT_LINES + 1, SIZE_MAX, nullptr, true);
        idx = std::min(idx, checkpoints_.size() - 1);
    }
    from_line = idx * CHECKPOINT_LINES;
    return checkpoints_[idx];
}

void ScopeHighlight::update(const Editor& ed, size_t first, size_t end) {
    const TextBuffer& buf = ed.buffer();
    std::string lang = const_cast<Editor&>(ed).syntax().language_id();
    if (&buf != buf_ || lang != lang_) {
        buf_ = &buf;
        lang_ = lang;
        checkpoints_.clear();
        version_ = buf.version();
        vis_version_ = cur_version_ = UINT64_MAX;
    }
    if (!enabled() || !(bracket_colors || scopes)) {
        vis_.clear();
        cur_stack_.clear();
        vis_version_ = cur_version_ = UINT64_MAX;
        return;
    }
    if (buf.version() != version_) {
        size_t off = buf.earliest_change_since(version_);
        if (off != SIZE_MAX) {
            size_t line = off == 0 ? 0 : buf.position_of(std::min(off, buf.length())).line;
            size_t keep = line / CHECKPOINT_LINES + 1;  // checkpoint at line k*K is valid when k*K <= line
            if (off == 0) keep = 1;
            if (checkpoints_.size() > keep) checkpoints_.resize(keep);
        }
        version_ = buf.version();
    }
    if (vis_version_ != version_ || first != vis_first_ || end != vis_end_) {
        vis_version_ = version_;
        vis_first_ = first;
        vis_end_ = end;
        vis_.clear();
        pair_.clear();
        size_t from;
        LexState st = state_at_line(first, from);
        scan(st, from, first, SIZE_MAX, nullptr, false);
        vis_start_stack_ = st.stack;
        scan(st, first, end, SIZE_MAX, &vis_, false);
    }
    size_t cur = buf.offset_of(ed.cursors().primary_pos().line, ed.cursors().primary_pos().col);
    if (cur_version_ != version_ || cur != cur_off_) {
        cur_version_ = version_;
        cur_off_ = cur;
        size_t line = ed.cursors().primary_pos().line, from;
        LexState st = state_at_line(line, from);
        scan(st, from, line + 1, cur, nullptr, false);
        cur_stack_ = std::move(st.stack);
    }
}

void ScopeHighlight::color_line(size_t line, std::vector<Color>& byte_colors, const Theme& th) const {
    if (!bracket_colors || vis_.empty() || line < vis_first_ || line >= vis_end_) return;
    auto it = std::lower_bound(vis_.begin(), vis_.end(), line, [](const Bracket& b, size_t l) { return b.line < l; });
    for (; it != vis_.end() && it->line == line; ++it)
        if (it->col < byte_colors.size()) byte_colors[it->col] = it->error ? th.error : th.bracket(static_cast<size_t>(it->depth));
}

bool ScopeHighlight::render(RenderContext& ctx, const Editor& ed, const Geometry& g) {
    const Theme& th = *ctx.theme;
    const TextBuffer& buf = ed.buffer();
    float right = g.text.x + g.text.w;
    float bottom = g.text.y + g.text.h;
    auto mark = [&](Theme::Mark style, Color c, float x0, float y, float x1) {
        if (x1 <= x0) return;
        switch (style) {
            case Theme::Mark::Underline:
                ctx.r->add_rect({x0, y + g.line_h - 2.0f, x1 - x0, 1.5f}, c);
                break;
            case Theme::Mark::Box:
                ctx.r->add_rect({x0, y, x1 - x0, g.line_h}, {0, 0, 0, 0}, 2.0f, 1.0f, c);
                break;
            case Theme::Mark::Background: {
                Color f = c;
                f.a = std::min(1.0f, f.a * (c.a > 0.3f ? 0.4f : 1.0f));
                ctx.r->add_rect({x0, y, x1 - x0, g.line_h}, f, 2.0f);
                break;
            }
        }
    };

    // Jump location (search hit, go to definition).
    if (auto jl = ed.jump_line(); jl && *jl >= vis_first_ && *jl < vis_end_ && *jl < buf.line_count()) {
        float y0 = g.line_top(*jl), y1 = g.line_top(*jl + 1);
        if (th.jump_line_style == Theme::Mark::Background) {
            ctx.r->add_rect({g.text.x, y0, g.text.w, y1 - y0}, th.jump_line);
        } else {
            std::string t = buf.line_text(*jl);
            size_t lead = t.find_first_not_of(" \t");
            if (lead != std::string::npos) {
                auto [x0, ya] = g.point(*jl, lead);
                auto [x1, yb] = g.point(*jl, t.size());
                Color c = th.jump_line;
                c.a = std::max(c.a, 0.6f);
                mark(th.jump_line_style, c, x0, ya, ya == yb ? x1 : right);
            }
        }
    }

    if (!enabled()) return false;
    bool drew_pair = false;
    auto find_vis = [&](size_t off) -> const Bracket* {
        auto it = std::lower_bound(vis_.begin(), vis_.end(), off, [](const Bracket& b, size_t o) { return b.off < o; });
        return it != vis_.end() && it->off == off ? &*it : nullptr;
    };
    auto partner = [&](size_t off) -> size_t {
        auto it = pair_.find(off);
        return it == pair_.end() ? SIZE_MAX : it->second;
    };
    size_t vis_lo = buf.line_start(vis_first_);
    if (scopes && ed.cursors().selections().size() == 1) {
        // Enclosing { } scopes: one stacked tint layer per level, from the opening line's
        // indent to the right edge, plus a bar along the innermost scope. Only the
        // innermost MAX_TINTED levels are tinted so deep code stays readable, and scopes
        // at column 0 whose body is not indented (C++ namespaces, extern "C") are skipped.
        constexpr size_t MAX_TINTED = 4;
        struct Band { float x0, y0, y1, bar_y0, bar_y1; };
        std::vector<Band> bands;
        auto indent_of = [](const std::string& t) {
            size_t n = t.find_first_not_of(" \t");
            return n == std::string::npos ? std::string::npos : utf8::display_col(t, n);
        };
        for (const Open& o : cur_stack_) {
            if (o.ch != '{') continue;
            bool open_vis = o.off >= vis_lo;
            const Bracket* ob = open_vis ? find_vis(o.off) : nullptr;
            if (open_vis && !ob) continue;  // opened below the visible lines
            if (!open_vis && std::none_of(vis_start_stack_.begin(), vis_start_stack_.end(),
                                          [&](const Open& s) { return s.off == o.off; }))
                continue;  // closed above the visible lines
            size_t open_line = ob ? ob->line : buf.position_of(o.off).line;
            size_t close_off = partner(o.off);
            const Bracket* cb = close_off != SIZE_MAX ? find_vis(close_off) : nullptr;
            if (cb && cb->line == open_line) continue;  // one-line scope: the pair box is enough
            std::string t = buf.line_text(open_line);
            size_t open_ind = indent_of(t);
            if (open_ind == std::string::npos) open_ind = 0;
            bool flat = false;  // top-level scope with an unindented body
            for (size_t l = open_line + 1; open_ind == 0 && l < std::min(buf.line_count(), open_line + 8); l++) {
                if (cb && l >= cb->line) break;
                size_t ind = indent_of(buf.line_text(l));
                if (ind == std::string::npos) continue;
                flat = ind <= open_ind;
                break;
            }
            if (flat) continue;
            size_t lead = t.find_first_not_of(" \t");
            if (lead == std::string::npos) lead = 0;
            float x0 = g.point(open_line, lead).first - g.char_w * 0.5f;
            float y0 = std::max(g.line_top(open_line), g.text.y - g.line_h);
            float y1 = cb ? g.line_top(cb->line + 1) : bottom + g.line_h;
            bands.push_back({x0, y0, y1, open_vis ? g.line_top(open_line + 1) : y0, cb ? g.line_top(cb->line) : y1});
        }
        size_t skip = bands.size() > MAX_TINTED ? bands.size() - MAX_TINTED : 0;
        for (size_t i = skip; i < bands.size(); i++)
            ctx.r->add_rect({bands[i].x0, bands[i].y0, right - bands[i].x0, bands[i].y1 - bands[i].y0}, th.scope_tint);
        if (!bands.empty() && bands.back().bar_y1 > bands.back().bar_y0) {
            const Band& in = bands.back();
            ctx.r->add_rect({in.x0, in.bar_y0, 2.0f, in.bar_y1 - in.bar_y0}, th.scope_boundary, 1.0f);
        }

        // Bracket pair: filled box at the bracket under/before the cursor, otherwise an
        // outline on the innermost enclosing pair.
        Position cp = ed.cursors().primary_pos();
        size_t cur = buf.offset_of(cp.line, cp.col);
        const Bracket* at = find_vis(cur);
        if (!at || at->error) at = cur > 0 ? find_vis(cur - 1) : nullptr;
        if (at && at->error) at = nullptr;
        auto box = [&](size_t off, bool fill, int depth_of) {
            const Bracket* b = find_vis(off);
            if (!b) return;
            auto [x, y] = g.point(b->line, b->col);
            Color f = th.bracket(static_cast<size_t>(depth_of));
            if (fill) {
                f.a = 0.24f;
                ctx.r->add_rect({x - 1.0f, y + 1.0f, g.char_w + 2.0f, g.line_h - 2.0f}, f, 3.0f, 1.0f, th.bracket_match);
            } else {
                ctx.r->add_rect({x, y, g.char_w, g.line_h}, {0, 0, 0, 0}, 2.0f, 1.0f, th.bracket_match);
            }
        };
        if (at) {
            box(at->off, true, at->depth);
            if (size_t p = partner(at->off); p != SIZE_MAX) box(p, true, at->depth);
            drew_pair = true;
        } else if (!cur_stack_.empty()) {
            const Open& o = cur_stack_.back();
            int d = static_cast<int>(cur_stack_.size() - 1);
            box(o.off, false, d);
            if (size_t p = partner(o.off); p != SIZE_MAX) box(p, false, d);
            drew_pair = true;
        }
    }

    // Identifier under the cursor and its other visible occurrences.
    if (occurrences && ed.cursors().selections().size() == 1 && ed.cursors().selections()[0].empty()) {
        Position cp = ed.cursors().primary_pos();
        std::string line = buf.line_text(cp.line);
        size_t a = std::min(cp.col, line.size()), b = a;
        while (a > 0 && ident_char(line[a - 1])) a--;
        while (b < line.size() && ident_char(line[b])) b++;
        bool word = b > a && !std::isdigit(static_cast<unsigned char>(line[a]));
        if (word) {  // skip keywords, literals and comments
            auto toks = const_cast<Editor&>(ed).syntax().tokens_for_line(buf, buf.line_start(cp.line), line.size());
            for (const Token& t : toks) {
                if (a < t.start || a >= t.start + t.length) continue;
                switch (t.type) {
                    case TokenType::Keyword: case TokenType::KeywordControl: case TokenType::Comment:
                    case TokenType::String: case TokenType::Number: case TokenType::Constant:
                    case TokenType::BuiltinType: case TokenType::Preprocessor:
                        word = false; break;
                    default: break;
                }
            }
        }
        if (word) {
            std::string w = line.substr(a, b - a);
            for (size_t l = vis_first_; l < vis_end_ && l < buf.line_count(); l++) {
                std::string t = l == cp.line ? line : buf.line_text(l);
                for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1)) {
                    if ((p > 0 && ident_char(t[p - 1])) || (p + w.size() < t.size() && ident_char(t[p + w.size()]))) continue;
                    auto [x0, y0] = g.point(l, p);
                    auto [x1, y1] = g.point(l, p + w.size());
                    if (y1 != y0) x1 = right;
                    Color c = th.occurrence;
                    if (l == cp.line && p == a) c.a = std::min(1.0f, c.a * 1.35f);
                    mark(th.occurrence_style, c, x0, y0, x1);
                }
            }
        }
    }
    return drew_pair;
}

} // namespace oh
