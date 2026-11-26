#include "fonts.h"
#include "platform/platform.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#ifdef _WIN32
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#include <fontconfig/fontconfig.h>
#endif

namespace oh {

std::vector<fs::path> bundled_font_dirs() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::path d : {platform::exe_dir() / "fonts", platform::exe_dir() / ".." / "share" / "overheat" / "fonts"}) {
        if (!fs::is_directory(d, ec)) continue;
        d = fs::weakly_canonical(d, ec);
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    }
    return out;
}

#ifdef _WIN32
// No fontconfig on Windows: scan the system and per-user font folders with
// FreeType once, keeping fixed-pitch faces (Regular preferred per family).
// Bundled fonts are scanned too and win over a system copy of the same family.
namespace {
struct WinFont {
    std::string file;
    int score = -1;  // 2 = Regular, 1 = other upright, 0 = bold
    bool bundled = false;
};

const std::map<std::string, WinFont>& windows_mono_fonts() {
    static std::map<std::string, WinFont> families;
    static std::once_flag once;
    std::call_once(once, [] {
        FT_Library ft;
        if (FT_Init_FreeType(&ft)) return;
        std::string windir = platform::getenv_str("WINDIR");
        std::vector<std::pair<fs::path, bool>> dirs = {
            {fs::path(windir.empty() ? "C:\\Windows" : windir) / "Fonts", false},
            {fs::path(platform::getenv_str("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts", false},
        };
        for (const fs::path& d : bundled_font_dirs()) dirs.push_back({d, true});
        for (const auto& [dir, bundled] : dirs) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".ttf" && ext != ".otf") continue;
                FT_Face face;
                std::string file = e.path().string();
                if (FT_New_Face(ft, file.c_str(), 0, &face)) continue;
                // Bundled fonts are all coding fonts; some (Iosevka Term) are not
                // flagged fixed-pitch because of their double-width glyphs.
                if ((bundled || FT_IS_FIXED_WIDTH(face)) && face->family_name &&
                    !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
                    std::string style = face->style_name ? face->style_name : "";
                    int s = (style == "Regular" || style == "Book" || style == "Normal") ? 2 : 1;
                    if (face->style_flags & FT_STYLE_FLAG_BOLD) s = 0;
                    WinFont& cur = families[face->family_name];
                    if ((bundled && !cur.bundled) || (bundled == cur.bundled && s > cur.score))
                        cur = {file, s, bundled};
                }
                FT_Done_Face(face);
            }
        }
        FT_Done_FreeType(ft);
    });
    return families;
}
} // namespace

std::string font_file_for_family(const std::string& family) {
    const auto& f = windows_mono_fonts();
    auto it = f.find(family);
    return it == f.end() ? "" : it->second.file;
}

std::vector<FontFamily> list_monospace_families() {
    std::vector<FontFamily> out;
    for (const auto& [name, f] : windows_mono_fonts())
        if (name.find("Symbols") == std::string::npos) out.push_back({name, f.file, f.bundled});
    return out;
}

#else

namespace {
// Registers the bundled font folders with fontconfig (once, before any query)
// and returns family -> Regular (or closest upright) bundled file.
const std::map<std::string, std::string>& bundled_families() {
    static std::map<std::string, std::string> families;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const fs::path& d : bundled_font_dirs())
            FcConfigAppFontAddDir(nullptr, reinterpret_cast<const FcChar8*>(d.string().c_str()));
        FcFontSet* set = FcConfigGetFonts(nullptr, FcSetApplication);
        std::map<std::string, int> best;  // distance from upright Regular
        for (int i = 0; set && i < set->nfont; i++) {
            FcChar8* fam = nullptr;
            FcChar8* file = nullptr;
            int weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
            if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch ||
                FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
                continue;
            FcPatternGetInteger(set->fonts[i], FC_WEIGHT, 0, &weight);
            FcPatternGetInteger(set->fonts[i], FC_SLANT, 0, &slant);
            int dist = std::abs(weight - FC_WEIGHT_REGULAR) + (slant != FC_SLANT_ROMAN ? 1000 : 0);
            std::string name = reinterpret_cast<const char*>(fam);
            if (!best.count(name) || dist < best[name]) {
                best[name] = dist;
                families[name] = reinterpret_cast<const char*>(file);
            }
        }
    });
    return families;
}
} // namespace

std::string font_file_for_family(const std::string& family) {
    // The bundled copy wins over a system one: it is the version we tested.
    const auto& bundled = bundled_families();
    if (auto it = bundled.find(family); it != bundled.end()) return it->second;
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    std::string file;
    FcChar8* matched_family = nullptr;
    FcChar8* path = nullptr;
    // fontconfig always returns *something*; only accept it if the family really matches.
    if (match && FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
        family == reinterpret_cast<const char*>(matched_family) &&
        FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch)
        file = reinterpret_cast<const char*>(path);
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return file;
}

std::vector<FontFamily> list_monospace_families() {
    const auto& bundled = bundled_families();
    std::vector<FontFamily> out;
    FcPattern* pat = FcPatternCreate();
    FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, nullptr);
    FcFontSet* set = FcFontList(nullptr, pat, os);
    std::set<std::string> names;
    for (int i = 0; set && i < set->nfont; i++) {
        FcChar8* fam = nullptr;
        // First family name only (others are localized aliases or style-specific names).
        if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) == FcResultMatch)
            names.insert(reinterpret_cast<const char*>(fam));
    }
    if (set) FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    // Bundled fonts are all coding fonts, even those fontconfig calls "dual"
    // spacing because of double-width glyphs (Iosevka Term).
    for (const auto& [name, file] : bundled) names.insert(name);
    for (const std::string& n : names) {
        // "X Nerd Font" has double-width icons; keep only its grid-friendly "Mono" sibling.
        if (n.size() > 10 && n.compare(n.size() - 10, 10, "Nerd Font ") != 0 &&
            n.find(" Nerd Font") == n.size() - 10 && names.count(n + " Mono"))
            continue;
        // Skip symbol-only and proportional "Propo" variants.
        if (n.find("Symbols") != std::string::npos || n.find("Propo") != std::string::npos ||
            n.find("Emoji") != std::string::npos)
            continue;
        std::string file = font_file_for_family(n);
        if (!file.empty()) out.push_back({n, file, bundled.count(n) > 0});
    }
    return out;
}

#endif

} // namespace oh
