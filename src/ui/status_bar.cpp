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
    const Theme& th = *ctx.theme;
    float char_w = ctx.font->char_width();
    float baseline = bounds.y + (bounds.h - ctx.font->line_height()) * 0.5f + ctx.font->ascender();
    ctx.r->add_rect(bounds, th.status_bg);

    // Right segments, laid out right-to-left: language, encoding, indent, position.
    std::vector<std::string> right;
    if (editor_) {
        std::string lang = editor_->syntax().language_id();
        static const std::pair<const char*, const char*> names[] = {
            {"cpp", "C++"}, {"c", "C"}, {"python", "Python"}, {"rust", "Rust"},
            {"javascript", "JavaScript"}, {"typescript", "TypeScript"}, {"go", "Go"},
            {"java", "Java"}, {"lua", "Lua"}, {"bash", "Shell"}, {"json", "JSON"},
            {"markdown", "Markdown"}, {"cmake", "CMake"}, {"glsl", "GLSL"}, {"yaml", "YAML"},
            {"toml", "TOML"}, {"html", "HTML"}, {"css", "CSS"}};
        std::string shown = lang.empty() ? "Plain Text" : lang;
        for (auto& [id, name] : names)
            if (lang == id) shown = name;
        right.push_back(shown);
        right.push_back("UTF-8");
        right.push_back("Spaces: 4");
        auto pos = editor_->cursors().primary_pos();
        // Huge (minified) lines: the byte column, without copying the line every frame.
        size_t col = editor_->buffer().line_length(pos.line) > (1u << 20)
                         ? pos.col
                         : utf8::display_col(editor_->buffer().line_text(pos.line), pos.col);
        char buf[64];
        std::snprintf(buf, sizeof buf, "Ln %zu, Col %zu", pos.line + 1, col + 1);
        right.push_back(buf);
        if (editor_->binary()) right.push_back("Binary (read-only)");
        else if (editor_->large_file()) right.push_back("Large File Mode");
        else if (editor_->read_only()) right.push_back("Read-only");
    }
    float rx = bounds.x + bounds.w - 12.0f;
    float left_limit = bounds.x + bounds.w * 0.35f;
    for (auto& seg : right) {
        float w = static_cast<float>(seg.size()) * char_w;
        if (rx - w < left_limit) break;
        rx -= w;
        ctx.r->add_text(rx, baseline, seg, th.status_fg, *ctx.font);
        rx -= 18.0f;
    }

    float x = bounds.x;
    if (!remote_.empty()) {
        // Remote badge: accent block with the host, like VS Code's remote indicator.
        float w = static_cast<float>(remote_.size() + 3) * char_w + 16.0f;
        ctx.r->add_rect({x, bounds.y, w, bounds.h}, th.accent);
        ctx.r->add_char(x + 8.0f, baseline, 0xEB39, th.tab_active_bg, *ctx.font);
        ctx.r->add_text(x + 8.0f + 2.0f * char_w, baseline, remote_, th.tab_active_bg, *ctx.font);
        x += w;
    }
    x += 12.0f;

    ctx.r->push_clip({bounds.x, bounds.y, std::max(0.0f, rx - bounds.x), bounds.h});
    ctx.r->set_fade(rx - 32.0f, rx);
    if (!git_branch_.empty()) {
        ctx.r->add_char(x, baseline, 0xEA68, th.status_fg, *ctx.font);
        x = ctx.r->add_text(x + 2.0f * char_w, baseline, git_branch_, th.status_fg, *ctx.font);
        if (ahead_ || behind_) {
            char buf[48];
            std::snprintf(buf, sizeof buf, " %d\u2193 %d\u2191", behind_, ahead_);
            x = ctx.r->add_text(x, baseline, buf, th.status_fg, *ctx.font);
        }
        x += 18.0f;
    }
    if (errors_ || warnings_) {
        ctx.r->add_char(x, baseline, 0xEA87, errors_ ? th.error : th.status_fg, *ctx.font);
        x = ctx.r->add_text(x + 2.0f * char_w, baseline, std::to_string(errors_), th.status_fg, *ctx.font);
        ctx.r->add_char(x + char_w, baseline, 0xEA6C, warnings_ ? th.warning : th.status_fg, *ctx.font);
        x = ctx.r->add_text(x + 3.0f * char_w, baseline, std::to_string(warnings_), th.status_fg, *ctx.font) + 18.0f;
    }
    if (!message_.empty()) {
        ctx.r->add_text(x, baseline, message_, th.fg, *ctx.font);
    } else if (editor_) {
        ctx.r->add_text(x, baseline, editor_->buffer().file_path().empty()
                                         ? std::string("Untitled")
                                         : editor_->buffer().file_path().filename().string(),
                        th.status_fg, *ctx.font);
    }
    ctx.r->set_fade(0, 0);
    ctx.r->pop_clip();
}

} // namespace oh
