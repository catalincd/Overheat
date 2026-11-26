#include "ui/hover_popup.h"
#include "editor/buffer.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace oh {

namespace {

constexpr size_t MAX_COLS = 84;   // content width cap, in cells
constexpr float MAX_ROWS = 18.0f; // height cap, in lines
constexpr float PAD = 10.0f;

size_t width_of(const std::string& s) { return utf8::display_col(s, s.size()); }

Color token_color(int t, const Theme& th) {
    switch (static_cast<TokenType>(t)) {
        case TokenType::Keyword: return th.keyword;
        case TokenType::String: return th.string_color;
        case TokenType::Number: return th.number;
        case TokenType::Comment: return th.comment;
        case TokenType::Function: return th.function_name;
        case TokenType::Type: return th.type_color;
        case TokenType::Operator: return th.operator_color;
        case TokenType::Preprocessor: return th.preprocessor;
        case TokenType::Punctuation: return th.punctuation;
        case TokenType::Property: return th.property;
        case TokenType::KeywordControl: return th.keyword_control;
        case TokenType::Constant: return th.constant;
        case TokenType::BuiltinType: return th.type_builtin;
        case TokenType::Variable: return th.variable;
        default: return th.fg;
    }
}

Color style_color(md::Style s, bool heading, const Theme& th) {
    if (heading) return th.function_name;
    switch (s) {
        case md::Style::Bold: return th.keyword;      // no bold face: bold is a colour
        case md::Style::Italic: return th.type_color; // likewise italic
        case md::Style::Link: return th.accent;
        default: return th.fg;
    }
}

// Code-block info string -> highlighter language id ("" = none).
std::string grammar_for(std::string lang, const std::string& fallback) {
    for (auto& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lang.empty()) lang = fallback;
    if (lang == "c++" || lang == "cxx" || lang == "cc" || lang == "hpp" || lang == "objective-cpp") lang = "cpp";
    if (lang == "rs") lang = "rust";
    if (lang == "h" || lang == "objective-c") lang = "c";
    return SyntaxHighlighter::has_grammar(lang) ? lang : std::string();
}

} // namespace

void HoverPopup::show(const std::string& markdown, Rect anchor, const std::string& default_lang) {
    markdown_ = markdown;
    blocks_ = md::parse(markdown);
    lang_ = default_lang;
    anchor_ = anchor;
    scroll_ = 0;
    layout_cols_ = 0;
    visible_ = !blocks_.empty();
    // Highlight each code block once, as a little document of its own.
    code_tokens_.clear();
    for (auto& b : blocks_) {
        code_tokens_.emplace_back();
        if (b.kind != md::Block::Code) continue;
        std::string lang = grammar_for(b.lang, default_lang);
        auto& per_line = code_tokens_.back();
        for (auto& l : b.lines) per_line.emplace_back(l.size(), -1);
        if (lang.empty()) continue;
        std::string text;
        for (size_t i = 0; i < b.lines.size(); i++) text += b.lines[i] + (i + 1 < b.lines.size() ? "\n" : "");
        TextBuffer buf;
        buf.load_string(text);
        SyntaxHighlighter hl;
        hl.set_language(lang);
        hl.update(buf);
        for (size_t i = 0; i < b.lines.size() && i < buf.line_count(); i++)
            for (auto& tok : hl.tokens_for_line(buf, buf.line_start(i), b.lines[i].size()))
                for (size_t j = tok.start; j < tok.start + tok.length && j < per_line[i].size(); j++)
                    per_line[i][j] = static_cast<int>(tok.type);
    }
}

bool HoverPopup::keeps_open(float x, float y) const {
    if (!visible_) return false;
    auto grow = [](Rect r, float m) { return Rect{r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m}; };
    if (grow(rect_, 4.0f).contains(x, y) || grow(anchor_, 2.0f).contains(x, y)) return true;
    // The band between the word and the popup.
    float top = std::min(anchor_.y, rect_.y), bottom = std::max(anchor_.y + anchor_.h, rect_.y + rect_.h);
    float left = std::max(rect_.x, anchor_.x - 8.0f), right = std::min(rect_.x + rect_.w, anchor_.x + anchor_.w + 8.0f);
    return x >= left && x <= right && y >= top && y <= bottom;
}

bool HoverPopup::scroll(float lines) {
    if (!visible_ || max_scroll_ <= 0) return false;
    scroll_ = std::clamp(scroll_ - lines, 0.0f, max_scroll_);
    return true;
}

void HoverPopup::layout(size_t cols) {
    layout_cols_ = cols;
    lines_.clear();
    auto new_line = [&](Line::Kind k = Line::Text) {
        lines_.push_back({});
        lines_.back().kind = k;
        return &lines_.back();
    };
    auto add_run = [](Line* l, const std::string& t, md::Style st, int token, size_t col) {
        if (!l->runs.empty()) {
            Run& last = l->runs.back();
            if (last.style == st && last.token == token &&
                static_cast<size_t>(last.x) + width_of(last.text) == col) {
                last.text += t;
                return;
            }
        }
        l->runs.push_back({t, st, token, static_cast<float>(col)});
    };
    for (size_t bi = 0; bi < blocks_.size(); bi++) {
        const md::Block& b = blocks_[bi];
        bool list_cont = b.kind == md::Block::ListItem && bi > 0 && blocks_[bi - 1].kind == md::Block::ListItem;
        if (!lines_.empty() && !list_cont) new_line(Line::Gap);
        if (b.kind == md::Block::Rule) {
            new_line(Line::Rule);
            continue;
        }
        if (b.kind == md::Block::Code) {
            const auto& toks = code_tokens_[bi];
            size_t width = cols > 2 ? cols - 2 : 1;
            for (size_t li = 0; li < b.lines.size(); li++) {
                const std::string& text = b.lines[li];
                Line* l = new_line(Line::Code);
                size_t col = 0;
                for (size_t i = 0; i < text.size(); i = utf8::next_boundary(text, i)) {
                    size_t w = utf8::cell_width(text[i], col);
                    if (col + w > width && col > 0) {  // hard-wrap long code lines
                        l = new_line(Line::Code);
                        col = 0;
                    }
                    size_t n = utf8::next_boundary(text, i) - i;
                    std::string ch = text[i] == '\t' ? std::string(w, ' ') : text.substr(i, n);
                    add_run(l, ch, md::Style::Normal, toks[li][i], col + 1);
                    col += w;
                }
            }
            if (b.lines.empty()) new_line(Line::Code);
            continue;
        }
        size_t indent = 0, cont_indent = 0;
        Line* l = new_line();
        l->heading = b.kind == md::Block::Heading;
        if (b.kind == md::Block::ListItem) {
            indent = static_cast<size_t>(b.level) * 2;
            add_run(l, b.marker, md::Style::Normal, -1, indent);
            cont_indent = indent + width_of(b.marker) + 1;
        }
        size_t col = cont_indent;
        size_t width = std::max<size_t>(cols, cont_indent + 8);
        for (const md::Span& sp : b.spans) {
            if (sp.text == "\n") {
                l = new_line();
                l->heading = b.kind == md::Block::Heading;
                col = cont_indent;
                continue;
            }
            size_t i = 0;
            while (i < sp.text.size()) {
                bool space = sp.text[i] == ' ';
                size_t j = i;
                while (j < sp.text.size() && (sp.text[j] == ' ') == space) j++;
                std::string piece = space ? std::string(" ") : sp.text.substr(i, j - i);
                i = j;
                if (space) {
                    if (col > cont_indent && col + 1 <= width) {
                        add_run(l, piece, sp.style, -1, col);
                        col++;
                    }
                    continue;
                }
                size_t w = width_of(piece);
                if (col + w > width && col > cont_indent) {
                    l = new_line();
                    l->heading = b.kind == md::Block::Heading;
                    col = cont_indent;
                }
                // Longer than a whole line: break it by characters.
                while (col + w > width && w > 0) {
                    size_t fit = utf8::byte_col_at(piece, width - col);
                    if (fit == 0) fit = utf8::next_boundary(piece, 0);
                    add_run(l, piece.substr(0, fit), sp.style, -1, col);
                    piece = piece.substr(fit);
                    w = width_of(piece);
                    l = new_line();
                    l->heading = b.kind == md::Block::Heading;
                    col = cont_indent;
                }
                if (!piece.empty()) {
                    add_run(l, piece, sp.style, -1, col);
                    col += w;
                }
            }
        }
    }
    // Trailing spaces would widen the box for nothing.
    for (auto& line : lines_)
        while (!line.runs.empty() && line.runs.back().text == " ") line.runs.pop_back();
}

void HoverPopup::render(RenderContext& ctx, Rect bounds) {
    if (!visible_) return;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    size_t cols = static_cast<size_t>(std::max(16.0f, std::min(static_cast<float>(MAX_COLS), (bounds.w - 40.0f - 2 * PAD) / cw)));
    if (cols != layout_cols_) layout(cols);

    auto line_h = [&](const Line& l) { return l.kind == Line::Gap ? lh * 0.5f : l.kind == Line::Rule ? lh * 0.6f : lh; };
    size_t used = 8;
    float content_h = 0;
    for (auto& l : lines_) {
        content_h += line_h(l);
        if (!l.runs.empty())
            used = std::max(used, static_cast<size_t>(l.runs.back().x) + width_of(l.runs.back().text) + (l.kind == Line::Code ? 1 : 0));
    }
    float w = std::min(static_cast<float>(used), static_cast<float>(cols)) * cw + 2 * PAD;
    float max_h = std::min(MAX_ROWS * lh, bounds.h * 0.6f);
    float h = std::min(content_h, max_h) + 2 * PAD;
    max_scroll_ = std::max(0.0f, (content_h - max_h) / lh);
    scroll_ = std::min(scroll_, max_scroll_);

    // Above the word when there is room (VS Code), else below.
    float x = std::clamp(anchor_.x, bounds.x + 4.0f, std::max(bounds.x + 4.0f, bounds.x + bounds.w - w - 4.0f));
    float y = anchor_.y - h - 4.0f;
    if (y < bounds.y + 4.0f) y = anchor_.y + anchor_.h + 4.0f;
    y = std::min(y, bounds.y + bounds.h - h - 4.0f);
    rect_ = {std::floor(x), std::floor(y), std::ceil(w), std::ceil(h)};

    Color sh = th.shadow;
    sh.a *= 0.45f;
    ctx.r->add_rect({rect_.x - 2, rect_.y + 2, rect_.w + 4, rect_.h + 4}, sh, 8.0f);
    ctx.r->add_rect(rect_, th.surface, 6.0f, 1.0f, th.border);

    Rect clip{rect_.x + 1, rect_.y + PAD * 0.5f, rect_.w - 2, rect_.h - PAD};
    ctx.r->push_clip(clip);
    float cx = rect_.x + PAD;
    float cy = rect_.y + PAD - scroll_ * lh;
    Color code_bg = th.surface_hover;
    for (size_t i = 0; i < lines_.size(); i++) {
        const Line& l = lines_[i];
        float hgt = line_h(l);
        if (cy + hgt < clip.y || cy > clip.y + clip.h) {
            cy += hgt;
            continue;
        }
        if (l.kind == Line::Code) {
            bool first = i == 0 || lines_[i - 1].kind != Line::Code;
            bool last = i + 1 == lines_.size() || lines_[i + 1].kind != Line::Code;
            ctx.r->add_rect({cx - 4.0f, cy - (first ? 2.0f : 0.0f), rect_.w - 2 * PAD + 8.0f, hgt + (first ? 2.0f : 0.0f) + (last ? 2.0f : 0.0f)},
                            code_bg, first || last ? 4.0f : 0.0f);
        } else if (l.kind == Line::Rule) {
            ctx.r->add_rect({rect_.x + 1.0f, std::floor(cy + hgt * 0.5f), rect_.w - 2.0f, 1.0f}, th.border);
        }
        float base = cy + ctx.font->ascender();
        for (const Run& r : l.runs) {
            float rx = cx + r.x * cw;
            Color c = r.token >= 0 ? token_color(r.token, th) : l.kind == Line::Code ? th.fg : style_color(r.style, l.heading, th);
            if (r.style == md::Style::Code && l.kind != Line::Code) {
                ctx.r->add_rect({rx - 2.0f, cy + 1.0f, width_of(r.text) * cw + 4.0f, lh - 2.0f}, code_bg, 3.0f);
                c = th.string_color;
            }
            if (r.style == md::Style::Link)
                ctx.r->add_rect({rx, cy + lh - 2.0f, width_of(r.text) * cw, 1.0f}, c);
            ctx.r->add_text(rx, base, r.text, c, *ctx.font);
        }
        cy += hgt;
    }
    ctx.r->pop_clip();
    if (max_scroll_ > 0) {  // scrollbar
        float track = rect_.h - 8.0f;
        float th_h = std::max(16.0f, track * max_h / content_h);
        float ty = rect_.y + 4.0f + (track - th_h) * (scroll_ / max_scroll_);
        ctx.r->add_rect({rect_.x + rect_.w - 6.0f, ty, 3.0f, th_h}, th.scrollbar, 1.5f);
    }
}

} // namespace oh
