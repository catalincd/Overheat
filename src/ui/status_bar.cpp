#include "ui/status_bar.h"
#include "editor/utf8.h"
#include "editor/editor.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <cstdio>
#include <cstring>

namespace oh {

void StatusBar::set_message(const std::string& msg, float duration) {
    message_ = msg;
    message_timer_ = duration;
}

void StatusBar::update(float dt) {
    if (message_timer_ > 0) {
        message_timer_ -= dt;
        if (message_timer_ <= 0) {
            message_timer_ = 0;
            message_.clear();
        }
    }
}

void StatusBar::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;

    float char_w = ctx.font->char_width();
    float text_y = bounds.y + ctx.font->ascender() + 2.0f;

    // Background
    ctx.r->add_rect(bounds, ctx.theme->status_bg);

    // Top border
    ctx.r->add_rect({bounds.x, bounds.y, bounds.w, 1.0f}, ctx.theme->border);

    float left_x = bounds.x + 12.0f;
    float right_x = bounds.x + bounds.w - 12.0f;

    // Right side first (cursor | language | branch | encoding); it wins when space is short.
    std::vector<std::pair<std::string, Color>> right_segments;
    right_segments.push_back({"UTF-8", ctx.theme->comment});
    if (!git_branch_.empty()) right_segments.push_back({git_branch_, ctx.theme->git_modified});
    if (editor_) {
        std::string lang = editor_->syntax().language_id();
        if (!lang.empty()) right_segments.push_back({lang, ctx.theme->comment});
        auto pos = editor_->cursors().primary_pos();
        size_t col = utf8::display_col(editor_->buffer().line_text(pos.line), pos.col);
        char pos_buf[64];
        std::snprintf(pos_buf, sizeof(pos_buf), "Ln %zu, Col %zu", pos.line + 1, col + 1);
        right_segments.push_back({pos_buf, ctx.theme->fg});
    }

    float rx = right_x;
    for (size_t i = 0; i < right_segments.size(); i++) {
        auto& [text, color] = right_segments[i];
        float text_w = static_cast<float>(text.size()) * char_w;
        if (rx - text_w < left_x) break;
        rx -= text_w;
        ctx.r->add_text(rx, text_y, text, color, *ctx.font);
        if (i + 1 < right_segments.size()) {
            rx -= 8.0f;
            ctx.r->add_text(rx - char_w, text_y, "|", ctx.theme->comment, *ctx.font);
            rx -= char_w + 8.0f;
        }
    }

    // Left side: timed message, or mode + filename, clipped to what is left.
    ctx.r->push_clip({bounds.x, bounds.y, std::max(0.0f, rx - 16.0f - bounds.x), bounds.h});
    ctx.r->set_fade(rx - 48.0f, rx - 16.0f);
    if (!message_.empty()) {
        ctx.r->add_text(left_x, text_y, message_, ctx.theme->fg, *ctx.font);
    } else {
        std::string mode = "NORMAL";
        ctx.r->add_text(left_x, text_y, mode, ctx.theme->keyword, *ctx.font);
        left_x += static_cast<float>(mode.size()) * char_w + 16.0f;
        ctx.r->add_text(left_x, text_y, "|", ctx.theme->comment, *ctx.font);
        left_x += char_w + 8.0f;
        if (editor_) {
            ctx.r->add_text(left_x, text_y, editor_->title(), ctx.theme->fg, *ctx.font);
        }
    }
    ctx.r->set_fade(0, 0);
    ctx.r->pop_clip();
}

} // namespace oh
