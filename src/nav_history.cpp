#include "nav_history.h"
#include <algorithm>

namespace oh {

bool NavLocation::close_to(const NavLocation& o) const {
    size_t d = pos.line > o.pos.line ? pos.line - o.pos.line : o.pos.line - pos.line;
    return path == o.path && d <= 3;
}

void NavHistory::record(const NavLocation& from) {
    forward_.clear();
    if (!back_.empty() && back_.back().close_to(from)) {
        back_.back() = from;  // refine instead of stacking duplicates
        return;
    }
    back_.push_back(from);
    if (back_.size() > MAX_ENTRIES) back_.pop_front();
}

std::optional<NavLocation> NavHistory::back(const NavLocation& current) {
    while (!back_.empty() && back_.back().close_to(current)) back_.pop_back();  // already here
    if (back_.empty()) return std::nullopt;
    NavLocation to = back_.back();
    back_.pop_back();
    if (forward_.empty() || !forward_.back().close_to(current)) forward_.push_back(current);
    return to;
}

std::optional<NavLocation> NavHistory::forward(const NavLocation& current) {
    while (!forward_.empty() && forward_.back().close_to(current)) forward_.pop_back();
    if (forward_.empty()) return std::nullopt;
    NavLocation to = forward_.back();
    forward_.pop_back();
    if (back_.empty() || !back_.back().close_to(current)) back_.push_back(current);
    return to;
}

void NavHistory::forget(const fs::path& path) {
    auto gone = [&](const NavLocation& l) { return l.path == path; };
    back_.erase(std::remove_if(back_.begin(), back_.end(), gone), back_.end());
    forward_.erase(std::remove_if(forward_.begin(), forward_.end(), gone), forward_.end());
}

} // namespace oh
