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

std::string TabBar::tab_title(const Tab& tab) const {
    const auto& path = tab.editor->buffer().file_path();
    return path.empty() ? "Untitled" : path.filename().string();
}

float TabBar::tab_width(const Tab& tab) const {
    std::string t = tab_title(tab);
    size_t chars = 0;
    for (unsigned char c : t) chars += (c & 0xC0) != 0x80;
    return std::round(PAD * 2.0f + static_cast<float>(chars) * char_w_ + CLOSE_W + 6.0f);
}

void TabBar::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    char_w_ = ctx.font->char_width();
    ctx.r->add_rect(bounds, th.tab_bg);

    float baseline = bounds.y + (bounds.h - ctx.font->line_height()) * 0.5f + ctx.font->ascender();
    float x = bounds.x - scroll_x_;
    float ul_target_x = -1, ul_target_w = 0;
    for (size_t i = 0; i < tabs_.size(); i++) {
        const Tab& tab = tabs_[i];
        float w = tab_width(tab);
        bool active = static_cast<int>(i) == active_;
        bool hover = !active && ctx.mouse_x >= x && ctx.mouse_x < x + w && ctx.mouse_y >= bounds.y &&
                     ctx.mouse_y < bounds.y + bounds.h;
        if (active) {
            ctx.r->add_rect({x, bounds.y, w, bounds.h}, th.tab_active_bg);
            ul_target_x = x;
            ul_target_w = w;
        } else if (hover) {
            ctx.r->add_rect({x, bounds.y, w, bounds.h}, th.surface_hover);
        }
        ctx.r->add_text(x + PAD, baseline, tab_title(tab), active || hover ? th.fg : th.tab_inactive_fg, *ctx.font);

        // Close glyph; an unsaved tab shows a dot in its place.
        float cx = x + w - PAD - CLOSE_W + 4.0f;
        if (tab.editor->is_modified())
            ctx.r->add_char(cx, baseline, 0x25CF, active ? th.fg : th.tab_inactive_fg, *ctx.font);
        else
            ctx.r->add_char(cx, baseline, 0xEA76, active ? th.fg_muted : th.line_number, *ctx.font);
        x += w;
    }
    if (ul_target_x >= 0) {  // active-tab underline slides between tabs
        float ux = ul_x_.follow(ul_target_x, anim::NORMAL), uw = ul_w_.follow(ul_target_w, anim::NORMAL);
        ctx.r->add_rect({ux, bounds.y + bounds.h - 2.0f, uw, 2.0f}, th.accent);
    }
}

bool TabBar::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        scroll_x_ = std::max(0.0f, scroll_x_ - e.scroll_y * 30.0f);
        return true;
    }
    if (!e.pressed || e.motion) return false;

    float x = bounds_.x - scroll_x_;
    for (size_t i = 0; i < tabs_.size(); i++) {
        float w = tab_width(tabs_[i]);
        if (e.x >= x && e.x < x + w) {
            bool on_close = e.x >= x + w - PAD - CLOSE_W;
            if (e.button == 2 || (e.button == 1 && on_close)) {
                if (on_close_request_) on_close_request_(static_cast<int>(i));
                else close_tab(static_cast<int>(i));
            }
            else if (e.button == 1) set_active(static_cast<int>(i));
            return true;
        }
        x += w;
    }
    return false;
}

} // namespace oh
