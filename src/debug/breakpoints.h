#pragma once
// Breakpoints as the user set them: source lines (with condition, hit count and
// logpoint message), function breakpoints and exception filters. Lines follow
// edits (shift_for_edit) and the whole set persists per project.
#include "types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oh {

struct SourceBreakpoint {
    fs::path path;            // absolute, lexically normal
    size_t line = 0;          // 0-based, where the user put it
    std::string condition;    // expression; empty = always
    std::string hit_condition;  // e.g. "3" or ">= 5" (adapter syntax)
    std::string log_message;  // non-empty: a logpoint ("x = {x}")
    bool enabled = true;
    // Adapter state for the running session.
    int64_t adapter_id = -1;
    bool verified = false;
    long actual_line = -1;    // 0-based line the adapter bound it to (-1: unknown)
    std::string message;      // why it is unverified
    bool is_logpoint() const { return !log_message.empty(); }
    bool is_conditional() const { return !condition.empty() || !hit_condition.empty(); }
    // The line to draw it on: the adapter's line while bound, else the user's.
    size_t shown_line() const { return actual_line >= 0 ? static_cast<size_t>(actual_line) : line; }
};

struct FunctionBreakpoint {
    std::string name;
    std::string condition;
    bool enabled = true;
    int64_t adapter_id = -1;
    bool verified = false;
};

// One text edit in line/column terms (0-based), as TextBuffer::Change reports it.
struct LineEdit {
    size_t start_line, start_col;
    size_t old_end_line, old_end_col;
    size_t new_end_line, new_end_col;
};

// Where a breakpoint on `line` belongs after `edit`. Text inserted at the very start
// of the line pushes it down; lines removed entirely collapse onto the edit's start.
size_t shift_line_for_edit(size_t line, const LineEdit& e);

class BreakpointStore {
public:
    std::vector<SourceBreakpoint>& source() { return source_; }
    const std::vector<SourceBreakpoint>& source() const { return source_; }
    std::vector<FunctionBreakpoint>& functions() { return functions_; }

    SourceBreakpoint* find(const fs::path& path, size_t line);  // by shown line
    // Adds a plain breakpoint, or removes the one on that line. Returns true when added.
    bool toggle(const fs::path& path, size_t line);
    SourceBreakpoint& ensure(const fs::path& path, size_t line);
    void remove(const fs::path& path, size_t line);
    void remove_all() { source_.clear(); functions_.clear(); bump(); }
    std::vector<SourceBreakpoint*> in_file(const fs::path& path);
    std::vector<fs::path> files() const;

    // Moves the breakpoints of `path` for one edit. Returns true if any moved.
    bool shift_for_edit(const fs::path& path, const LineEdit& e);
    void clear_adapter_state();  // session ended

    // Exception filters the user turned on, by filter id ("rust_panic" is ours).
    std::vector<std::string> exception_filters;
    bool filter_on(const std::string& id) const;
    void set_filter(const std::string& id, bool on);

    // Bumped on every change; the UI rebuilds derived data when it moves.
    uint64_t generation() const { return generation_; }
    void bump() { generation_++; }

    // Plain text persistence (one record per line, tab-separated fields).
    std::string serialize() const;
    void deserialize(const std::string& text);

private:
    std::vector<SourceBreakpoint> source_;
    std::vector<FunctionBreakpoint> functions_;
    uint64_t generation_ = 1;
};

fs::path normalize_path(const fs::path& p);

} // namespace oh
