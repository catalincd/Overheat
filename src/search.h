#pragma once
#include "types.h"
#include <atomic>
#include <string>
#include <vector>

namespace oh {

struct SearchHit {
    fs::path file;
    size_t line = 0;   // 0-based
    size_t col = 0;    // byte column of the match
    size_t len = 0;
    std::string text;  // the whole line (trimmed to a sane length)
};

struct SearchQuery {
    std::string text;
    bool case_sensitive = false;
    bool whole_word = false;
};

// Plain-text search over files on disk; skips binaries and very large files.
std::vector<SearchHit> search_files(const std::vector<fs::path>& files, const SearchQuery& q,
                                    const std::atomic<bool>& cancel, size_t max_hits = 20000);

// Finds matches of q inside one line (used for local and remote results).
std::vector<std::pair<size_t, size_t>> find_in_line(const std::string& line, const SearchQuery& q);

} // namespace oh
