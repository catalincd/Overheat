#include "fonts.h"
#include <algorithm>
#include <set>
#ifdef _WIN32
#include "platform/platform.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <map>
#include <mutex>
#else
#include <fontconfig/fontconfig.h>
#endif

namespace oh {

#ifdef _WIN32
// No fontconfig on Windows: scan the system and per-user font folders with
// FreeType once, keeping fixed-pitch faces (Regular preferred per family).
namespace {
const std::map<std::string, std::string>& windows_mono_fonts() {
    static std::map<std::string, std::string> families;
    static std::once_flag once;
    std::call_once(once, [] {
        FT_Library ft;
        if (FT_Init_FreeType(&ft)) return;
        std::string windir = platform::getenv_str("WINDIR");
        std::vector<fs::path> dirs = {
            fs::path(windir.empty() ? "C:\\Windows" : windir) / "Fonts",
            fs::path(platform::getenv_str("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts",
            platform::exe_dir() / "fonts",
        };
        std::map<std::string, int> score;  // 2 = Regular, 1 = other upright
        for (const auto& dir : dirs) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".ttf" && ext != ".otf") continue;
                FT_Face face;
                std::string file = e.path().string();
                if (FT_New_Face(ft, file.c_str(), 0, &face)) continue;
                if (FT_IS_FIXED_WIDTH(face) && face->family_name && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
                    std::string style = face->style_name ? face->style_name : "";
                    int s = (style == "Regular" || style == "Book" || style == "Normal") ? 2 : 1;
                    if (face->style_flags & FT_STYLE_FLAG_BOLD) s = 0;
                    std::string fam = face->family_name;
                    if (!score.count(fam) || s > score[fam]) {
                        score[fam] = s;
                        families[fam] = file;
                    }
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
    return it == f.end() ? "" : it->second;
}

std::vector<FontFamily> list_monospace_families() {
    std::vector<FontFamily> out;
    for (const auto& [name, file] : windows_mono_fonts())
        if (name.find("Symbols") == std::string::npos) out.push_back({name, file});
    return out;
}

#else

std::string font_file_for_family(const std::string& family) {
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
        if (!file.empty()) out.push_back({n, file});
    }
    return out;
}

#endif

} // namespace oh
