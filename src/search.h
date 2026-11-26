#pragma once
#include "types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oh {

struct SearchHit {
    fs::path file;
    size_t line = 0;   // 0-based
    size_t col = 0;    // byte column of the match in the full line (for opening the file)
    size_t len = 0;
    std::string text;  // preview: a bounded slice of the line around the match
    size_t preview_col = 0;  // byte offset of the match inside text
};

struct SearchQuery {
    std::string text;
    bool case_sensitive = false;
    bool whole_word = false;
};

// Same limits as VS Code: results are capped and the user is asked to refine the query.
constexpr size_t SEARCH_MAX_RESULTS = 20000;
constexpr std::uintmax_t SEARCH_MAX_FILE_SIZE = 8u << 20;
constexpr size_t SEARCH_PREVIEW_BEFORE = 48;   // bytes of context kept before a match
constexpr size_t SEARCH_PREVIEW_MAX = 256;     // total preview bytes per result

struct SearchStats {
    size_t hits = 0;
    size_t files_searched = 0;
    size_t skipped_binary = 0;
    size_t skipped_large = 0;
    bool truncated = false;  // more matches existed beyond max_hits
};

// Hits are handed out in batches as they are found (called on the searching thread).
using SearchSink = std::function<void(std::vector<SearchHit>&&)>;

// Plain-text search over files on disk; skips binaries and very large files.
SearchStats search_files(const std::vector<fs::path>& files, const SearchQuery& q,
                         const std::atomic<bool>& cancel, const SearchSink& sink,
                         size_t max_hits = SEARCH_MAX_RESULTS);

// Finds matches of q inside one line (used for local and remote results).
std::vector<std::pair<size_t, size_t>> find_in_line(std::string_view line, const SearchQuery& q);

// Builds a hit whose preview is clipped to a bounded, UTF-8-safe window around the match.
SearchHit make_search_hit(const fs::path& file, size_t line_no, std::string_view line, size_t col, size_t len);

// Results shared between a search worker and the UI thread, which drains them each frame.
struct SearchStream {
    std::mutex mutex;
    std::vector<SearchHit> pending;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    SearchStats stats;  // valid once done

    void push(std::vector<SearchHit>&& batch) {
        std::lock_guard lock(mutex);
        if (pending.empty()) pending = std::move(batch);
        else pending.insert(pending.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    }
    std::vector<SearchHit> take() {
        std::lock_guard lock(mutex);
        return std::exchange(pending, {});
    }
};

} // namespace oh
