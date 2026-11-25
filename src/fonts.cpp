#include "fonts.h"
#include <algorithm>
#include <fontconfig/fontconfig.h>
#include <set>

namespace oh {

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

} // namespace oh
