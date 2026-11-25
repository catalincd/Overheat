#include "ui/editor_view.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>
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
    render_selections(ctx, text_rect);
    render_text(ctx, text_rect);
    render_cursors(ctx, text_rect);
    ctx.r->pop_clip();
}

void EditorView::render_gutter(RenderContext& ctx, Rect gutter_rect) {
    auto& buf = editor_->buffer();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_y = editor_->scroll_y();

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
    return text_rect.y + static_cast<float>(line) * cached_line_h_ - editor_->scroll_y();
}

float EditorView::col_x(const std::string& line_text, size_t byte_col, Rect text_rect) const {
    return text_rect.x + TEXT_PAD - editor_->scroll_x() +
           static_cast<float>(utf8::display_col(line_text, byte_col)) * cached_char_w_;
}

std::pair<size_t, size_t> EditorView::visible_lines(Rect text_rect) const {
    size_t first = static_cast<size_t>(std::max(0.0f, editor_->scroll_y()) / cached_line_h_);
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

        float x_base = text_rect.x + TEXT_PAD - editor_->scroll_x();
        float baseline = y + ctx.font->ascender();
        size_t dcol = 0;
        for (size_t i = 0; i < line_len;) {
            size_t start = i;
            size_t next = utf8::next_boundary(line, i);
            char lead = line[i];
            size_t width = utf8::cell_width(lead, dcol);
            float x = x_base + static_cast<float>(dcol) * char_w;
            if (x > text_rect.x + text_rect.w) break;
            if (lead != '\t' && lead != '\r' && x + char_w * width >= text_rect.x) {
                uint32_t cp = static_cast<unsigned char>(lead);
                if (cp >= 0x80) {
                    int extra = next - start - 1;
                    cp &= 0x3F >> extra;
                    for (size_t k = start + 1; k < next; k++)
                        cp = (cp << 6) | (static_cast<unsigned char>(line[k]) & 0x3F);
                }
                ctx.r->add_char(x, baseline, cp, byte_colors[start], *ctx.font);
            }
            dcol += width;
            i = next;
        }
    }
}

void EditorView::render_selections(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);

    // Current line first, so selections paint over it.
    size_t cursor_line = editor_->cursors().primary_pos().line;
    if (cursor_line >= first && cursor_line < end)
        ctx.r->add_rect({text_rect.x, row_y(cursor_line, text_rect), text_rect.w, cached_line_h_},
                        ctx.theme->current_line);

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

void EditorView::render_cursors(RenderContext& ctx, Rect text_rect) {
    if (!cursor_visible_ || !focused_) return;
    auto& buf = editor_->buffer();
    auto [first, end] = visible_lines(text_rect);
    for (auto& sel : editor_->cursors().selections()) {
        Position pos = sel.head;
        if (pos.line < first || pos.line >= end) continue;
        float x = col_x(buf.line_text(pos.line), pos.col, text_rect);
        ctx.r->add_rect({x, row_y(pos.line, text_rect), 2.0f, cached_line_h_}, ctx.theme->cursor_color);
    }
}

void EditorView::render_minimap(RenderContext& /*ctx*/, Rect /*minimap_rect*/) {}

Position EditorView::position_at(float x, float y) const {
    Rect text_rect = {bounds_.x + cached_gutter_w_, bounds_.y, bounds_.w - cached_gutter_w_, bounds_.h};
    auto& buf = editor_->buffer();
    float rel_y = y - text_rect.y + editor_->scroll_y();
    float rel_x = x - text_rect.x - TEXT_PAD + editor_->scroll_x();
    size_t line = rel_y > 0 ? static_cast<size_t>(rel_y / cached_line_h_) : 0;
    line = std::min(line, buf.line_count() - 1);
    // Round to the nearest cell edge; byte_col_at snaps to a character boundary.
    size_t dcol = rel_x > 0 ? static_cast<size_t>(rel_x / cached_char_w_ + 0.5f) : 0;
    return {line, utf8::byte_col_at(buf.line_text(line), dcol)};
}

bool EditorView::handle_key(const KeyEvent& e) {
    if (!editor_ || !e.pressed) return false;

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
