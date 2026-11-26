#pragma once
#include <cstdint>
#include <string_view>

namespace oh {

class Renderer2D;
class FontAtlas;
struct Theme;

// File-type icons for the explorer, tabs, Quick Open and breadcrumbs.
//
// The "overheat" style draws a tinted rounded tile (a pixel-snapped quad) with a
// monoline mark from the embedded icon font on top, both in one per-language hue
// that has a dark- and a light-theme variant. Folders are a tinted silhouette, an
// outline and, for well-known folders (src, test, docs...), a small badge.
// The glyphs come from assets/icons/build_icons.py; the name -> icon table is in
// file_icon_map.cpp.
namespace file_icons {

enum class Style { Overheat, Minimal, None };
extern Style style;  // the "file_icons" setting
Style parse_style(std::string_view s);  // "overheat" | "minimal" | "none"; unknown -> Overheat
const char* style_name(Style s);

enum class Hue : uint8_t { Blue, Indigo, Purple, Pink, Red, Orange, Yellow, Green, Teal, Cyan, Gray, Brown, Accent };

struct FileIcon {
    uint32_t mark;  // icon_glyph codepoint
    Hue hue;
};
struct FolderIcon {
    uint32_t badge = 0, badge_open = 0;  // 0 = a plain folder
    Hue hue = Hue::Accent;
};

// Lookup by file name (not path); case-insensitive. Exact names win over
// extensions (longest first, so "x.d.ts" before "x.ts"), then name prefixes.
FileIcon for_file(std::string_view name);
FolderIcon for_folder(std::string_view name);

// Icon square in pixels for a font (follows font size and zoom).
float size(const FontAtlas& font);
// Horizontal space an icon takes before the text, gap included; 0 when style is None.
float advance(const FontAtlas& font);
// Draws the icon for `name` left-aligned at x, centred on the text line whose baseline is
// `baseline`. Returns the x where the label should start (x itself when style is None).
float draw(Renderer2D& r, const FontAtlas& font, const Theme& th, float x, float baseline,
           std::string_view name, bool is_dir, bool open = false);

} // namespace file_icons
} // namespace oh
