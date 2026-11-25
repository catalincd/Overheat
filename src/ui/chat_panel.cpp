#include "ui/chat_panel.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

namespace {
// Greedy word wrap to `cols` display columns; hard-breaks words longer than a line.
std::vector<std::string> wrap(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    cols = std::max<size_t>(cols, 8);
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
        start = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (para.empty()) { out.push_back(""); continue; }
        while (!para.empty()) {
            if (utf8::display_col(para, para.size()) <= cols) { out.push_back(para); break; }
            size_t cut = utf8::byte_col_at(para, cols);
            size_t space = para.rfind(' ', cut);
            if (space != std::string::npos && space > cut / 3) cut = space + 1;
            out.push_back(para.substr(0, cut));
            para.erase(0, cut);
        }
    }
    return out;
}
} // namespace

void ChatPanel::append_reply(const std::string& delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.empty() || messages_.back().role != "assistant") messages_.push_back({"assistant", ""});
    messages_.back().content += delta;
}

void ChatPanel::finish_reply(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    if (!error.empty()) {
        if (messages_.empty() || messages_.back().role != "assistant") messages_.push_back({"assistant", ""});
        messages_.back().content += (messages_.back().content.empty() ? "" : "\n") + std::string("[") + error + "]";
    }
}

void ChatPanel::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
    scroll_ = 0;
}

void ChatPanel::submit() {
    std::string text = draft_;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty() || busy_) return;
    std::vector<ChatMessage> history;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back({"user", text});
        history = messages_;
        busy_ = true;
        follow_ = true;
    }
    draft_.clear();
    if (cb_.send) cb_.send(std::move(history));
}

void ChatPanel::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    auto base = [&](float y, float h) { return y + (h - lh) * 0.5f + ctx.font->ascender(); };
    ctx.r->add_rect(bounds, th.sidebar_bg);

    // Header: title, backend switcher, new chat
    float hb = base(bounds.y, 30.0f);
    float x = ctx.r->add_text(bounds.x + 14.0f, hb, "CHAT", th.fg_muted, *ctx.font);
    float bw = static_cast<float>(backend_name_.size() + 2) * cw + 12.0f;
    backend_btn_ = {x + cw, bounds.y + 5.0f, bw, 20.0f};
    ctx.r->add_rect(backend_btn_, th.surface_hover, 10.0f);
    ctx.r->add_text(backend_btn_.x + 8.0f, hb, backend_name_, th.accent, *ctx.font);
    ctx.r->add_char(backend_btn_.x + bw - 2.0f * cw, hb, 0xEAB4, th.fg_muted, *ctx.font);
    new_btn_ = {bounds.x + bounds.w - 34.0f, bounds.y + 3.0f, 24.0f, 24.0f};
    ctx.r->add_char(new_btn_.x + 6.0f, hb, 0xEA60, th.fg_muted, *ctx.font);  // "+" new chat

    // Input box at the bottom, growing with its content (up to 6 lines).
    size_t cols = static_cast<size_t>(std::max(8.0f, (bounds.w - 44.0f) / cw));
    auto draft_lines = wrap(draft_, cols);
    float in_h = std::min<float>(6.0f, static_cast<float>(draft_lines.size())) * lh + 16.0f;
    input_rect_ = {bounds.x + 10.0f, bounds.y + bounds.h - in_h - 10.0f, bounds.w - 20.0f, in_h};
    bool active = input_focused();
    ctx.r->add_rect(input_rect_, th.input_bg, 8.0f, active ? 1.0f : 0.0f, th.accent);
    ctx.r->push_clip(input_rect_.inset(2.0f));
    float ty = input_rect_.y + 8.0f + ctx.font->ascender();
    if (draft_.empty())
        ctx.r->add_text(input_rect_.x + 10.0f, ty, busy_ ? "Generating... (Esc to stop)" : "Ask anything (Enter to send)",
                        th.fg_muted, *ctx.font);
    size_t first = draft_lines.size() > 6 ? draft_lines.size() - 6 : 0;
    float caret_x = input_rect_.x + 10.0f, caret_y = ty;
    for (size_t i = first; i < draft_lines.size(); i++) {
        caret_x = ctx.r->add_text(input_rect_.x + 10.0f, ty, draft_lines[i], th.fg, *ctx.font);
        caret_y = ty;
        ty += lh;
    }
    if (active) ctx.r->add_rect({caret_x, caret_y - ctx.font->ascender(), 2.0f, lh}, th.accent);
    ctx.r->pop_clip();

    // Conversation
    Rect list{bounds.x, bounds.y + 30.0f, bounds.w, input_rect_.y - bounds.y - 38.0f};
    view_h_ = list.h;
    std::vector<ChatMessage> msgs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        msgs = messages_;
    }
    struct Line { std::string text; Color color; bool code; bool label; };
    std::vector<Line> lines;
    size_t mcols = static_cast<size_t>(std::max(8.0f, (bounds.w - 36.0f) / cw));
    for (auto& m : msgs) {
        bool user = m.role == "user";
        lines.push_back({user ? "You" : backend_name_, user ? th.accent : th.function_name, false, true});
        bool code = false;
        for (auto& raw : wrap(m.content, mcols)) {
            if (raw.rfind("```", 0) == 0) { code = !code; continue; }  // fences become a code background
            lines.push_back({raw, code ? th.string_color : th.fg, code, false});
        }
        lines.push_back({"", th.fg, false, false});
    }
    if (msgs.empty())
        lines.push_back({"Chat with " + backend_name_ + ". Select code and run", th.fg_muted, false, false}),
        lines.push_back({"\"AI: Ask About Selection\" to include it.", th.fg_muted, false, false});
    content_h_ = static_cast<float>(lines.size()) * lh;
    float max_scroll = std::max(0.0f, content_h_ - list.h);
    if (follow_) scroll_ = max_scroll;
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll);
    ctx.r->push_clip(list);
    for (size_t i = 0; i < lines.size(); i++) {
        float y = list.y + static_cast<float>(i) * lh - scroll_;
        if (y + lh < list.y || y > list.y + list.h) continue;
        if (lines[i].code) ctx.r->add_rect({bounds.x + 10.0f, y, bounds.w - 20.0f, lh}, th.input_bg);
        ctx.r->add_text(bounds.x + (lines[i].label ? 14.0f : 18.0f), y + ctx.font->ascender(), lines[i].text,
                        lines[i].color, *ctx.font);
    }
    if (busy_) {  // pulsing dots while the model is working
        float y = list.y + content_h_ - scroll_ - lh;
        ctx.r->add_text(bounds.x + 18.0f, y + ctx.font->ascender(), "...", th.fg_muted, *ctx.font);
    }
    ctx.r->pop_clip();
}

bool ChatPanel::handle_key(const KeyEvent& e) {
    if (!e.pressed || !input_active_) return false;
    bool shift = e.mods & KeyMod::Shift, ctrl = e.mods & KeyMod::Ctrl;
    switch (e.keycode) {
        case SDLK_RETURN:
        case SDLK_RETURN2:
            if (shift) draft_ += '\n';
            else submit();
            return true;
        case SDLK_BACKSPACE:
            if (ctrl) draft_.clear();
            else {
                while (!draft_.empty() && (static_cast<unsigned char>(draft_.back()) & 0xC0) == 0x80) draft_.pop_back();
                if (!draft_.empty()) draft_.pop_back();
            }
            return true;
        case SDLK_ESCAPE:
            if (busy_ && cb_.stop) cb_.stop();
            else input_active_ = false;
            return true;
        default:
            return !ctrl && !(e.mods & KeyMod::Alt);
    }
}

bool ChatPanel::handle_text(const std::string& text) {
    if (!input_active_) return false;
    draft_ += text;
    return true;
}

bool ChatPanel::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        scroll_ -= e.scroll_y * 60.0f;
        follow_ = scroll_ >= content_h_ - view_h_ - 1.0f;
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return true;
    if (new_btn_.contains(e.x, e.y)) { if (cb_.stop) cb_.stop(); clear(); return true; }
    if (backend_btn_.contains(e.x, e.y)) { if (cb_.pick_backend) cb_.pick_backend(); return true; }
    input_active_ = input_rect_.contains(e.x, e.y) || input_active_;
    return true;
}

} // namespace oh
