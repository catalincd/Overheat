#pragma once
// File locations in terminal / task output, for Ctrl+click:
//   path:line[:col]          gcc, clang, grep -n, most tools
//   path(line[,col])         MSVC
//   --> path:line:col        rustc (also "::: path:line:col")
//   File "path", line N      Python tracebacks
#include <cstddef>
#include <string>
#include <vector>

namespace oh {

struct TextLink {
    size_t begin = 0, end = 0;  // byte range in the line (the underlined part)
    std::string path;
    size_t line = 0, col = 0;   // 1-based as printed; 0 = not given
};

// Every location in `line`, left to right.
std::vector<TextLink> find_links(const std::string& line);
// The location covering byte offset `at`, if any.
bool link_at(const std::string& line, size_t at, TextLink& out);

} // namespace oh
