#include "ui/editor_view.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
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
    Rect text_rect = {bounds.x + gw, bounds.y, bounds.w - gw, bounds.h};

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
        if (cursor_line >= first && cursor_line < end)
            ctx.r->add_rect({text_rect.x, row_y(cursor_line, text_rect), text_rect.w, cached_line_h_},
                            ctx.theme->current_line);
    }
    render_find_matches(ctx, text_rect);
    render_selections(ctx, text_rect);
    render_text(ctx, text_rect);
    render_diagnostics(ctx, text_rect);
    render_blame(ctx, text_rect);
    render_cursors(ctx, text_rect);
    ctx.r->pop_clip();
    render_scrollbar(ctx, text_rect);
    find_.render(ctx, bounds);
    if (completion_.visible()) {
        const Position& a = completion_.anchor();
        std::string line = editor_->buffer().line_text(a.line);
        completion_.render(ctx, col_x(line, a.col, text_rect), row_y(a.line, text_rect), cached_line_h_, bounds);
    }
}

void EditorView::render_gutter(RenderContext& ctx, Rect gutter_rect) {
    auto& buf = editor_->buffer();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_y = view_y_;

    // Gutter background
    ctx.r->add_rect(gutter_rect, ctx.theme->gutter_bg);

    size_t first_line = static_cast<size_t>(scroll_y / line_h);
    size_t visible_lines = static_cast<size_t>(gutter_rect.h / line_h) + 2;
    size_t total_lines = buf.line_count();

    // Current cursor line for highlighting
    size_t cursor_line = editor_->cursors().primary_pos().line;

    for (size_t i = 0; i < visible_lines && (first_line + i) < total_lines; i++) {
        size_t line_num = first_line + i + 1; // 1-based
        float y = gutter_rect.y + static_cast<float>(i) * line_h -
                  std::fmod(scroll_y, line_h);

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

float EditorView::row_y(size_t line, Rect text_rect) const {
    return text_rect.y + static_cast<float>(line) * cached_line_h_ - view_y_;
}

float EditorView::col_x(const std::string& line_text, size_t byte_col, Rect text_rect) const {
    return text_rect.x + TEXT_PAD - view_x_ +
           static_cast<float>(utf8::display_col(line_text, byte_col)) * cached_char_w_;
}

std::pair<size_t, size_t> EditorView::visible_lines(Rect text_rect) const {
    size_t first = static_cast<size_t>(std::max(0.0f, view_y_) / cached_line_h_);
    size_t count = static_cast<size_t>(text_rect.h / cached_line_h_) + 2;
    size_t end = std::min(first + count, editor_->buffer().line_count());
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

    for (size_t line_idx = first; line_idx < end; line_idx++) {
        std::string line = buf.line_text(line_idx);
        float y = row_y(line_idx, text_rect);
        size_t line_len = line.size();
        auto tokens = syntax.tokens_for_line(line_idx, buf.line_start(line_idx), line_len);

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
        for (const FontAtlas::Shaped& g : it->second) {
            if (g.cluster >= line_len) continue;
            char lead = line[g.cluster];
            if (lead == '\t' || lead == '\r') continue;
            if (g.cluster != prev_cluster) { prev_cluster = g.cluster; in_cluster = 0; }
            float x = x_base + static_cast<float>(col_of[g.cluster]) * char_w + in_cluster + g.x_offset;
            if (x > right_edge) break;
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
            float x0 = col_x(line, line_idx == mn.line ? mn.col : 0, text_rect);
            float x1 = col_x(line, line_idx == mx.line ? mx.col : line.size(), text_rect);
            if (line_idx != mx.line) x1 += cached_char_w_;  // show the selected newline
            if (x1 <= x0) continue;
            ctx.r->add_rect({x0, row_y(line_idx, text_rect), x1 - x0, cached_line_h_},
                            ctx.theme->selection);
        }
    }
}

float EditorView::max_scroll() const {
    return std::max(0.0f, static_cast<float>(editor_->buffer().line_count() - 1) * cached_line_h_);
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
            float x0 = col_x(text, c0, text_rect);
            float x1 = std::max(col_x(text, c1, text_rect), x0 + cached_char_w_);
            float y = row_y(line, text_rect) + cached_line_h_ - 5.0f;
            ctx.r->add_squiggle({x0, y, x1 - x0, 5.0f}, color_of(d.severity), 1.2f, 6.0f, 1.2f);
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
        float x = col_x(text, text.size(), text_rect) + cached_char_w_ * 4.0f;
        ctx.r->add_text(x, row_y(line, text_rect) + ctx.font->ascender(), msg, c, *ctx.font);
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
    float x = col_x(line_text, line_text.size(), text_rect) + cached_char_w_ * 4.0f;
    Color c = ctx.theme->fg_muted;
    c.a *= 0.75f;
    ctx.r->add_text(x, row_y(line, text_rect) + ctx.font->ascender(), text, c, *ctx.font);
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
    Color hl = ctx.theme->warning;
    hl.a = 0.22f;
    Color cur = ctx.theme->warning;
    cur.a = 0.5f;
    for (; it != ms.end() && it->start < hi; ++it) {
        Position a = buf.position_of(it->start), b = buf.position_of(it->start + it->len);
        if (a.line != b.line) continue;  // single-line queries only
        std::string line = buf.line_text(a.line);
        float x0 = col_x(line, a.col, text_rect), x1 = col_x(line, b.col, text_rect);
        bool is_cur = static_cast<int>(it - ms.begin()) == find_.current();
        ctx.r->add_rect({x0, row_y(a.line, text_rect), x1 - x0, cached_line_h_}, is_cur ? cur : hl, 2.0f);
    }
}

void EditorView::render_cursors(RenderContext& ctx, Rect text_rect) {
    if (!cursor_visible_ || !focused_) return;
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    for (auto& sel : editor_->cursors().selections()) {
        Position pos = sel.head;
        if (pos.line < first || pos.line >= end) continue;
        float x = col_x(buf.line_text(pos.line), pos.col, text_rect);
        float y = row_y(pos.line, text_rect);
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

void EditorView::render_minimap(RenderContext& /*ctx*/, Rect /*minimap_rect*/) {}

Position EditorView::position_at(float x, float y) const {
    Rect text_rect = {bounds_.x + cached_gutter_w_, bounds_.y, bounds_.w - cached_gutter_w_, bounds_.h};
    auto& buf = editor_->buffer();
    float rel_y = y - text_rect.y + view_y_;
    float rel_x = x - text_rect.x - TEXT_PAD + view_x_;
    size_t line = rel_y > 0 ? static_cast<size_t>(rel_y / cached_line_h_) : 0;
    line = std::min(line, buf.line_count() - 1);
    // Round to the nearest cell edge; byte_col_at snaps to a character boundary.
    size_t dcol = rel_x > 0 ? static_cast<size_t>(rel_x / cached_char_w_ + 0.5f) : 0;
    return {line, utf8::byte_col_at(buf.line_text(line), dcol)};
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
        completion_.refilter(*editor_);
        editor_->scroll_to_cursor(bounds_.w - cached_gutter_w_, bounds_.h,
                                  cached_char_w_, cached_line_h_);
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
            editor_->scroll_to_cursor(bounds_.w - cached_gutter_w_, bounds_.h,
                                      cached_char_w_, cached_line_h_);
            return true;
        }
    }

    return false;
}

bool EditorView::handle_mouse(const MouseEvent& e) {
    if (!editor_) return false;
    if (completion_.handle_mouse(*editor_, e)) return true;
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
        float max_scroll = static_cast<float>(editor_->buffer().line_count() - 1) * cached_line_h_;
        float new_scroll = editor_->scroll_y() - e.scroll_y * cached_line_h_ * 3.0f;
        editor_->set_scroll(editor_->scroll_x(), std::clamp(new_scroll, 0.0f, max_scroll));
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
    editor_->insert_text(text);

    // Keep cursor in view after typing
    editor_->scroll_to_cursor(bounds_.w - cached_gutter_w_, bounds_.h,
                              cached_char_w_, cached_line_h_);

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
