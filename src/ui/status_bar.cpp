#include "ui/status_bar.h"
#include "editor/editor.h"
#include "renderer/font_atlas.h"
#include "renderer/text_renderer.h"
#include "renderer/ui_renderer.h"
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
    ctx.ui->add_rect(bounds, ctx.theme->status_bg);

    // Top border
    ctx.ui->add_rect({bounds.x, bounds.y, bounds.w, 1.0f}, ctx.theme->border);

    float left_x = bounds.x + 12.0f;
    float right_x = bounds.x + bounds.w - 12.0f;

    // Left side: timed message or mode + filename
    if (!message_.empty()) {
        ctx.text->add_text(left_x, text_y, message_, ctx.theme->fg, *ctx.font);
    } else {
        // Mode indicator
        std::string mode = "NORMAL";
        ctx.text->add_text(left_x, text_y, mode, ctx.theme->keyword, *ctx.font);
        left_x += static_cast<float>(mode.size()) * char_w + 16.0f;

        // Separator
        ctx.text->add_text(left_x, text_y, "|", ctx.theme->comment, *ctx.font);
        left_x += char_w + 8.0f;

        // File name
        if (editor_) {
            std::string title = editor_->title();
            if (editor_->is_modified()) title += " [+]";
            ctx.text->add_text(left_x, text_y, title, ctx.theme->fg, *ctx.font);
        }
    }

    // Right side: cursor position | language | git branch | encoding
    // Build right-side segments and render right-to-left
    std::vector<std::pair<std::string, Color>> right_segments;

    // Encoding
    right_segments.push_back({"UTF-8", ctx.theme->comment});

    // Git branch
    if (!git_branch_.empty()) {
        right_segments.push_back({git_branch_, ctx.theme->git_modified});
    }

    // Language
    if (editor_) {
        std::string lang = editor_->syntax().language_id();
        if (!lang.empty()) {
            right_segments.push_back({lang, ctx.theme->comment});
        }
    }

    // Cursor position (Ln:Col)
    if (editor_) {
        auto pos = editor_->cursors().primary_pos();
        char pos_buf[64];
        std::snprintf(pos_buf, sizeof(pos_buf), "Ln %zu, Col %zu",
                      pos.line + 1, pos.col + 1);
        right_segments.push_back({pos_buf, ctx.theme->fg});
    }

    // Render right segments from right to left
    float rx = right_x;
    for (size_t i = 0; i < right_segments.size(); i++) {
        auto& [text, color] = right_segments[i];
        float text_w = static_cast<float>(text.size()) * char_w;
        rx -= text_w;
        ctx.text->add_text(rx, text_y, text, color, *ctx.font);

        if (i + 1 < right_segments.size()) {
            rx -= 8.0f;
            ctx.text->add_text(rx - char_w, text_y, "|", ctx.theme->comment, *ctx.font);
            rx -= char_w + 8.0f;
        }
    }
}

} // namespace oh
