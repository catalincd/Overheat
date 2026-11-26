#include "ui/debug_toolbar.h"
#include "debug/debugger.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <cmath>

namespace oh {

void DebugToolbar::render(RenderContext& ctx, Rect editor, const Debugger& dbg) {
    buttons_.clear();
    visible_ = dbg.active() && editor.w > 220.0f && editor.h > 60.0f;
    if (!visible_) return;
    const Theme& th = *ctx.theme;
    struct Btn {
        uint32_t icon;
        const char* cmd;
        Color color;
        bool enabled;
    };
    bool stopped = dbg.stopped();
    bool running = dbg.state() == DebugState::Running;
    Btn btns[] = {
        stopped ? Btn{0xEACF, "debug_continue", th.accent, true} : Btn{0xEAD1, "debug_pause", th.accent, running},
        {0xEAD6, "debug_step_over", th.accent, stopped},
        {0xEAD4, "debug_step_into", th.accent, stopped},
        {0xEAD5, "debug_step_out", th.accent, stopped},
        {0xEAD2, "debug_restart", th.git_added, true},
        {0xEAD7, "debug_stop", th.error, true},
    };
    float bw = 30.0f, bh = 26.0f, pad = 4.0f, grip = 10.0f;
    float w = grip + bw * static_cast<float>(std::size(btns)) + pad * 2.0f;
    rect_ = {std::round(editor.x + (editor.w - w) * 0.5f), editor.y + 6.0f, w, bh + pad * 2.0f};
    ctx.r->add_rect({rect_.x + 1.0f, rect_.y + 3.0f, rect_.w, rect_.h}, th.shadow, 6.0f);
    ctx.r->add_rect(rect_, th.surface, 6.0f, 1.0f, th.border);
    // Grip dots (the toolbar does not move yet, but reads as a floating widget).
    for (int i = 0; i < 3; i++)
        ctx.r->add_rect({rect_.x + 6.0f, rect_.y + rect_.h * 0.5f - 6.0f + static_cast<float>(i) * 5.0f, 2.0f, 2.0f},
                        th.fg_muted, 1.0f);
    float x = rect_.x + pad + grip;
    for (auto& b : btns) {
        Rect r{x, rect_.y + pad, bw, bh};
        bool hover = r.contains(ctx.mouse_x, ctx.mouse_y) && b.enabled;
        if (hover) ctx.r->add_rect(r, th.surface_hover, 4.0f);
        Color c = b.enabled ? b.color : th.line_number;
        if (const GlyphInfo* gi = ctx.font->glyph_sized(b.icon, 18)) {
            ctx.r->add_glyph(std::round(r.x + (r.w - gi->width) * 0.5f - gi->bearing_x),
                             std::round(r.y + (r.h - gi->height) * 0.5f + gi->bearing_y), gi, c);
        }
        if (b.enabled) buttons_.push_back({r, b.cmd});
        x += bw;
    }
}

std::string DebugToolbar::hit(float x, float y) const {
    if (!visible_) return {};
    for (auto& [r, cmd] : buttons_)
        if (r.contains(x, y)) return cmd;
    return {};
}

} // namespace oh
