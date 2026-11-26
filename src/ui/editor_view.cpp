#include "ui/editor_view.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace oh {

EditorView::EditorView() = default;

void EditorView::set_editor(std::shared_ptr<Editor> editor) {
    editor_ = std::move(editor);
}

float EditorView::gutter_width(const FontAtlas& font) const {
    if (!editor_) return 40.0f;
    size_t line_count = editor_->buffer().line_count();
    int digits = 1;
    size_t n = line_count;
    while (n >= 10) { digits++; n /= 10; }
    // At least 3 digits wide, plus padding on both sides
    digits = std::max(digits, 3);
    return static_cast<float>(digits) * font.char_width() + 30.0f;  // + change-marker lane
}

void EditorView::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    if (!editor_) return;
    editor_->sync_syntax();
    editor_->virtual_normalize();  // virtual whitespace: no caret inside the hidden indent

    cached_line_h_ = ctx.font->line_height();
    cached_char_w_ = ctx.font->char_width();
    // Smooth scrolling: the view eases toward the editor's scroll target; switching
    // files or jumping far snaps instead of sweeping through the whole document.
    if (editor_.get() != anim_editor_ || std::abs(editor_->scroll_y() - smooth_y_.value) > bounds.h * 3.0f) {
        smooth_y_.snap(editor_->scroll_y());
        smooth_x_.snap(editor_->scroll_x());
        anim_editor_ = editor_.get();
    }
    view_y_ = smooth_y_.follow(editor_->scroll_y(), anim::NORMAL, 0.5f);
    view_x_ = smooth_x_.follow(editor_->scroll_x(), anim::NORMAL, 0.5f);
    if (shape_font_ != ctx.font || shape_font_size_ != cached_line_h_ ||
        shape_ligatures_ != ctx.font->ligatures() || ctx.font->generation() != shape_generation_) {
        shape_cache_.clear();
        shape_font_ = ctx.font;
        shape_font_size_ = cached_line_h_;
        shape_ligatures_ = ctx.font->ligatures();
        shape_generation_ = ctx.font->generation();
    }

    float gw = gutter_width(*ctx.font);
    cached_gutter_w_ = gw;
    Rect gutter_rect = {bounds.x, bounds.y, gw, bounds.h};
    float mm_w = minimap_width();
    Rect text_rect = {bounds.x + gw, bounds.y, bounds.w - gw - mm_w, bounds.h};
    minimap_rect_ = {bounds.x + bounds.w - mm_w, bounds.y, mm_w, bounds.h};
    update_wrap_map(text_rect.w);
    if (wrapping()) view_x_ = 0;

    // Editor background
    ctx.r->add_rect(bounds, ctx.theme->bg);

    // Render layers
    render_gutter(ctx, gutter_rect);
    ctx.r->push_clip(gutter_rect);
    render_git_marks(ctx, gutter_rect);
    ctx.r->pop_clip();
    ctx.r->push_clip(text_rect);
    {   // Current line underneath everything else in the text area.
        auto [first, end] = visible_lines(text_rect);
        size_t cursor_line = editor_->cursors().primary_pos().line;
        if (cursor_line >= first && cursor_line < end) {
            float rows = static_cast<float>(first_row(cursor_line + 1) - first_row(cursor_line));
            if (row_start_.empty()) rows = 1;
            ctx.r->add_rect({text_rect.x, row_y(cursor_line, text_rect), text_rect.w, cached_line_h_ * rows},
                            ctx.theme->current_line);
        }
    }
    render_indent_guides(ctx, text_rect);
    render_find_matches(ctx, text_rect);
    {   // Matching bracket outline
        Position a, b;
        if (editor_->cursors().selections().size() == 1 && editor_->find_bracket_pair(a, b)) {
            Color c = ctx.theme->bracket_match;
            auto [first, end] = visible_lines(text_rect);
            for (Position p : {a, b}) {
                if (p.line < first || p.line >= end) continue;
                std::string l = editor_->buffer().line_text(p.line);
                Pt pt = point(l, p.line, p.col, text_rect);
                ctx.r->add_rect({pt.x, pt.y, cached_char_w_, cached_line_h_}, {0, 0, 0, 0}, 2.0f, 1.0f, c);
            }
        }
    }
    render_selections(ctx, text_rect);
    render_text(ctx, text_rect);
    render_diagnostics(ctx, text_rect);
    render_blame(ctx, text_rect);
    render_cursors(ctx, text_rect);
    ctx.r->pop_clip();
    render_minimap(ctx, minimap_rect_);
    render_scrollbar(ctx, {text_rect.x, text_rect.y, text_rect.w + minimap_width(), text_rect.h});
    find_.render(ctx, bounds);
    if (completion_.visible()) {
        const Position& a = completion_.anchor();
        std::string line = editor_->buffer().line_text(a.line);
        Pt pt = point(line, a.line, a.col, text_rect);
        completion_.render(ctx, pt.x, pt.y, cached_line_h_, bounds);
    }
}

void EditorView::render_gutter(RenderContext& ctx, Rect gutter_rect) {
    auto& buf = editor_->buffer();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_y = view_y_;

    // Gutter background
    ctx.r->add_rect(gutter_rect, ctx.theme->gutter_bg);

    (void)scroll_y;
    (void)buf;
    auto [first_vis, end_vis] = visible_lines(gutter_rect);
    size_t first_line = first_vis;
    size_t cursor_line = editor_->cursors().primary_pos().line;

    for (size_t i = 0; first_line + i < end_vis; i++) {
        size_t line_num = first_line + i + 1; // 1-based
        float y = row_y(first_line + i, gutter_rect);

        if (y + line_h < gutter_rect.y || y > gutter_rect.y + gutter_rect.h)
            continue;

        // Format the line number
        char num_buf[16];
        std::snprintf(num_buf, sizeof(num_buf), "%zu", line_num);
        int len = static_cast<int>(std::strlen(num_buf));

        // Right-align, leaving room for the git change lane at the gutter edge.
        float text_x = gutter_rect.x + gutter_rect.w - 16.0f -
                        static_cast<float>(len) * char_w;

        // Highlight current line number
        Color color = ((first_line + i) == cursor_line)
                           ? ctx.theme->line_number_active
                           : ctx.theme->line_number;

        ctx.r->add_text(text_x, y + ctx.font->ascender(), num_buf, color,
                           *ctx.font);
    }
}

bool EditorView::wrapping() const {
    return wrap_ && wrap_cols_ > 0 && editor_ && editor_->buffer().length() <= (2u << 20);
}

std::vector<size_t> EditorView::segments(const std::string& text) const {
    std::vector<size_t> out{0};
    if (!wrapping()) return out;
    size_t dcol = 0, row_dcol = 0, last_space = SIZE_MAX;
    for (size_t i = 0; i < text.size(); i = utf8::next_boundary(text, i)) {
        size_t w = utf8::cell_width(text[i], dcol);
        if (dcol + w - row_dcol > wrap_cols_ && i > out.back()) {
            // Break after the last space on this row if there is one, else mid-word.
            size_t brk = (last_space != SIZE_MAX && last_space > out.back()) ? last_space : i;
            out.push_back(brk);
            row_dcol = utf8::display_col(text, brk);
            last_space = SIZE_MAX;
        }
        if (text[i] == ' ' || text[i] == '\t') last_space = utf8::next_boundary(text, i);
        dcol += w;
    }
    return out;
}

void EditorView::update_wrap_map(float text_width) {
    wrap_cols_ = wrap_ ? static_cast<size_t>(std::max(10.0f, (text_width - TEXT_PAD - RULER_W) / cached_char_w_)) : 0;
    if (!wrapping()) {
        row_start_.clear();
        return;
    }
    auto& buf = editor_->buffer();
    if (wrap_version_ == buf.version() && wrap_map_cols_ == wrap_cols_ && row_start_.size() == buf.line_count() + 1)
        return;
    wrap_version_ = buf.version();
    wrap_map_cols_ = wrap_cols_;
    row_start_.assign(buf.line_count() + 1, 0);
    size_t row = 0;
    for (size_t l = 0; l < buf.line_count(); l++) {
        row_start_[l] = row;
        row += segments(buf.line_text(l)).size();
    }
    row_start_.back() = row;
}

size_t EditorView::first_row(size_t line) const {
    if (row_start_.empty()) return line;
    return row_start_[std::min(line, row_start_.size() - 1)];
}

size_t EditorView::total_rows() const {
    return row_start_.empty() ? editor_->buffer().line_count() : row_start_.back();
}

std::pair<size_t, size_t> EditorView::line_at_row(size_t row) const {
    if (row_start_.empty()) return {std::min(row, editor_->buffer().line_count() - 1), 0};
    auto it = std::upper_bound(row_start_.begin(), row_start_.end() - 1, row);
    size_t line = static_cast<size_t>(it - row_start_.begin()) - 1;
    return {line, row - row_start_[line]};
}

EditorView::Pt EditorView::point(const std::string& text, size_t line, size_t byte, Rect text_rect) const {
    auto segs = segments(text);
    size_t seg = static_cast<size_t>(std::upper_bound(segs.begin(), segs.end(), byte) - segs.begin()) - 1;
    size_t seg_col = seg ? utf8::display_col(text, segs[seg]) : 0;
    float x = text_rect.x + TEXT_PAD - view_x_ + static_cast<float>(dcol(text, line, byte) - seg_col) * cached_char_w_;
    return {x, text_rect.y + static_cast<float>(first_row(line) + seg) * cached_line_h_ - view_y_};
}

size_t EditorView::dcol(const std::string& text, size_t line, size_t byte) const {
    return virt() ? editor_->view_col(line, text, byte) : utf8::display_col(text, byte);
}

float EditorView::row_y(size_t line, Rect text_rect) const {
    return text_rect.y + static_cast<float>(first_row(line)) * cached_line_h_ - view_y_;
}

float EditorView::col_x(const std::string& line_text, size_t byte_col, Rect text_rect) const {
    return text_rect.x + TEXT_PAD - view_x_ +
           static_cast<float>(utf8::display_col(line_text, byte_col)) * cached_char_w_;
}

std::pair<size_t, size_t> EditorView::visible_lines(Rect text_rect) const {
    size_t top = static_cast<size_t>(std::max(0.0f, view_y_) / cached_line_h_);
    size_t count = static_cast<size_t>(text_rect.h / cached_line_h_) + 2;
    size_t first = line_at_row(top).first;
    size_t end = std::min(line_at_row(top + count).first + 1, editor_->buffer().line_count());
    return {first, end};
}

void EditorView::render_git_marks(RenderContext& ctx, Rect gutter_rect) {
    if (!editor_->has_git_base()) return;
    Rect text_rect{gutter_rect.x + gutter_rect.w, gutter_rect.y, 0, gutter_rect.h};
    auto [first, end] = visible_lines(text_rect);
    float lane_x = gutter_rect.x + gutter_rect.w - 7.0f;
    for (const GutterMark& m : editor_->git_marks()) {
        if (m.kind == GutterMark::Deleted) {
            if (m.line < first || m.line > end) continue;
            float y = row_y(m.line, text_rect);
            ctx.r->add_rect({lane_x - 1.0f, y - 2.0f, 6.0f, 4.0f}, ctx.theme->git_deleted, 2.0f);
            continue;
        }
        size_t a = std::max(m.line, first), b = std::min(m.line + m.count, end);
        if (a >= b) continue;
        float y0 = row_y(a, text_rect), y1 = row_y(b, text_rect);
        Color c = m.kind == GutterMark::Added ? ctx.theme->git_added : ctx.theme->git_modified;
        ctx.r->add_rect({lane_x, y0, 3.0f, y1 - y0}, c, 1.0f);
    }
}

void EditorView::render_text(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    auto& syntax = editor_->syntax();
    float char_w = cached_char_w_;
    auto [first, end] = visible_lines(text_rect);

    const bool virt_ws = virt();
    for (size_t line_idx = first; line_idx < end; line_idx++) {
        std::string line = buf.line_text(line_idx);
        float y = row_y(line_idx, text_rect);
        auto tokens = syntax.tokens_for_line(buf, buf.line_start(line_idx), line.size());
        if (virt_ws) {  // shown as computed indent + the text after the file's own whitespace
            size_t ws = leading_ws(line), vi = editor_->view_col(line_idx, line, ws);
            if (vi != utf8::display_col(line, ws) || line.find('\t') < ws) {
                line = std::string(vi, ' ') + line.substr(ws);
                std::vector<Token> shifted;
                for (Token t : tokens) {
                    size_t a = std::max<size_t>(t.start, ws), b = t.start + t.length;
                    if (b <= a) continue;
                    shifted.push_back({static_cast<uint32_t>(a - ws + vi), static_cast<uint32_t>(b - a), t.type});
                }
                tokens = std::move(shifted);
            }
        }
        size_t line_len = line.size();

        std::vector<Color> byte_colors(line_len, ctx.theme->fg);
        for (auto& tok : tokens) {
            Color c = ctx.theme->fg;
            switch (tok.type) {
                case TokenType::Keyword:      c = ctx.theme->keyword; break;
                case TokenType::String:       c = ctx.theme->string_color; break;
                case TokenType::Number:       c = ctx.theme->number; break;
                case TokenType::Comment:      c = ctx.theme->comment; break;
                case TokenType::Function:     c = ctx.theme->function_name; break;
                case TokenType::Type:         c = ctx.theme->type_color; break;
                case TokenType::Operator:     c = ctx.theme->operator_color; break;
                case TokenType::Preprocessor: c = ctx.theme->preprocessor; break;
                case TokenType::Punctuation:  c = ctx.theme->punctuation; break;
                case TokenType::Property:     c = ctx.theme->property; break;
                case TokenType::Error:        c = ctx.theme->error; break;
                case TokenType::KeywordControl: c = ctx.theme->keyword_control; break;
                case TokenType::Constant:     c = ctx.theme->constant; break;
                case TokenType::BuiltinType:  c = ctx.theme->type_builtin; break;
                case TokenType::Variable:     c = ctx.theme->variable; break;
                default: break;
            }
            for (size_t j = tok.start; j < tok.start + tok.length && j < line_len; j++)
                byte_colors[j] = c;
        }

        float x_base = text_rect.x + TEXT_PAD - view_x_;
        float baseline = y + ctx.font->ascender();

        // Shape the line (cached by its text) so ligatures and contextual
        // alternates render; each glyph is placed at its character's cell.
        auto it = shape_cache_.find(line);
        if (it == shape_cache_.end()) {
            if (shape_cache_.size() > 8192) shape_cache_.clear();
            std::vector<FontAtlas::Shaped> run;
            ctx.font->shape(line, run);
            it = shape_cache_.emplace(line, std::move(run)).first;
        }
        // Display column for each byte offset where a character starts.
        std::vector<uint32_t> col_of(line_len + 1, 0);
        {
            size_t dcol = 0;
            for (size_t i = 0; i < line_len; i = utf8::next_boundary(line, i)) {
                col_of[i] = static_cast<uint32_t>(dcol);
                dcol += utf8::cell_width(line[i], dcol);
            }
        }
        uint32_t prev_cluster = UINT32_MAX;
        float in_cluster = 0;
        float right_edge = text_rect.x + text_rect.w;
        auto segs = segments(line);
        size_t seg = 0;
        for (const FontAtlas::Shaped& g : it->second) {
            if (g.cluster >= line_len) continue;
            char lead = line[g.cluster];
            if (lead == '\t' || lead == '\r') continue;
            if (g.cluster != prev_cluster) { prev_cluster = g.cluster; in_cluster = 0; }
            while (seg + 1 < segs.size() && g.cluster >= segs[seg + 1]) seg++;
            float seg_x = static_cast<float>(col_of[segs[seg]]) * char_w;
            float x = x_base + static_cast<float>(col_of[g.cluster]) * char_w - seg_x + in_cluster + g.x_offset;
            baseline = y + static_cast<float>(seg) * cached_line_h_ + ctx.font->ascender();
            if (x > right_edge) {
                if (segs.size() == 1) break;
                continue;
            }
            if (x + char_w * 3 < text_rect.x) continue;
            Color c = byte_colors[g.cluster];
            if (g.gid == 0) {
                // Not in the primary face: use the per-codepoint fallback chain.
                size_t end = utf8::next_boundary(line, g.cluster);
                uint32_t cp = static_cast<unsigned char>(lead);
                if (cp >= 0x80) {
                    cp &= 0x3F >> (end - g.cluster - 1);
                    for (size_t k = g.cluster + 1; k < end; k++)
                        cp = (cp << 6) | (static_cast<unsigned char>(line[k]) & 0x3F);
                }
                ctx.r->add_char(x, baseline - g.y_offset, cp, c, *ctx.font);
            } else {
                const GlyphInfo* gi = ctx.font->glyph_by_index(g.gid);
                ctx.r->add_glyph(x, baseline - g.y_offset, gi, c);
                if (gi) in_cluster += gi->advance;
            }
        }
    }
}

void EditorView::render_selections(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);

    for (auto& sel : editor_->cursors().selections()) {
        if (sel.empty()) continue;
        Position mn = sel.min(), mx = sel.max();
        for (size_t line_idx = std::max(first, mn.line); line_idx < end && line_idx <= mx.line; line_idx++) {
            std::string line = buf.line_text(line_idx);
            size_t c0 = line_idx == mn.line ? mn.col : 0;
            size_t c1 = line_idx == mx.line ? mx.col : line.size();
            auto segs = segments(line);
            for (size_t k = 0; k < segs.size(); k++) {
                size_t s0 = segs[k], s1 = k + 1 < segs.size() ? segs[k + 1] : line.size();
                size_t a = std::max(c0, s0), b = std::min(c1, s1);
                bool last = k + 1 == segs.size();
                if (a > b || (a == b && !(last && line_idx != mx.line))) continue;
                Pt p0 = point(line, line_idx, a, text_rect);
                float x1 = p0.x + static_cast<float>(dcol(line, line_idx, b) - dcol(line, line_idx, a)) * cached_char_w_;
                if (last && line_idx != mx.line) x1 += cached_char_w_;  // show the selected newline
                if (x1 > p0.x) ctx.r->add_rect({p0.x, p0.y, x1 - p0.x, cached_line_h_}, ctx.theme->selection);
            }
        }
    }
}

float EditorView::max_scroll() const {
    return std::max(0.0f, static_cast<float>(total_rows() - 1) * cached_line_h_);
}

void EditorView::render_scrollbar(RenderContext& ctx, Rect text_rect) {
    ruler_ = {text_rect.x + text_rect.w - RULER_W, text_rect.y, RULER_W, text_rect.h};
    const Theme& th = *ctx.theme;
    auto& buf = editor_->buffer();
    float lines = static_cast<float>(std::max<size_t>(1, buf.line_count()));
    auto y_of = [&](size_t line) { return ruler_.y + ruler_.h * static_cast<float>(line) / lines; };
    float min_h = 2.0f;

    if (editor_->has_git_base()) {
        for (const GutterMark& m : editor_->git_marks()) {
            Color c = m.kind == GutterMark::Added ? th.git_added
                    : m.kind == GutterMark::Modified ? th.git_modified : th.git_deleted;
            float y0 = y_of(m.line), y1 = std::max(y_of(m.line + m.count), y0 + min_h);
            ctx.r->add_rect({ruler_.x + 1.0f, y0, 3.0f, y1 - y0}, c);
        }
    }
    if (find_.is_open()) {
        Color c = th.warning;
        c.a = 0.8f;
        size_t last = SIZE_MAX;
        for (auto& m : find_.matches()) {
            size_t line = buf.position_of(m.start).line;
            if (line == last) continue;
            last = line;
            ctx.r->add_rect({ruler_.x + 5.0f, y_of(line), 4.0f, min_h}, c);
        }
    }
    if (diagnostics_) {
        for (auto& d : *diagnostics_) {
            Color c = d.severity == 1 ? th.error : d.severity == 2 ? th.warning : th.info;
            ctx.r->add_rect({ruler_.x + RULER_W - 4.0f, y_of(d.start.line), 3.0f, 3.0f}, c);
        }
    }
    Color cur = th.fg;
    cur.a = 0.6f;
    ctx.r->add_rect({ruler_.x + 1.0f, y_of(editor_->cursors().primary_pos().line), RULER_W - 2.0f, 1.0f}, cur);

    // Thumb: the visible window over the scrollable range.
    float content = max_scroll() + text_rect.h;
    if (content <= text_rect.h + 1.0f) return;
    float h = std::max(24.0f, ruler_.h * text_rect.h / content);
    float y = ruler_.y + (ruler_.h - h) * (view_y_ / std::max(1.0f, max_scroll()));
    Color tc = th.scrollbar;
    if (dragging_thumb_) tc.a = std::min(1.0f, tc.a * 2.0f);
    ctx.r->add_rect({ruler_.x + 2.0f, y, RULER_W - 4.0f, h}, tc, 3.0f);
}

bool EditorView::line_has_diagnostic(size_t line) const {
    if (!diagnostics_) return false;
    for (auto& d : *diagnostics_)
        if (d.start.line == line) return true;
    return false;
}

void EditorView::render_diagnostics(RenderContext& ctx, Rect text_rect) {
    if (!diagnostics_ || diagnostics_->empty()) return;
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    auto color_of = [&](int sev) {
        return sev == 1 ? ctx.theme->error : sev == 2 ? ctx.theme->warning : ctx.theme->info;
    };
    std::vector<const LSPDiagnostic*> worst(end - first, nullptr);  // per visible line, most severe
    for (const LSPDiagnostic& d : *diagnostics_) {
        if (d.end.line < first || d.start.line >= end) continue;
        for (size_t line = std::max(d.start.line, first); line <= d.end.line && line < end; line++) {
            std::string text = buf.line_text(line);
            size_t c0 = line == d.start.line ? std::min(d.start.col, text.size()) : 0;
            size_t c1 = line == d.end.line ? std::min(d.end.col, text.size()) : text.size();
            auto segs = segments(text);
            for (size_t k = 0; k < segs.size(); k++) {
                size_t s0 = segs[k], s1 = k + 1 < segs.size() ? segs[k + 1] : text.size();
                size_t a = std::max(c0, s0), b = std::min(c1, s1);
                if (a > b || (a == b && segs.size() > 1)) continue;
                Pt p0 = point(text, line, a, text_rect);
                float x1 = std::max(p0.x + static_cast<float>(dcol(text, line, b) - dcol(text, line, a)) *
                                               cached_char_w_, p0.x + cached_char_w_);
                ctx.r->add_squiggle({p0.x, p0.y + cached_line_h_ - 5.0f, x1 - p0.x, 5.0f}, color_of(d.severity), 1.2f, 6.0f, 1.2f);
            }
        }
        if (d.start.line >= first) {
            auto& slot = worst[d.start.line - first];
            if (!slot || d.severity < slot->severity) slot = &d;
        }
    }
    // Message after the end of the line (Error Lens style), first line of the text only.
    for (size_t i = 0; i < worst.size(); i++) {
        if (!worst[i]) continue;
        size_t line = first + i;
        std::string text = buf.line_text(line);
        std::string msg = worst[i]->message.substr(0, worst[i]->message.find('\n'));
        Color c = color_of(worst[i]->severity);
        c.a *= 0.85f;
        Pt end_pt = point(text, line, text.size(), text_rect);
        ctx.r->add_text(end_pt.x + cached_char_w_ * 4.0f, end_pt.y + ctx.font->ascender(), msg, c, *ctx.font);
    }
}

static std::string relative_time(int64_t when) {
    int64_t d = static_cast<int64_t>(std::time(nullptr)) - when;
    auto fmt = [](int64_t n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (d < 60) return "just now";
    if (d < 3600) return fmt(d / 60, "minute");
    if (d < 86400) return fmt(d / 3600, "hour");
    if (d < 86400 * 30) return fmt(d / 86400, "day");
    if (d < 86400 * 365) return fmt(d / (86400 * 30), "month");
    return fmt(d / (86400 * 365), "year");
}

void EditorView::render_blame(RenderContext& ctx, Rect text_rect) {
    if (!show_blame_ || editor_->cursors().selections().size() != 1) return;
    size_t line = editor_->cursors().primary_pos().line;
    if (line_has_diagnostic(line)) return;  // the diagnostic message owns the line end
    auto [first, end] = visible_lines(text_rect);
    if (line < first || line >= end) return;
    bool uncommitted = false;
    const GitBlame* b = editor_->blame_for_line(line, uncommitted);
    std::string text;
    if (uncommitted) text = "You \u00b7 Uncommitted changes";
    else if (b) text = b->author + ", " + relative_time(b->time) + " \u00b7 " + b->summary;
    else return;
    std::string line_text = editor_->buffer().line_text(line);
    Pt end_pt = point(line_text, line, line_text.size(), text_rect);
    Color c = ctx.theme->fg_muted;
    c.a *= 0.75f;
    ctx.r->add_text(end_pt.x + cached_char_w_ * 4.0f, end_pt.y + ctx.font->ascender(), text, c, *ctx.font);
}

void EditorView::render_indent_guides(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    Color c = ctx.theme->indent_guide;
    const size_t unit = static_cast<size_t>(std::max(1, IndentSettings::width));
    auto indent_of = [&](size_t line, bool& blank) {
        std::string t = buf.line_text(line);
        size_t i = leading_ws(t);
        blank = i == t.size();
        if (blank && virt() && editor_->computed_indent(line) >= 0) {  // virtual: blank lines have an indent too
            blank = false;
            return static_cast<size_t>(editor_->computed_indent(line));
        }
        return dcol(t, line, i);
    };
    float x0 = text_rect.x + TEXT_PAD - view_x_;
    for (size_t line = first; line < end; line++) {
        bool blank;
        size_t ind = indent_of(line, blank);
        if (blank) {  // blank lines continue the guides of the next non-blank line
            ind = 0;
            for (size_t n = line + 1; n < std::min(buf.line_count(), line + 50); n++) {
                bool b2;
                size_t i2 = indent_of(n, b2);
                if (!b2) { ind = i2; break; }
            }
        }
        for (size_t col = unit; col < ind + (blank ? 1 : 0) && col <= ind; col += unit)
            ctx.r->add_rect({x0 + static_cast<float>(col - unit) * cached_char_w_, row_y(line, text_rect), 1.0f, cached_line_h_}, c);
    }
}

void EditorView::render_find_matches(RenderContext& ctx, Rect text_rect) {
    if (!find_.is_open()) return;
    find_.refresh(*editor_);
    const auto& ms = find_.matches();
    if (ms.empty()) return;
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    size_t lo = buf.line_start(first);
    size_t hi = end < buf.line_count() ? buf.line_start(end) : buf.length();
    auto it = std::lower_bound(ms.begin(), ms.end(), lo, [](const FindBar::Match& m, size_t o) { return m.start < o; });
    Color hl = ctx.theme->find_match;
    Color cur = ctx.theme->find_match_active;
    for (; it != ms.end() && it->start < hi; ++it) {
        Position a = buf.position_of(it->start), b = buf.position_of(it->start + it->len);
        if (a.line != b.line) continue;  // single-line queries only
        std::string line = buf.line_text(a.line);
        Pt p0 = point(line, a.line, a.col, text_rect), p1 = point(line, b.line, b.col, text_rect);
        bool is_cur = static_cast<int>(it - ms.begin()) == find_.current();
        if (p1.y == p0.y)
            ctx.r->add_rect({p0.x, p0.y, p1.x - p0.x, cached_line_h_}, is_cur ? cur : hl, 2.0f);
        else  // match split by a wrap: highlight from its start to the row end
            ctx.r->add_rect({p0.x, p0.y, text_rect.x + text_rect.w - p0.x, cached_line_h_}, is_cur ? cur : hl, 2.0f);
    }
}

void EditorView::render_cursors(RenderContext& ctx, Rect text_rect) {
    if (!cursor_visible_ || !focused_) return;
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    for (auto& sel : editor_->cursors().selections()) {
        Position pos = sel.head;
        if (pos.line < first || pos.line >= end) continue;
        Pt pt = point(buf.line_text(pos.line), pos.line, pos.col, text_rect);
        float x = pt.x, y = pt.y;
        if (&sel == &editor_->cursors().selections().front()) {
            // Glide in document space so scrolling doesn't make the caret lag behind the text.
            float doc_x = x + view_x_, doc_y = y + view_y_;
            if (std::abs(doc_y - caret_y_.target) > cached_line_h_ * 8.0f) {
                caret_x_.snap(doc_x);
                caret_y_.snap(doc_y);
            }
            x = caret_x_.follow(doc_x, anim::FAST, 0.3f) - view_x_;
            y = caret_y_.follow(doc_y, anim::FAST, 0.3f) - view_y_;
        }
        ctx.r->add_rect({x, y, 2.0f, cached_line_h_}, ctx.theme->cursor_color);
    }
}

float EditorView::minimap_width() const {
    return show_minimap_ && bounds_.w > 500.0f ? MINIMAP_W : 0.0f;
}

// First document line shown at the top of the minimap: the minimap window moves
// proportionally with the editor viewport when the file is taller than it.
size_t EditorView::minimap_first_line(Rect r) const {
    size_t total = editor_->buffer().line_count();
    size_t fits = static_cast<size_t>(r.h / MINIMAP_LINE_H);
    if (total <= fits) return 0;
    size_t vis = static_cast<size_t>(r.h / cached_line_h_);
    float top = view_y_ / cached_line_h_;
    float denom = static_cast<float>(total > vis ? total - vis : 1);
    return static_cast<size_t>(std::clamp(top / denom, 0.0f, 1.0f) * static_cast<float>(total - fits));
}

void EditorView::reveal_cursor() {
    if (!editor_) return;
    if (!wrapping()) {
        editor_->scroll_to_cursor(bounds_.w - cached_gutter_w_ - minimap_width(), bounds_.h, cached_char_w_, cached_line_h_);
        return;
    }
    Position p = editor_->cursors().primary_pos();
    std::string text = editor_->buffer().line_text(p.line);
    auto segs = segments(text);
    size_t seg = static_cast<size_t>(std::upper_bound(segs.begin(), segs.end(), p.col) - segs.begin()) - 1;
    float y = static_cast<float>(first_row(p.line) + seg) * cached_line_h_;
    float margin = std::min(cached_line_h_ * 3, std::max(0.0f, (bounds_.h - cached_line_h_) * 0.5f));
    float sy = editor_->scroll_y();
    if (y < sy + margin) sy = std::max(0.0f, y - margin);
    if (y + cached_line_h_ > sy + bounds_.h - margin) sy = y + cached_line_h_ - bounds_.h + margin;
    editor_->set_scroll(0, sy);
}

void EditorView::render_minimap(RenderContext& ctx, Rect r) {
    if (r.w <= 0) return;
    auto& buf = editor_->buffer();
    auto& syntax = editor_->syntax();
    const Theme& th = *ctx.theme;
    ctx.r->add_rect(r, th.bg);
    size_t first = minimap_first_line(r);
    size_t count = static_cast<size_t>(r.h / MINIMAP_LINE_H) + 1;
    float cell = 1.2f;  // horizontal pixels per character
    for (size_t line = first; line < first + count && line < buf.line_count(); line++) {
        std::string text = buf.line_text(line);
        if (text.empty()) continue;
        float y = r.y + static_cast<float>(line - first) * MINIMAP_LINE_H;
        auto tokens = syntax.tokens_for_line(buf, buf.line_start(line), text.size());
        auto color_of = [&](TokenType t) {
            switch (t) {
                case TokenType::Keyword: return th.keyword;
                case TokenType::String: return th.string_color;
                case TokenType::Number: return th.number;
                case TokenType::Comment: return th.comment;
                case TokenType::Function: return th.function_name;
                case TokenType::Type: return th.type_color;
                case TokenType::Preprocessor: return th.preprocessor;
                case TokenType::KeywordControl: return th.keyword_control;
                case TokenType::Constant: return th.constant;
                case TokenType::BuiltinType: return th.type_builtin;
                default: return th.fg;
            }
        };
        size_t lead = text.find_first_not_of(" \t");
        if (lead == std::string::npos) continue;
        if (tokens.empty()) {
            Color c = th.fg;
            c.a = 0.35f;
            ctx.r->add_rect({r.x + 6.0f + lead * cell, y, std::min(r.w - 10.0f, (text.size() - lead) * cell), MINIMAP_LINE_H - 0.5f}, c);
            continue;
        }
        for (auto& tok : tokens) {
            if (text.find_first_not_of(" \t", tok.start) >= tok.start + tok.length) continue;  // whitespace
            float x = r.x + 6.0f + static_cast<float>(tok.start) * cell;
            if (x > r.x + r.w - 4.0f) break;
            Color c = color_of(tok.type);
            c.a = 0.55f;
            ctx.r->add_rect({x, y, std::max(1.0f, tok.length * cell), MINIMAP_LINE_H - 0.5f}, c);
        }
    }
    // Viewport indicator
    float vy = r.y + (view_y_ / cached_line_h_ - static_cast<float>(first)) * MINIMAP_LINE_H;
    float vh = r.h / cached_line_h_ * MINIMAP_LINE_H;
    Color vc = th.fg;
    vc.a = minimap_drag_ ? 0.14f : 0.07f;
    ctx.r->add_rect({r.x, vy, r.w, vh}, vc);
}

Position EditorView::position_at(float x, float y) const {
    Rect text_rect = {bounds_.x + cached_gutter_w_, bounds_.y, bounds_.w - cached_gutter_w_ - minimap_width(), bounds_.h};
    auto& buf = editor_->buffer();
    float rel_y = y - text_rect.y + view_y_;
    float rel_x = x - text_rect.x - TEXT_PAD + view_x_;
    size_t row = rel_y > 0 ? static_cast<size_t>(rel_y / cached_line_h_) : 0;
    row = std::min(row, total_rows() - 1);
    auto [line, seg] = line_at_row(row);
    std::string text = buf.line_text(line);
    auto segs = segments(text);
    seg = std::min(seg, segs.size() - 1);
    size_t seg_dcol = utf8::display_col(text, segs[seg]);
    // Round to the nearest cell edge; byte_col_at snaps to a character boundary.
    size_t dcol = seg_dcol + (rel_x > 0 ? static_cast<size_t>(rel_x / cached_char_w_ + 0.5f) : 0);
    size_t byte = virt() ? editor_->view_byte(line, text, dcol) : utf8::byte_col_at(text, dcol);
    if (seg + 1 < segs.size() && byte >= segs[seg + 1])  // clicking past a wrapped row's end
        byte = utf8::prev_boundary(text, segs[seg + 1]);
    return {line, byte};
}

bool EditorView::handle_key(const KeyEvent& e) {
    if (!editor_ || !e.pressed) return false;
    if (completion_.handle_key(*editor_, e)) {
        reveal_cursor();
        return true;
    }
    if (find_.handle_key(*editor_, e)) {
        reveal_cursor();
        return true;
    }
    if (find_.is_open() && e.keycode == SDLK_ESCAPE) {
        find_.close();
        return true;
    }

    bool ctrl = e.mods & KeyMod::Ctrl;
    bool shift = e.mods & KeyMod::Shift;

    // Reset blink on any key
    cursor_blink_timer_ = 0;
    cursor_visible_ = true;

    auto& cursors = editor_->cursors();
    auto& buf = editor_->buffer();
    const std::vector<Selection> sel_before = cursors.selections();  // for virtual whitespace
    const size_t pref_before = cursors.preferred_col();

    bool nav_handled = true;
    switch (e.keycode) {
        case SDLK_LEFT:
            if (ctrl) cursors.move_word_left(buf, shift);
            else cursors.move_left(buf, shift);
            break;
        case SDLK_RIGHT:
            if (ctrl) cursors.move_word_right(buf, shift);
            else cursors.move_right(buf, shift);
            break;
        case SDLK_UP:
            if (ctrl && shift) editor_->move_line_up();
            else cursors.move_up(buf, shift);
            break;
        case SDLK_DOWN:
            if (ctrl && shift) editor_->move_line_down();
            else cursors.move_down(buf, shift);
            break;
        case SDLK_HOME:
            if (ctrl) cursors.move_to_start(buf, shift);
            else cursors.move_home(buf, shift);
            break;
        case SDLK_END:
            if (ctrl) cursors.move_to_end(buf, shift);
            else cursors.move_end(buf, shift);
            break;
        case SDLK_PAGEUP: {
            size_t page_lines = std::max(static_cast<size_t>(1),
                static_cast<size_t>(bounds_.h / cached_line_h_));
            cursors.move_page_up(buf, page_lines, shift);
            break;
        }
        case SDLK_PAGEDOWN: {
            size_t page_lines = std::max(static_cast<size_t>(1),
                static_cast<size_t>(bounds_.h / cached_line_h_));
            cursors.move_page_down(buf, page_lines, shift);
            break;
        }
        case SDLK_BACKSPACE:
            if (ctrl) editor_->delete_word_backward();
            else editor_->delete_backward();
            break;
        case SDLK_DELETE:
            if (ctrl) editor_->delete_word_forward();
            else editor_->delete_forward();
            break;
        case SDLK_RETURN:
        case SDLK_RETURN2:
            editor_->newline();
            break;
        case SDLK_TAB:
            if (shift) editor_->unindent();
            else editor_->tab();
            break;
        case SDLK_ESCAPE:
            cursors.clear_secondary();
            break;
        default:
            nav_handled = false;
            break;
    }

    if (nav_handled) {
        bool move = e.keycode == SDLK_LEFT || e.keycode == SDLK_RIGHT || e.keycode == SDLK_UP || e.keycode == SDLK_DOWN ||
                    e.keycode == SDLK_HOME || e.keycode == SDLK_END || e.keycode == SDLK_PAGEUP || e.keycode == SDLK_PAGEDOWN;
        bool vertical = (e.keycode == SDLK_UP || e.keycode == SDLK_DOWN || e.keycode == SDLK_PAGEUP ||
                         e.keycode == SDLK_PAGEDOWN) && !(ctrl && shift);
        editor_->virtual_normalize(move ? &sel_before : nullptr,
                                   vertical ? Editor::Nav::Vertical
                                            : e.keycode == SDLK_HOME ? Editor::Nav::Home : Editor::Nav::Other,
                                   pref_before);
        completion_.refilter(*editor_);
        reveal_cursor();
        return true;
    }

    // Ctrl shortcuts
    if (ctrl) {
        bool handled = false;
        switch (e.keycode) {
            case 'a': cursors.select_all(buf); handled = true; break;
            case 'd': cursors.select_word(buf); handled = true; break;
            case 'l': cursors.select_line(buf); handled = true; break;
            case 'z':
                if (shift) editor_->redo(); else editor_->undo();
                handled = true; break;
            case 'y': editor_->redo(); handled = true; break;
            case '/': editor_->toggle_comment(); handled = true; break;
            default: break;
        }
        if (!handled && shift) {
            switch (e.keycode) {
                case 'k': editor_->delete_line(); handled = true; break;
                default: break;
            }
        }
        if (handled) {
            reveal_cursor();
            return true;
        }
    }

    return false;
}

bool EditorView::handle_mouse(const MouseEvent& e) {
    if (!editor_) return false;
    if (completion_.handle_mouse(*editor_, e)) return true;
    // Minimap: click or drag to centre the viewport on that spot.
    if (e.button == 1 && (minimap_drag_ || (e.pressed && !e.motion && minimap_rect_.contains(e.x, e.y) &&
                                            !ruler_.contains(e.x, e.y)))) {
        if (!e.pressed) {
            minimap_drag_ = false;
            return true;
        }
        minimap_drag_ = true;
        float line = static_cast<float>(minimap_first_line(minimap_rect_)) + (e.y - minimap_rect_.y) / MINIMAP_LINE_H;
        float target = line * cached_line_h_ - bounds_.h * 0.5f;
        editor_->set_scroll(editor_->scroll_x(), std::clamp(target, 0.0f, max_scroll()));
        return true;
    }
    if (e.button == 1 && (dragging_thumb_ || (e.pressed && !e.motion && ruler_.contains(e.x, e.y)))) {
        float content = max_scroll() + ruler_.h;
        float thumb_h = std::max(24.0f, ruler_.h * ruler_.h / std::max(content, 1.0f));
        float track = std::max(1.0f, ruler_.h - thumb_h);
        if (!e.pressed) {
            dragging_thumb_ = false;
            return true;
        }
        if (!dragging_thumb_) {
            float thumb_y = ruler_.y + track * (editor_->scroll_y() / std::max(1.0f, max_scroll()));
            // Grab the thumb where clicked; clicking the track centres the thumb there.
            drag_offset_ = (e.y >= thumb_y && e.y < thumb_y + thumb_h) ? e.y - thumb_y : thumb_h * 0.5f;
            dragging_thumb_ = true;
        }
        float t = std::clamp((e.y - drag_offset_ - ruler_.y) / track, 0.0f, 1.0f);
        editor_->set_scroll(editor_->scroll_x(), std::round(t * max_scroll()));
        return true;
    }
    if (find_.handle_mouse(*editor_, e)) {
        reveal_cursor();
        return true;
    }
    if (e.button == 1 && e.pressed) completion_.hide();
    if (e.button == 1 && e.pressed && !e.motion) find_.blur();
    if (!bounds_.contains(e.x, e.y) && !mouse_selecting_) return false;

    if (e.scroll_y != 0.0f) {
        float new_scroll = editor_->scroll_y() - e.scroll_y * cached_line_h_ * 3.0f;
        editor_->set_scroll(editor_->scroll_x(), std::clamp(new_scroll, 0.0f, max_scroll()));
        return true;
    }
    if (e.button != 1) return false;

    cursor_blink_timer_ = 0;
    cursor_visible_ = true;
    if (e.motion) {
        if (mouse_selecting_) editor_->cursors().extend_primary(position_at(e.x, e.y));
        return true;
    }
    if (!e.pressed) {
        mouse_selecting_ = false;
        return true;
    }
    Position pos = position_at(e.x, e.y);
    if (SDL_GetModState() & SDL_KMOD_ALT) {  // Alt+click adds a cursor
        editor_->cursors().add_cursor(pos);
        return true;
    }
    editor_->cursors().set_primary(pos);
    if (e.double_click) editor_->cursors().select_word(editor_->buffer());
    else mouse_selecting_ = true;
    return true;
}

bool EditorView::handle_text(const std::string& text) {
    if (!editor_) return false;
    if (find_.handle_text(*editor_, text)) {
        reveal_cursor();
        return true;
    }
    editor_->type_text(text);

    // Keep cursor in view after typing
    reveal_cursor();

    cursor_blink_timer_ = 0;
    cursor_visible_ = true;
    return true;
}

void EditorView::update(float dt) {
    if (!editor_) return;
    cursor_blink_timer_ += dt;
    if (cursor_blink_timer_ >= 1.0f) {
        cursor_blink_timer_ -= 1.0f;
        cursor_visible_ = !cursor_visible_;
    }
}

} // namespace oh
