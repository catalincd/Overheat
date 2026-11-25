#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

struct FontFamily {
    std::string name;
    std::string file;  // the Regular (or closest) face
};

// Installed monospace families via fontconfig, sorted by name.
std::vector<FontFamily> list_monospace_families();
// Regular face file for a family name, or "" if not installed.
std::string font_file_for_family(const std::string& family);

} // namespace oh
