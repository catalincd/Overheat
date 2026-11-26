#include "ui/debug_console.h"
#include "debug/debugger.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void DebugConsole::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    const FontAtlas& f = *ctx.font;
    float lh = f.line_height(), cw = f.char_width();
    line_h_ = lh;
    ctx.r->add_rect(bounds, th.terminal_bg);
    if (!dbg_) return;
    if (dbg_->console_generation() != seen_gen_) {  // new output follows the end unless scrolled up
        seen_gen_ = dbg_->console_generation();
        if (scroll_ < 1.0f) scroll_ = 0;
    }
    float pad = 10.0f;
    Rect input{bounds.x + 6.0f, bounds.y + bounds.h - lh - 10.0f, bounds.w - 12.0f, lh + 6.0f};
    Rect out{bounds.x, bounds.y + 4.0f, bounds.w, input.y - bounds.y - 6.0f};
    visible_lines_ = static_cast<size_t>(std::max(1.0f, out.h / lh));

    // Output, bottom-aligned.
    const auto& lines = dbg_->console();
    size_t total = lines.size();
    size_t skip = static_cast<size_t>(scroll_);
    ctx.r->push_clip(out);
    float y = out.y + out.h - lh;
    for (size_t k = 0; k + skip < total && y + lh > out.y; k++, y -= lh) {
        const ConsoleEntry& e = lines[total - 1 - skip - k];
        Color c = th.terminal_fg;
        switch (e.kind) {
            case ConsoleEntry::Err:
            case ConsoleEntry::Error: c = th.error; break;
            case ConsoleEntry::Info: c = th.fg_muted; break;
            case ConsoleEntry::Input: c = th.accent; break;
            case ConsoleEntry::Result: c = th.fg; break;
            case ConsoleEntry::Important: c = th.warning; break;
            default: break;
        }
        ctx.r->add_text(bounds.x + pad, y + f.ascender(), e.text, c, f);
    }
    ctx.r->pop_clip();

    // REPL input.
    bool active = focused_;
    ctx.r->add_rect(input, th.input_bg, 4.0f, active ? 1.0f : 0.0f, th.accent);
    float b = input.y + (input.h - lh) * 0.5f + f.ascender();
    ctx.r->add_char(input.x + 6.0f, b, 0xEAB6, th.accent, f);  // chevron prompt
    float tx = input.x + 8.0f + cw * 1.6f;
    ctx.r->push_clip({tx, input.y, input.x + input.w - tx - 4.0f, input.h});
    if (input_.empty()) {
        std::string ph = !dbg_->active() ? "Start debugging to evaluate expressions"
                       : dbg_->stopped() ? "Evaluate an expression in the selected frame (-exec <gdb command>)"
                                         : "Pause to evaluate expressions";
        ctx.r->add_text(tx, b, ph, th.fg_muted, f);
    } else {
        ctx.r->add_text(tx, b, input_, th.fg, f);
    }
    if (active) {
        float cx = tx + static_cast<float>(utf8::display_col(input_, caret_)) * cw;
        ctx.r->add_rect({cx, input.y + 4.0f, 2.0f, input.h - 8.0f}, th.cursor_color);
    }
    ctx.r->pop_clip();
}

bool DebugConsole::handle_key(const KeyEvent& e) {
    if (!e.pressed || !focused_) return false;
    bool ctrl = e.mods & KeyMod::Ctrl;
    switch (e.keycode) {
        case SDLK_RETURN:
        case SDLK_RETURN2:
        case SDLK_KP_ENTER:
            if (!input_.empty()) {
                if (history_.empty() || history_.back() != input_) history_.push_back(input_);
                history_pos_ = history_.size();
                if (dbg_) dbg_->evaluate_repl(input_);
                input_.clear();
                caret_ = 0;
                scroll_ = 0;
            }
            return true;
        case SDLK_BACKSPACE:
            if (caret_ > 0) {
                size_t p = utf8::prev_boundary(input_, caret_);
                input_.erase(p, caret_ - p);
                caret_ = p;
            }
            return true;
        case SDLK_DELETE:
            if (caret_ < input_.size()) input_.erase(caret_, utf8::next_boundary(input_, caret_) - caret_);
            return true;
        case SDLK_LEFT:
            if (caret_ > 0) caret_ = utf8::prev_boundary(input_, caret_);
            return true;
        case SDLK_RIGHT:
            if (caret_ < input_.size()) caret_ = utf8::next_boundary(input_, caret_);
            return true;
        case SDLK_HOME: caret_ = 0; return true;
        case SDLK_END: caret_ = input_.size(); return true;
        case SDLK_UP:
            if (history_pos_ > 0) {
                input_ = history_[--history_pos_];
                caret_ = input_.size();
            }
            return true;
        case SDLK_DOWN:
            if (history_pos_ + 1 < history_.size()) {
                input_ = history_[++history_pos_];
            } else {
                history_pos_ = history_.size();
                input_.clear();
            }
            caret_ = input_.size();
            return true;
        case SDLK_PAGEUP:
            scroll_ = std::min(scroll_ + static_cast<float>(visible_lines_),
                               dbg_ ? static_cast<float>(dbg_->console().size()) - 1.0f : 0.0f);
            scroll_ = std::max(scroll_, 0.0f);
            return true;
        case SDLK_PAGEDOWN:
            scroll_ = std::max(0.0f, scroll_ - static_cast<float>(visible_lines_));
            return true;
        case SDLK_L:
            if (ctrl && dbg_) {
                dbg_->clear_console();
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool DebugConsole::handle_text(const std::string& text) {
    if (!focused_) return false;
    input_.insert(caret_, text);
    caret_ += text.size();
    return true;
}

bool DebugConsole::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f && dbg_) {
        float max_scroll = std::max(0.0f, static_cast<float>(dbg_->console().size()) - static_cast<float>(visible_lines_));
        scroll_ = std::clamp(scroll_ + e.scroll_y * 3.0f, 0.0f, max_scroll);
    }
    return true;
}

} // namespace oh
