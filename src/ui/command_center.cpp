#include "ui/command_center.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>

namespace oh {

void CommandCenter::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    Renderer2D& r = *ctx.r;
    float cw = font.char_width();
    r.add_rect(bounds, th.tab_bg);

    // The box: centred on the window, about two fifths of its width.
    float bh = std::min(bounds.h - 10.0f, std::round(font.line_height() + 8.0f));
    float bw = std::round(std::clamp(bounds.w * 0.4f, std::min(bounds.w - 24.0f, 240.0f), 600.0f));
    box_ = {std::round(bounds.x + (bounds.w - bw) * 0.5f), std::round(bounds.y + (bounds.h - bh) * 0.5f), bw, bh};
    bool hot = box_.contains(ctx.mouse_x, ctx.mouse_y);
    r.add_rect(box_, hot ? th.surface_hover : th.input_bg, 6.0f, 1.0f, hot ? th.fg_muted : th.border);

    // Search icon + title, centred together; a long title fades out at the right.
    float baseline = std::round(box_.y + (bh - font.line_height()) * 0.5f + font.ascender());
    size_t chars = 0;
    for (unsigned char c : title_) chars += (c & 0xC0) != 0x80;
    float content_w = cw * 2.0f + static_cast<float>(chars) * cw;
    float inner_l = box_.x + 10.0f, inner_r = box_.x + box_.w - 10.0f;
    float x = std::max(inner_l, std::round(box_.x + (box_.w - content_w) * 0.5f));
    r.push_clip({inner_l, box_.y, inner_r - inner_l, bh});
    if (x + content_w > inner_r) r.set_fade(inner_r - 24.0f, inner_r);
    r.add_char(x, baseline, 0xEA6D, th.fg_muted, font);
    r.add_text(x + cw * 2.0f, baseline, title_, hot ? th.fg : th.fg_muted, font);
    r.set_fade(0, 0);
    r.pop_clip();
}

bool CommandCenter::handle_mouse(const MouseEvent& e) {
    if (!e.pressed || e.motion || e.button != 1 || !box_.contains(e.x, e.y)) return false;
    if (on_click_) on_click_();
    return true;
}

} // namespace oh
