#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

struct FontFamily {
    std::string name;
    std::string file;  // the Regular (or closest) face
    bool bundled = false;  // ships with Overheat (assets/fonts)
};

// Folders that hold the fonts shipped with Overheat (exe_dir/fonts for the
// build tree and Windows, ../share/overheat/fonts for Linux installs).
std::vector<fs::path> bundled_font_dirs();

// Installed monospace families plus the bundled ones, sorted by name.
std::vector<FontFamily> list_monospace_families();
// Regular face file for a family name, or "" if not installed.
std::string font_file_for_family(const std::string& family);

} // namespace oh
