// Debugger decorations of the editor (EditorView members kept out of
// editor_view.cpp): breakpoint dots in the gutter's lane, the execution arrow and
// line highlight, and inline values at line ends.
#include "ui/editor_view.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>

namespace oh {

namespace {
Color with_alpha(Color c, float a) {
    c.a = a;
    return c;
}
} // namespace

void EditorView::render_debug_lines(RenderContext& ctx, Rect text_rect) {
    if (!debug_marks_ || debug_marks_->exec_line < 0) return;
    size_t line = static_cast<size_t>(debug_marks_->exec_line);
    auto [first, end] = visible_lines(text_rect);
    if (line < first || line >= end) return;
    Color c = debug_marks_->exec_top ? with_alpha(ctx.theme->warning, 0.16f) : with_alpha(ctx.theme->git_added, 0.14f);
    float rows = row_start_.empty() ? 1.0f : static_cast<float>(first_row(line + 1) - first_row(line));
    ctx.r->add_rect({text_rect.x, row_y(line, text_rect), text_rect.w, cached_line_h_ * rows}, c);
}

void EditorView::render_debug_gutter(RenderContext& ctx, Rect gutter_rect) {
    Rect text_rect{gutter_rect.x + gutter_rect.w, gutter_rect.y, 0, gutter_rect.h};
    auto [first, end] = visible_lines(text_rect);
    float d = std::round(std::min(cached_line_h_ * 0.62f, 12.0f));
    float lane_x = gutter_rect.x + 2.0f;
    auto dot_rect = [&](size_t line) {
        float y = row_y(line, text_rect) + (cached_line_h_ - d) * 0.5f;
        return Rect{lane_x + (DEBUG_LANE_W - d) * 0.5f, std::round(y), d, d};
    };
    const Theme& th = *ctx.theme;
    Color red = th.error;
    Color grey = th.line_number;
    // Hover hint: a faint dot where a click would add a breakpoint.
    if (on_gutter_click && ctx.mouse_x >= gutter_rect.x && ctx.mouse_x < gutter_rect.x + DEBUG_LANE_W + 4.0f &&
        gutter_rect.contains(ctx.mouse_x, ctx.mouse_y)) {
        size_t hl = position_at(ctx.mouse_x, ctx.mouse_y).line;
        bool has = false;
        if (debug_marks_)
            for (auto& b : debug_marks_->breakpoints) has |= b.line == hl;
        if (!has && hl >= first && hl < end) ctx.r->add_rect(dot_rect(hl), with_alpha(red, 0.35f), d * 0.5f);
    }
    if (!debug_marks_) return;
    for (const DebugMarks::Breakpoint& b : debug_marks_->breakpoints) {
        if (b.line < first || b.line >= end) continue;
        Rect r = dot_rect(b.line);
        Color c = b.enabled ? red : grey;
        if (b.kind == DebugMarks::Breakpoint::Log) {
            // A diamond: the codicon glyph centred on the dot.
            uint32_t cp = b.verified ? 0xEAAB : 0xEAAA;
            const GlyphInfo* gi = ctx.font->glyph_sized(cp, static_cast<uint32_t>(d + 4.0f));
            if (gi)
                ctx.r->add_glyph(std::round(r.x + (r.w - gi->width) * 0.5f - gi->bearing_x),
                                 std::round(r.y + (r.h - gi->height) * 0.5f + gi->bearing_y), gi, c);
            continue;
        }
        if (!b.verified) {
            ctx.r->add_rect(r, {0, 0, 0, 0}, d * 0.5f, 1.5f, c);  // hollow: not bound by the adapter
        } else {
            ctx.r->add_rect(r, c, d * 0.5f);
        }
        if (b.kind == DebugMarks::Breakpoint::Conditional) {  // "=" inside the dot
            Color bar = b.verified ? th.gutter_bg : c;
            float bw = d * 0.5f, bx = r.x + (d - bw) * 0.5f;
            ctx.r->add_rect({bx, r.y + d * 0.32f, bw, 1.5f}, bar);
            ctx.r->add_rect({bx, r.y + d * 0.58f, bw, 1.5f}, bar);
        }
    }
    if (debug_marks_->exec_line >= 0) {
        size_t line = static_cast<size_t>(debug_marks_->exec_line);
        if (line >= first && line < end) {
            Rect r = dot_rect(line);
            Color c = debug_marks_->exec_top ? th.warning : th.git_added;
            bool on_bp = false;
            for (auto& b : debug_marks_->breakpoints) on_bp |= b.line == line;
            // A solid arrow; over a breakpoint it shrinks to sit inside the red dot.
            const GlyphInfo* gi = ctx.font->glyph_sized(0x25B6, static_cast<uint32_t>(on_bp ? d * 0.8f : d + 3.0f));
            if (gi)
                ctx.r->add_glyph(std::round(r.x + (r.w - gi->width) * 0.5f - gi->bearing_x),
                                 std::round(r.y + (r.h - gi->height) * 0.5f + gi->bearing_y), gi, c);
        }
    }
}

void EditorView::render_inline_values(RenderContext& ctx, Rect text_rect) {
    if (!debug_marks_ || debug_marks_->inline_values.empty()) return;
    auto [first, end] = visible_lines(text_rect);
    auto& buf = editor_->buffer();
    Color c = with_alpha(ctx.theme->fg_muted, 0.8f);
    Color bg = with_alpha(ctx.theme->fg_muted, 0.10f);
    for (auto& [line, text] : debug_marks_->inline_values) {
        if (line < first || line >= end || line_has_diagnostic(line)) continue;
        std::string lt = buf.line_text(line);
        Pt p = point(lt, line, lt.size(), text_rect);
        float x = p.x + cached_char_w_ * 3.0f;
        float w = static_cast<float>(utf8::display_col(text, text.size())) * cached_char_w_;
        ctx.r->add_rect({x - 4.0f, p.y + 2.0f, w + 8.0f, cached_line_h_ - 4.0f}, bg, 3.0f);
        ctx.r->add_text(x, p.y + ctx.font->ascender(), text, c, *ctx.font);
    }
}

} // namespace oh
