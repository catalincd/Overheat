#pragma once
// Problems found in build output (problem matchers) and the quick fixes some
// compilers suggest. Lines and columns are 0-based. Columns count characters
// (code points), as compilers report them; the App converts them to byte columns
// against the file's text.
#include <cstddef>
#include <string>
#include <vector>

namespace oh {

struct ProblemNote {
    std::string file;  // empty: no location
    size_t line = 0, col = 0;
    std::string message;
};

struct ProblemEdit {
    std::string file;
    size_t line = 0, col = 0, end_line = 0, end_col = 0;
    std::string text;
};

// A machine-applicable suggestion (rustc/clippy JSON): applied by Ctrl+. like an
// LSP code action.
struct ProblemFix {
    std::string title;
    std::vector<ProblemEdit> edits;
};

struct Problem {
    std::string file;  // as printed: relative to the task's cwd, or absolute; "" = no location
    size_t line = 0, col = 0;
    size_t end_line = 0, end_col = 0;  // == start when the tool gives a point only
    int severity = 1;                  // 1 error, 2 warning, 3 info / note
    std::string message;
    std::string code;    // E0308, C2065, -Wunused-variable, clippy::len_zero, ...
    std::string source;  // gcc, msvc, cmake, rustc, generic
    std::vector<ProblemNote> notes;  // include chain, instantiation context, notes
    std::vector<ProblemFix> fixes;
};

} // namespace oh
