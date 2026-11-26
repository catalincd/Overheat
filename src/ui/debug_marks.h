#pragma once
// What the debugger draws in an editor: breakpoint dots in the gutter lane, the
// stopped line (arrow + highlight) and inline variable values at line ends.
// Built by the App's debug glue per file; drawn by EditorView (debug_editor.cpp).
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oh {

struct DebugMarks {
    struct Breakpoint {
        size_t line;
        enum Kind : uint8_t { Plain, Conditional, Log } kind = Plain;
        bool enabled = true;
        bool verified = true;  // false while a session runs and the adapter could not bind it
    };
    std::vector<Breakpoint> breakpoints;
    long exec_line = -1;     // the selected frame's line in this file (0-based)
    bool exec_top = true;    // the top frame (yellow) vs. a caller frame (green)
    std::vector<std::pair<size_t, std::string>> inline_values;  // line -> "a = 1, b = 2"
};

} // namespace oh
