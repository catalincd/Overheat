#include "ui/tab_bar.h"
#include "editor/editor.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>

namespace oh {

void TabBar::add_tab(std::shared_ptr<Editor> editor) {
    tabs_.push_back({std::move(editor), false});
    set_active(static_cast<int>(tabs_.size()) - 1);
}

void TabBar::close_tab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;

    tabs_.erase(tabs_.begin() + index);

    if (tabs_.empty()) {
        active_ = -1;
    } else if (active_ >= static_cast<int>(tabs_.size())) {
        set_active(static_cast<int>(tabs_.size()) - 1);
    } else if (active_ > index) {
        active_--;
    } else if (active_ == index) {
        // Stay at same index if possible, otherwise go to last
        if (active_ >= static_cast<int>(tabs_.size())) {
            set_active(static_cast<int>(tabs_.size()) - 1);
        } else {
            set_active(active_);
        }
    }
}

void TabBar::set_active(int index) {
    if (index < -1 || index >= static_cast<int>(tabs_.size())) return;
    active_ = index;
    if (on_change_) on_change_(active_);
}

std::shared_ptr<Editor> TabBar::active_editor() const {
    if (active_ >= 0 && active_ < static_cast<int>(tabs_.size()))
        return tabs_[static_cast<size_t>(active_)].editor;
    return nullptr;
}

void TabBar::next_tab() {
    if (tabs_.empty()) return;
    set_active((active_ + 1) % static_cast<int>(tabs_.size()));
}

void TabBar::prev_tab() {
    if (tabs_.empty()) return;
    int n = static_cast<int>(tabs_.size());
    set_active((active_ - 1 + n) % n);
}

void TabBar::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;

    float char_w = ctx.font->char_width();
    float tab_height = bounds.h;

    // Background
    ctx.r->add_rect(bounds, ctx.theme->tab_bg);

    // Bottom border
    ctx.r->add_rect({bounds.x, bounds.y + bounds.h - 1.0f, bounds.w, 1.0f},
                     ctx.theme->border);

    float x = bounds.x - scroll_x_;
    constexpr float tab_padding = 16.0f;
    constexpr float close_btn_w = 16.0f;

    for (size_t i = 0; i < tabs_.size(); i++) {
        auto& tab = tabs_[i];
        std::string title = tab.editor->title();

        float text_w = static_cast<float>(title.size()) * char_w;
        float tab_w = text_w + tab_padding * 2.0f + close_btn_w;

        // Tab background
        bool is_active = (static_cast<int>(i) == active_);
        Color bg = is_active ? ctx.theme->tab_active_bg : ctx.theme->tab_bg;
        ctx.r->add_rect({x, bounds.y, tab_w, tab_height}, bg);

        // Active tab top accent
        if (is_active) {
            ctx.r->add_rect({x, bounds.y, tab_w, 2.0f}, ctx.theme->keyword);
        }

        // Right border separator
        ctx.r->add_rect({x + tab_w - 1.0f, bounds.y, 1.0f, tab_height},
                         ctx.theme->border);

        // Tab title text
        Color text_color = is_active ? ctx.theme->fg : ctx.theme->comment;
        ctx.r->add_text(x + tab_padding,
                           bounds.y + ctx.font->ascender() + 4.0f,
                           title, text_color, *ctx.font);

        // Close button "x"
        float close_x = x + tab_w - close_btn_w - 4.0f;
        float close_y = bounds.y + ctx.font->ascender() + 4.0f;
        ctx.r->add_text(close_x, close_y, "x", ctx.theme->comment, *ctx.font);

        x += tab_w;
    }
}

bool TabBar::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;

    // Scroll tabs horizontally
    if (e.scroll_y != 0.0f) {
        scroll_x_ -= e.scroll_y * 30.0f;
        scroll_x_ = std::max(0.0f, scroll_x_);
        return true;
    }

    if (!e.pressed) return false;

    float char_w = 9.0f; // Approximate; real comes from font
    float x = bounds_.x - scroll_x_;
    constexpr float tab_padding = 16.0f;
    constexpr float close_btn_w = 16.0f;

    for (size_t i = 0; i < tabs_.size(); i++) {
        auto& tab = tabs_[i];
        std::string title = tab.editor->title();

        float text_w = static_cast<float>(title.size()) * char_w;
        float tab_w = text_w + tab_padding * 2.0f + close_btn_w;

        Rect tab_rect = {x, bounds_.y, tab_w, bounds_.h};
        if (tab_rect.contains(e.x, e.y)) {
            // Check if close button was clicked
            float close_x = x + tab_w - close_btn_w - 4.0f;
            Rect close_rect = {close_x, bounds_.y, close_btn_w + 4.0f, bounds_.h};

            if (e.button == 1 && close_rect.contains(e.x, e.y)) {
                close_tab(static_cast<int>(i));
                return true;
            }

            // Middle-click to close
            if (e.button == 2) {
                close_tab(static_cast<int>(i));
                return true;
            }

            // Left-click to select
            if (e.button == 1) {
                set_active(static_cast<int>(i));
                return true;
            }
        }

        x += tab_w;
    }

    return false;
}

} // namespace oh
