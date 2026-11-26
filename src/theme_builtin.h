#pragma once
#include <vector>

namespace oh {

// TOML sources of the built-in themes modelled on popular VS Code themes
// (same format as user theme files). Parsed by ThemeManager::init.
const std::vector<const char*>& builtin_theme_sources();

} // namespace oh
