#include "ui/rename_box.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void RenameBox::open(Rect anchor, const std::string& text, std::function<void(const std::string&)> on_accept,
                     std::function<void()> on_cancel) {
    anchor_ = anchor;
    text_ = text;
    anchor_sel_ = 0;
    caret_ = text_.size();
    on_accept_ = std::move(on_accept);
    on_cancel_ = std::move(on_cancel);
    visible_ = true;
    busy_ = false;
}

void RenameBox::erase_selection() {
    size_t a = std::min(caret_, anchor_sel_), b = std::max(caret_, anchor_sel_);
    text_.erase(a, b - a);
    caret_ = anchor_sel_ = a;
}

bool RenameBox::handle_key(const KeyEvent& e) {
    if (!visible_) return false;
    if (!e.pressed) return true;
    bool shift = e.mods & KeyMod::Shift, ctrl = e.mods & KeyMod::Ctrl;
    auto move = [&](size_t to) {
        caret_ = to;
        if (!shift) anchor_sel_ = caret_;
    };
    switch (e.keycode) {
        case SDLK_ESCAPE: {
            visible_ = false;
            auto cb = on_cancel_;
            if (cb) cb();
            return true;
        }
        case SDLK_RETURN:
        case SDLK_RETURN2:
        case SDLK_KP_ENTER: {
            if (busy_) return true;
            auto cb = on_accept_;
            if (cb) cb(text_);
            return true;
        }
        case SDLK_LEFT:
            if (!shift && caret_ != anchor_sel_) move(std::min(caret_, anchor_sel_));
            else move(caret_ > 0 ? utf8::prev_boundary(text_, caret_) : 0);
            return true;
        case SDLK_RIGHT:
            if (!shift && caret_ != anchor_sel_) move(std::max(caret_, anchor_sel_));
            else move(caret_ < text_.size() ? utf8::next_boundary(text_, caret_) : caret_);
            return true;
        case SDLK_HOME: move(0); return true;
        case SDLK_END: move(text_.size()); return true;
        case SDLK_BACKSPACE:
            if (caret_ == anchor_sel_ && caret_ > 0) anchor_sel_ = ctrl ? 0 : utf8::prev_boundary(text_, caret_);
            erase_selection();
            return true;
        case SDLK_DELETE:
            if (caret_ == anchor_sel_ && caret_ < text_.size()) anchor_sel_ = utf8::next_boundary(text_, caret_);
            erase_selection();
            return true;
        case SDLK_A:
            if (ctrl) {
                anchor_sel_ = 0;
                caret_ = text_.size();
            }
            return true;
        default:
            return true;  // modal: nothing leaks to the editor underneath
    }
}

bool RenameBox::handle_text(const std::string& text) {
    if (!visible_) return false;
    if (busy_) return true;
    erase_selection();
    text_.insert(caret_, text);
    caret_ = anchor_sel_ = caret_ + text.size();
    return true;
}

bool RenameBox::handle_mouse(const MouseEvent& e) {
    if (!visible_) return false;
    if (rect_.contains(e.x, e.y)) return true;
    if (e.pressed && !e.motion && e.button == 1) {
        visible_ = false;
        auto cb = on_cancel_;
        if (cb) cb();
    }
    return false;
}

void RenameBox::render(RenderContext& ctx, Rect bounds) {
    if (!visible_) return;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    size_t cells = utf8::display_col(text_, text_.size());
    float w = std::clamp((static_cast<float>(cells) + 12.0f) * cw, 220.0f, std::max(220.0f, bounds.w - 16.0f));
    float h = lh + 10.0f;
    float x = std::clamp(anchor_.x - 6.0f, bounds.x + 4.0f, std::max(bounds.x + 4.0f, bounds.x + bounds.w - w - 4.0f));
    float y = anchor_.y - 5.0f;
    rect_ = {std::floor(x), std::floor(y), w, h};
    Color sh = th.shadow;
    sh.a *= 0.5f;
    ctx.r->add_rect({rect_.x - 2, rect_.y + 2, rect_.w + 4, rect_.h + 4}, sh, 7.0f);
    ctx.r->add_rect(rect_, th.input_bg, 5.0f, 1.5f, th.accent);
    float tx = rect_.x + 6.0f, base = rect_.y + 5.0f + ctx.font->ascender();
    ctx.r->push_clip({rect_.x + 2, rect_.y, rect_.w - 4, rect_.h});
    size_t a = std::min(caret_, anchor_sel_), b = std::max(caret_, anchor_sel_);
    if (a != b) {
        float x0 = tx + utf8::display_col(text_, a) * cw, x1 = tx + utf8::display_col(text_, b) * cw;
        ctx.r->add_rect({x0, rect_.y + 4.0f, x1 - x0, lh + 2.0f}, th.selection);
    }
    ctx.r->add_text(tx, base, text_, th.fg, *ctx.font);
    ctx.r->add_rect({tx + utf8::display_col(text_, caret_) * cw, rect_.y + 4.0f, 2.0f, lh + 2.0f}, th.cursor_color);
    const char* hint = busy_ ? "Renaming…" : "Enter to rename";
    float hw = 15.0f * cw;
    if (rect_.w - (static_cast<float>(cells) + 2.0f) * cw > hw + 12.0f)
        ctx.r->add_text(rect_.x + rect_.w - hw - 4.0f, base, hint, th.fg_muted, *ctx.font);
    ctx.r->pop_clip();
}

} // namespace oh
