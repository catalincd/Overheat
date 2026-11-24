#include "ui/editor_view.h"
#include "renderer/font_atlas.h"
#include "renderer/text_renderer.h"
#include "renderer/ui_renderer.h"
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
    return static_cast<float>(digits) * font.char_width() + 24.0f;
}

void EditorView::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    if (!editor_) return;

    // Update cursor blink
    cursor_blink_timer_ += ctx.dt;
    if (cursor_blink_timer_ >= 0.5f) {
        cursor_blink_timer_ -= 0.5f;
        cursor_visible_ = !cursor_visible_;
    }

    float gw = gutter_width(*ctx.font);
    Rect gutter_rect = {bounds.x, bounds.y, gw, bounds.h};
    Rect text_rect = {bounds.x + gw, bounds.y, bounds.w - gw, bounds.h};

    // Editor background
    ctx.ui->add_rect(bounds, ctx.theme->bg);

    // Render layers
    render_gutter(ctx, gutter_rect);
    render_selections(ctx, text_rect);
    render_text(ctx, text_rect);
    render_cursors(ctx, text_rect);
}

void EditorView::render_gutter(RenderContext& ctx, Rect gutter_rect) {
    auto& buf = editor_->buffer();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_y = editor_->scroll_y();

    // Gutter background
    ctx.ui->add_rect(gutter_rect, ctx.theme->sidebar_bg);

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

        // Right-align within gutter (leave 8px right padding)
        float text_x = gutter_rect.x + gutter_rect.w - 8.0f -
                        static_cast<float>(len) * char_w;

        // Highlight current line number
        Color color = ((first_line + i) == cursor_line)
                           ? ctx.theme->fg
                           : ctx.theme->line_number;

        ctx.text->add_text(text_x, y + ctx.font->ascender(), num_buf, color,
                           *ctx.font);
    }
}

void EditorView::render_text(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    auto& syntax = editor_->syntax();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_x = editor_->scroll_x();
    float scroll_y = editor_->scroll_y();

    size_t first_line = static_cast<size_t>(scroll_y / line_h);
    size_t visible_lines = static_cast<size_t>(text_rect.h / line_h) + 2;
    size_t total_lines = buf.line_count();

    for (size_t i = 0; i < visible_lines && (first_line + i) < total_lines; i++) {
        size_t line_idx = first_line + i;
        std::string line = buf.line_text(line_idx);
        float y = text_rect.y + static_cast<float>(i) * line_h -
                  std::fmod(scroll_y, line_h);

        if (y + line_h < text_rect.y || y > text_rect.y + text_rect.h)
            continue;

        // Get syntax tokens for this line
        size_t line_start = buf.line_start(line_idx);
        size_t line_len = line.size();
        auto tokens = syntax.tokens_for_line(line_idx, line_start, line_len);

        // Build a color array for each character, default to fg
        std::vector<Color> char_colors(line_len, ctx.theme->fg);

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
                case TokenType::Preprocessor: c = ctx.theme->keyword; break;
                case TokenType::Variable:     c = ctx.theme->fg; break;
                case TokenType::Property:     c = ctx.theme->function_name; break;
                case TokenType::Error:        c = ctx.theme->error; break;
                default: break;
            }
            for (uint32_t j = tok.start; j < tok.start + tok.length && j < line_len; j++) {
                char_colors[j] = c;
            }
        }

        // Render each character
        float x_base = text_rect.x + 4.0f - scroll_x;
        for (size_t col = 0; col < line_len; col++) {
            char ch = line[col];
            if (ch == '\n' || ch == '\r') continue;

            float x = x_base + static_cast<float>(col) * char_w;
            // Skip characters outside visible area
            if (x + char_w < text_rect.x || x > text_rect.x + text_rect.w) continue;

            if (ch == '\t') {
                // Tab — just skip the space, it already gets spacing from col width
                continue;
            }

            ctx.text->add_char(x, y + ctx.font->ascender(),
                               static_cast<uint32_t>(ch),
                               char_colors[col], *ctx.font);
        }
    }
}

void EditorView::render_selections(RenderContext& ctx, Rect text_rect) {
    auto& buf = editor_->buffer();
    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_x = editor_->scroll_x();
    float scroll_y = editor_->scroll_y();

    size_t first_line = static_cast<size_t>(scroll_y / line_h);
    size_t visible_lines = static_cast<size_t>(text_rect.h / line_h) + 2;

    for (auto& sel : editor_->cursors().selections()) {
        if (sel.empty()) continue;

        Position sel_min = sel.min();
        Position sel_max = sel.max();

        for (size_t i = 0; i < visible_lines; i++) {
            size_t line_idx = first_line + i;
            if (line_idx >= buf.line_count()) break;
            if (line_idx < sel_min.line || line_idx > sel_max.line) continue;

            size_t line_len = buf.line_length(line_idx);
            size_t start_col = 0;
            size_t end_col = line_len;

            if (line_idx == sel_min.line) start_col = sel_min.col;
            if (line_idx == sel_max.line) end_col = sel_max.col;

            if (start_col >= end_col && line_idx != sel_max.line) {
                // Select to end of line (including newline space)
                end_col = line_len + 1;
            }
            if (start_col >= end_col) continue;

            float y = text_rect.y + static_cast<float>(line_idx - first_line) * line_h -
                      std::fmod(scroll_y, line_h);
            float x = text_rect.x + 4.0f - scroll_x +
                      static_cast<float>(start_col) * char_w;
            float w = static_cast<float>(end_col - start_col) * char_w;

            ctx.ui->add_rect({x, y, w, line_h}, ctx.theme->selection);
        }
    }

    // Also highlight the current line (subtle)
    size_t cursor_line = editor_->cursors().primary_pos().line;
    if (cursor_line >= first_line && cursor_line < first_line + visible_lines) {
        float y = text_rect.y +
                  static_cast<float>(cursor_line - first_line) * line_h -
                  std::fmod(scroll_y, line_h);
        Color line_highlight = ctx.theme->fg;
        line_highlight.a = 0.04f;
        ctx.ui->add_rect({text_rect.x, y, text_rect.w, line_h}, line_highlight);
    }
}

void EditorView::render_cursors(RenderContext& ctx, Rect text_rect) {
    if (!cursor_visible_ && focused_) return;

    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();
    float scroll_x = editor_->scroll_x();
    float scroll_y = editor_->scroll_y();

    size_t first_line = static_cast<size_t>(scroll_y / line_h);
    size_t visible_lines = static_cast<size_t>(text_rect.h / line_h) + 2;

    for (auto& sel : editor_->cursors().selections()) {
        Position pos = sel.head;
        if (pos.line < first_line || pos.line >= first_line + visible_lines) continue;

        float x = text_rect.x + 4.0f - scroll_x +
                  static_cast<float>(pos.col) * char_w;
        float y = text_rect.y +
                  static_cast<float>(pos.line - first_line) * line_h -
                  std::fmod(scroll_y, line_h);

        // 2px wide cursor bar
        ctx.ui->add_rect({x, y, 2.0f, line_h}, ctx.theme->cursor_color);
    }
}

void EditorView::render_minimap(RenderContext& /*ctx*/, Rect /*minimap_rect*/) {
    // Minimap rendering — deferred for future implementation
}

Position EditorView::pixel_to_position(float x, float y, Rect text_rect,
                                       const FontAtlas& font) const {
    if (!editor_) return {0, 0};

    float line_h = font.line_height();
    float char_w = font.char_width();
    float scroll_x = editor_->scroll_x();
    float scroll_y = editor_->scroll_y();

    float rel_y = y - text_rect.y + scroll_y;
    float rel_x = x - text_rect.x - 4.0f + scroll_x;

    size_t line = 0;
    if (rel_y > 0) {
        line = static_cast<size_t>(rel_y / line_h);
    }
    line = std::min(line, editor_->buffer().line_count() - 1);

    size_t col = 0;
    if (rel_x > 0) {
        col = static_cast<size_t>((rel_x + char_w * 0.5f) / char_w);
    }
    size_t line_len = editor_->buffer().line_length(line);
    col = std::min(col, line_len);

    return {line, col};
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

    switch (e.keycode) {
        case SDLK_LEFT:
            if (ctrl) cursors.move_word_left(buf, shift);
            else cursors.move_left(buf, shift);
            return true;
        case SDLK_RIGHT:
            if (ctrl) cursors.move_word_right(buf, shift);
            else cursors.move_right(buf, shift);
            return true;
        case SDLK_UP:
            if (ctrl && shift) {
                editor_->move_line_up();
            } else {
                cursors.move_up(buf, shift);
            }
            return true;
        case SDLK_DOWN:
            if (ctrl && shift) {
                editor_->move_line_down();
            } else {
                cursors.move_down(buf, shift);
            }
            return true;
        case SDLK_HOME:
            if (ctrl) cursors.move_to_start(buf, shift);
            else cursors.move_home(buf, shift);
            return true;
        case SDLK_END:
            if (ctrl) cursors.move_to_end(buf, shift);
            else cursors.move_end(buf, shift);
            return true;
        case SDLK_PAGEUP: {
            size_t page_lines = std::max(static_cast<size_t>(1), static_cast<size_t>(bounds_.h / 20.0f));
            cursors.move_page_up(buf, page_lines, shift);
            return true;
        }
        case SDLK_PAGEDOWN: {
            size_t page_lines = std::max(static_cast<size_t>(1), static_cast<size_t>(bounds_.h / 20.0f));
            cursors.move_page_down(buf, page_lines, shift);
            return true;
        }
        case SDLK_BACKSPACE:
            if (ctrl) editor_->delete_word_backward();
            else editor_->delete_backward();
            return true;
        case SDLK_DELETE:
            if (ctrl) editor_->delete_word_forward();
            else editor_->delete_forward();
            return true;
        case SDLK_RETURN:
        case SDLK_RETURN2:
            editor_->newline();
            return true;
        case SDLK_TAB:
            if (shift) editor_->unindent();
            else editor_->tab();
            return true;
        case SDLK_ESCAPE:
            cursors.clear_secondary();
            return true;
        default:
            break;
    }

    // Ctrl shortcuts
    if (ctrl) {
        switch (e.keycode) {
            case 'a': cursors.select_all(buf); return true;
            case 'd': cursors.select_word(buf); return true;
            case 'l': cursors.select_line(buf); return true;
            case 'z': buf.undo(); return true;
            case 'y': buf.redo(); return true;
            case '/': editor_->toggle_comment(); return true;
            default: break;
        }
        if (shift) {
            switch (e.keycode) {
                case 'z': buf.redo(); return true;
                case 'k': editor_->delete_line(); return true;
                case 'd': editor_->duplicate_line(); return true;
                default: break;
            }
        }
    }

    return false;
}

bool EditorView::handle_mouse(const MouseEvent& e) {
    if (!editor_) return false;
    if (!bounds_.contains(e.x, e.y) && !mouse_selecting_) return false;

    float gw = 40.0f; // Approximate; in practice we'd cache this
    if (editor_) {
        // We don't have font here directly, so use a rough estimate
        // The actual gutter width is computed during render
    }
    Rect text_rect = {bounds_.x + gw, bounds_.y, bounds_.w - gw, bounds_.h};

    // Scroll
    if (e.scroll_y != 0.0f) {
        float line_h = 20.0f; // Approximate; real line height from font
        float new_scroll = editor_->scroll_y() - e.scroll_y * line_h * 3.0f;
        new_scroll = std::max(0.0f, new_scroll);
        editor_->set_scroll(editor_->scroll_x(), new_scroll);
        return true;
    }

    if (e.button == 1) { // Left button
        if (e.pressed) {
            // Reset blink
            cursor_blink_timer_ = 0;
            cursor_visible_ = true;

            // We need font metrics — use defaults if not available
            // In a real render path, we'd cache the font reference
            // For now, approximate char width/line height
            Position pos = {0, 0};

            // Rough pixel_to_position without font reference
            float line_h = 20.0f;
            float char_w = 9.0f;
            float scroll_y = editor_->scroll_y();
            float scroll_x = editor_->scroll_x();

            float rel_y = e.y - text_rect.y + scroll_y;
            float rel_x = e.x - text_rect.x - 4.0f + scroll_x;

            size_t line = 0;
            if (rel_y > 0) line = static_cast<size_t>(rel_y / line_h);
            line = std::min(line, editor_->buffer().line_count() - 1);

            size_t col = 0;
            if (rel_x > 0) col = static_cast<size_t>((rel_x + char_w * 0.5f) / char_w);
            col = std::min(col, editor_->buffer().line_length(line));

            pos = {line, col};

            if (e.double_click) {
                editor_->cursors().set_primary(pos);
                editor_->cursors().select_word(editor_->buffer());
            } else {
                editor_->cursors().set_primary(pos);
                mouse_selecting_ = true;
            }
            return true;
        } else {
            mouse_selecting_ = false;
            return true;
        }
    }

    return false;
}

bool EditorView::handle_text(const std::string& text) {
    if (!editor_) return false;
    editor_->insert_text(text);

    // Reset cursor blink on typing
    cursor_blink_timer_ = 0;
    cursor_visible_ = true;
    return true;
}

} // namespace oh
