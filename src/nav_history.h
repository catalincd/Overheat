#pragma once
// Back/forward navigation history (VS Code's Go Back / Go Forward). Entries are
// the places the cursor jumped *from*: definition, references and symbol picks,
// search hits, tab switches and big cursor moves.
#include "types.h"
#include <deque>
#include <optional>

namespace oh {

struct NavLocation {
    fs::path path;
    Position pos{};
    // Same place for history purposes: same file and within a few lines.
    bool close_to(const NavLocation& o) const;
};

class NavHistory {
public:
    static constexpr size_t MAX_ENTRIES = 64;

    // A jump is leaving `from`: remember it and drop the forward list.
    void record(const NavLocation& from);
    // Returns where to go, remembering `current` on the other stack.
    std::optional<NavLocation> back(const NavLocation& current);
    std::optional<NavLocation> forward(const NavLocation& current);
    bool can_back() const { return !back_.empty(); }
    bool can_forward() const { return !forward_.empty(); }
    size_t back_size() const { return back_.size(); }
    size_t forward_size() const { return forward_.size(); }
    // A file was closed or renamed away: forget its entries.
    void forget(const fs::path& path);
    void clear() { back_.clear(), forward_.clear(); }

private:
    std::deque<NavLocation> back_, forward_;
};

} // namespace oh
