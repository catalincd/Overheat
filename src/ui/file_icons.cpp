#include "ui/file_icons.h"
#include "ui/file_icon_glyphs.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <cmath>

namespace oh::file_icons {

Style style = Style::Overheat;

namespace {

// Per-hue colours: {dark theme, light theme}. Light variants are darker and a little more
// saturated so marks keep contrast on white. assets/icons/build_icons.py mirrors this
// table for its preview.
const uint32_t kHues[][2] = {
    {0x5b9cff, 0x1f63d6},  // Blue
    {0x8c84ff, 0x5a4fd8},  // Indigo
    {0xc38cf5, 0x8a3fc4},  // Purple
    {0xf07ab4, 0xc4357d},  // Pink
    {0xf26d6d, 0xc93636},  // Red
    {0xf2924c, 0xc4621c},  // Orange
    {0xe8c25a, 0x9a7a00},  // Yellow
    {0x7ccf6c, 0x2f8a2a},  // Green
    {0x4fc7b0, 0x13836f},  // Teal
    {0x56c3e6, 0x137fa6},  // Cyan
    {0x9aa3b2, 0x5f6878},  // Gray
    {0xc49a6c, 0x8a6236},  // Brown
};

Color hue_color(Hue h, const Theme& th) {
    if (h == Hue::Accent) return th.accent;
    return Color::hex(kHues[static_cast<size_t>(h)][th.dark ? 0 : 1]);
}

Color with_alpha(Color c, float a) {
    c.a *= a;
    return c;
}

// Draws a glyph of the icon font with its em box's bottom-left corner at (x, bottom).
void layer(Renderer2D& r, const FontAtlas& font, uint32_t cp, uint32_t px, float x, float bottom, Color c) {
    if (const GlyphInfo* gi = font.icon_glyph(cp, px)) r.add_glyph(x, bottom, gi, c);
}

} // namespace

float size(const FontAtlas& font) {
    return std::max(10.0f, std::round(font.line_height() * 0.84f));
}

float advance(const FontAtlas& font) {
    switch (style) {
        case Style::None: return 0.0f;
        case Style::Minimal: return font.char_width() + 8.0f;
        default: return size(font) + std::round(std::max(5.0f, font.char_width() * 0.7f));
    }
}

float draw(Renderer2D& r, const FontAtlas& font, const Theme& th, float x, float baseline,
           std::string_view name, bool is_dir, bool open) {
    if (style == Style::None) return x;
    if (style == Style::Minimal) {
        if (is_dir) r.add_char(x, baseline, open ? 0xEAF7 : 0xEA83, th.accent, font);
        else r.add_char(x, baseline, 0xEA7B, th.fg_muted, font);
        return x + advance(font);
    }

    float s = size(font);
    auto px = static_cast<uint32_t>(s);
    // Centre on the text line box, snapped to whole pixels so the tile edges stay crisp.
    float mid = baseline - font.ascender() + font.line_height() * 0.5f;
    float top = std::round(mid - s * 0.5f);
    float left = std::round(x);
    float bottom = top + s;

    if (is_dir) {
        FolderIcon fi = for_folder(name);
        Color c = hue_color(fi.hue, th);
        layer(r, font, open ? icon_glyph::folder_open_fill : icon_glyph::folder_fill, px, left, bottom,
              with_alpha(c, th.dark ? 0.30f : 0.22f));
        layer(r, font, open ? icon_glyph::folder_open_line : icon_glyph::folder_line, px, left, bottom, c);
        if (uint32_t badge = open ? fi.badge_open : fi.badge) layer(r, font, badge, px, left, bottom, c);
    } else {
        FileIcon fi = for_file(name);
        Color c = hue_color(fi.hue, th);
        r.add_rect({left, top, s, s}, with_alpha(c, th.dark ? 0.20f : 0.15f), std::round(s * 0.24f));
        layer(r, font, fi.mark, px, left, bottom, c);
    }
    return x + advance(font);
}

} // namespace oh::file_icons
