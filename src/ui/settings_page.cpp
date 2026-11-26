#include "ui/settings_page.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void SettingsPage::activate(int i, int delta) {
    if (i < 0 || i >= static_cast<int>(rows_.size())) return;
    if (rows_[static_cast<size_t>(i)].change) rows_[static_cast<size_t>(i)].change(delta);
}

void SettingsPage::render(RenderContext& ctx, Rect bounds) {
    if (!visible_) return;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    float t = appear_.follow(1.0f, anim::FAST, 0.01f);
    ctx.r->set_alpha(t);
    Color dim = th.shadow;
    dim.a *= 0.6f;
    ctx.r->add_rect(bounds, dim);

    float row_h = std::round(lh * 2.0f + 18.0f);
    float wob = wobble_.step();  // elastic pop when wobbly = true
    float w = std::min(760.0f, bounds.w - 48.0f) * (1.0f + 0.03f * wob);
    float h = std::min(bounds.h - 80.0f, 64.0f + row_h * static_cast<float>(rows_.size()));
    card_ = {std::floor(bounds.x + (bounds.w - w) * 0.5f),
             std::floor(bounds.y + (bounds.h - h) * 0.5f) - (1 - t) * 10.0f - std::round(wob * 12.0f), w, h};
    ctx.r->add_rect({card_.x - 3, card_.y + 4, card_.w + 6, card_.h + 6}, th.shadow, 14.0f);
    ctx.r->add_rect(card_, th.surface, 12.0f);
    float base = card_.y + 22.0f + ctx.font->ascender();
    ctx.r->add_text(card_.x + 24.0f, base, "Settings", th.fg, *ctx.font);
    ctx.r->add_text(card_.x + card_.w - 24.0f - 34.0f * cw, base, "Esc to close  ·  ↑↓ select  ·  ←→ change",
                    th.fg_muted, *ctx.font);

    Rect list{card_.x, card_.y + 52.0f, card_.w, card_.h - 60.0f};
    // Keep the selected row visible.
    float sel_top = selected_ * row_h, sel_bot = sel_top + row_h;
    if (sel_top < scroll_) scroll_ = sel_top;
    if (sel_bot > scroll_ + list.h) scroll_ = sel_bot - list.h;
    ctx.r->push_clip(list);
    row_rects_.assign(rows_.size(), {});
    minus_rects_.assign(rows_.size(), {});
    plus_rects_.assign(rows_.size(), {});
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& row = rows_[i];
        Rect rr{list.x + 12.0f, list.y + static_cast<float>(i) * row_h - scroll_, list.w - 24.0f, row_h - 6.0f};
        row_rects_[i] = rr;
        bool sel = static_cast<int>(i) == selected_;
        bool hover = rr.contains(ctx.mouse_x, ctx.mouse_y);
        if (sel || hover) ctx.r->add_rect(rr, th.surface_hover, 8.0f);
        if (sel) ctx.r->add_rect({rr.x, rr.y + 10.0f, 2.0f, rr.h - 20.0f}, th.accent, 1.0f);
        float y1 = rr.y + 8.0f + ctx.font->ascender();
        // Text stops (with a fade) before the control column on the right.
        float text_right = rr.x + rr.w - 16.0f - 34.0f * cw;
        ctx.r->push_clip({rr.x, rr.y, text_right - rr.x, rr.h});
        ctx.r->set_fade(text_right - 30.0f, text_right);
        ctx.r->add_text(rr.x + 16.0f, y1, row.label, th.fg, *ctx.font);
        ctx.r->add_text(rr.x + 16.0f, y1 + lh + 2.0f, row.description, th.fg_muted, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();

        float right = rr.x + rr.w - 16.0f;
        float cy = rr.y + rr.h * 0.5f;
        float vb = cy - lh * 0.5f + ctx.font->ascender();
        switch (row.kind) {
            case Row::Toggle: {
                bool on = row.on && row.on();
                Rect track{right - 40.0f, cy - 11.0f, 40.0f, 22.0f};
                ctx.r->add_rect(track, on ? th.accent : th.line_number, 11.0f);
                float knob_x = on ? track.x + track.w - 20.0f : track.x + 2.0f;
                ctx.r->add_rect({knob_x, track.y + 2.0f, 18.0f, 18.0f}, th.tab_active_bg, 9.0f);
                break;
            }
            case Row::Stepper: {
                std::string v = row.value ? row.value() : "";
                Rect plus{right - 28.0f, cy - 13.0f, 28.0f, 26.0f};
                float vw = std::max(4.0f, static_cast<float>(v.size())) * cw + 16.0f;
                Rect minus{plus.x - vw - 28.0f, cy - 13.0f, 28.0f, 26.0f};
                ctx.r->add_rect(minus, th.input_bg, 6.0f);
                ctx.r->add_rect(plus, th.input_bg, 6.0f);
                ctx.r->add_text(minus.x + 10.0f, vb, "-", th.fg, *ctx.font);
                ctx.r->add_text(plus.x + 10.0f, vb, "+", th.fg, *ctx.font);
                ctx.r->add_text(minus.x + minus.w + (vw - static_cast<float>(v.size()) * cw) * 0.5f, vb, v, th.fg, *ctx.font);
                minus_rects_[i] = minus;
                plus_rects_[i] = plus;
                break;
            }
            case Row::Choice: {
                std::string v = row.value ? row.value() : "";
                float vw = static_cast<float>(v.size()) * cw + 40.0f;
                Rect box{right - vw, cy - 14.0f, vw, 28.0f};
                ctx.r->add_rect(box, th.input_bg, 6.0f);
                ctx.r->add_text(box.x + 12.0f, vb, v, th.fg, *ctx.font);
                ctx.r->add_char(box.x + box.w - 20.0f, vb, 0xEAB4, th.fg_muted, *ctx.font);
                break;
            }
            case Row::Button: {
                std::string v = row.value ? row.value() : "Open";
                float vw = static_cast<float>(v.size()) * cw + 28.0f;
                Rect box{right - vw, cy - 14.0f, vw, 28.0f};
                ctx.r->add_rect(box, th.accent, 6.0f);
                ctx.r->add_text(box.x + 14.0f, vb, v, th.tab_active_bg, *ctx.font);
                break;
            }
        }
    }
    ctx.r->pop_clip();
    ctx.r->set_alpha(1.0f);
}

bool SettingsPage::handle_key(const KeyEvent& e) {
    if (!visible_ || !e.pressed) return false;
    int n = static_cast<int>(rows_.size());
    switch (e.keycode) {
        case SDLK_ESCAPE: close(); return true;
        case SDLK_UP: selected_ = std::max(0, selected_ - 1); return true;
        case SDLK_DOWN: selected_ = std::min(n - 1, selected_ + 1); return true;
        case SDLK_LEFT: activate(selected_, -1); return true;
        case SDLK_RIGHT: activate(selected_, +1); return true;
        case SDLK_RETURN: case SDLK_RETURN2: case SDLK_SPACE: activate(selected_, 0); return true;
        default: return true;  // modal: swallow everything else
    }
}

bool SettingsPage::handle_mouse(const MouseEvent& e) {
    if (!visible_) return false;
    if (e.scroll_y != 0.0f) {
        selected_ = std::clamp(selected_ - static_cast<int>(e.scroll_y), 0, static_cast<int>(rows_.size()) - 1);
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return true;
    if (!card_.contains(e.x, e.y)) { close(); return true; }
    for (size_t i = 0; i < row_rects_.size(); i++) {
        if (!row_rects_[i].contains(e.x, e.y)) continue;
        selected_ = static_cast<int>(i);
        if (minus_rects_[i].contains(e.x, e.y)) activate(static_cast<int>(i), -1);
        else if (plus_rects_[i].contains(e.x, e.y)) activate(static_cast<int>(i), +1);
        else if (rows_[i].kind != Row::Stepper) activate(static_cast<int>(i), 0);
        return true;
    }
    return true;
}

} // namespace oh
