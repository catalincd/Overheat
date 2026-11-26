#include "search.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>

namespace oh {

namespace {
bool is_word(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }
bool is_cont(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

void ascii_lower(std::string& s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Reads a file for searching; false for unreadable files and binaries (NUL in the first 8 KiB).
bool read_text_file(const fs::path& f, std::string& data) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    constexpr size_t PROBE = 8192;
    data.resize(PROBE);
    in.read(data.data(), static_cast<std::streamsize>(PROBE));
    data.resize(static_cast<size_t>(in.gcount()));
    if (data.find('\0') != std::string::npos) return false;  // binary
    if (data.size() == PROBE) data.append(std::istreambuf_iterator<char>(in), {});
    return true;
}
} // namespace

std::vector<std::pair<size_t, size_t>> find_in_line(std::string_view line, const SearchQuery& q) {
    std::vector<std::pair<size_t, size_t>> out;
    if (q.text.empty()) return out;
    auto eq = [&](char a, char b) {
        return q.case_sensitive ? a == b
                                : std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    auto it = line.begin();
    while (true) {
        it = std::search(it, line.end(), q.text.begin(), q.text.end(), eq);
        if (it == line.end()) break;
        size_t pos = static_cast<size_t>(it - line.begin());
        size_t end = pos + q.text.size();
        bool ok = !q.whole_word ||
                  ((pos == 0 || !is_word(static_cast<unsigned char>(line[pos - 1]))) &&
                   (end >= line.size() || !is_word(static_cast<unsigned char>(line[end]))));
        if (ok) out.push_back({pos, q.text.size()});
        it += static_cast<std::ptrdiff_t>(std::max<size_t>(1, q.text.size()));
    }
    return out;
}

SearchHit make_search_hit(const fs::path& file, size_t line_no, std::string_view line, size_t col, size_t len) {
    col = std::min(col, line.size());
    len = std::min(len, line.size() - col);
    // Drop indentation, keep a little context before the match and a bounded tail after it.
    size_t lead = line.find_first_not_of(" \t");
    if (lead == std::string_view::npos || lead > col) lead = col;
    size_t start = col > lead + SEARCH_PREVIEW_BEFORE ? col - SEARCH_PREVIEW_BEFORE : lead;
    while (start < col && is_cont(line[start])) start++;
    size_t end = std::max(col + len, std::min(line.size(), start + SEARCH_PREVIEW_MAX));
    while (end > col + len && end < line.size() && is_cont(line[end])) end--;
    SearchHit h;
    h.file = file;
    h.line = line_no;
    h.col = col;
    h.len = len;
    h.text.assign(line.substr(start, end - start));
    for (char& c : h.text)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    h.preview_col = col - start;
    return h;
}

SearchStats search_files(const std::vector<fs::path>& files, const SearchQuery& q,
                         const std::atomic<bool>& cancel, const SearchSink& sink, size_t max_hits) {
    using Clock = std::chrono::steady_clock;
    SearchStats st;
    if (q.text.empty()) return st;
    std::string needle = q.text;
    if (!q.case_sensitive) ascii_lower(needle);

    std::vector<SearchHit> batch;
    auto last_flush = Clock::now();
    auto flush = [&] {
        if (!batch.empty() && sink) sink(std::move(batch));
        batch.clear();
        last_flush = Clock::now();
    };

    std::string data, lowered;
    for (const fs::path& f : files) {
        if (cancel.load(std::memory_order_relaxed) || st.truncated) break;
        std::error_code ec;
        auto size = fs::file_size(f, ec);
        if (ec) continue;
        if (size > SEARCH_MAX_FILE_SIZE) { st.skipped_large++; continue; }
        if (!read_text_file(f, data)) { st.skipped_binary++; continue; }
        st.files_searched++;
        const std::string* hay = &data;
        if (!q.case_sensitive) {
            lowered = data;
            ascii_lower(lowered);
            hay = &lowered;
        }
        // Scan the whole buffer and only work out line numbers for actual matches.
        size_t line_no = 0, line_start = 0, line_end = 0, scanned = 0;
        bool have_line = false;
        for (size_t pos = hay->find(needle); pos != std::string::npos; pos = hay->find(needle, pos + needle.size())) {
            if (cancel.load(std::memory_order_relaxed)) break;
            if (!have_line || pos > line_end) {
                size_t nl = static_cast<size_t>(std::count(data.begin() + static_cast<std::ptrdiff_t>(scanned),
                                                           data.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
                if (nl) {
                    line_no += nl;
                    line_start = data.rfind('\n', pos - 1) + 1;
                }
                line_end = data.find('\n', pos);
                if (line_end == std::string::npos) line_end = data.size();
                scanned = pos;
                have_line = true;
            }
            size_t end = pos + needle.size();
            if (end > line_end) continue;  // cannot happen for single-line queries
            if (q.whole_word && ((pos > line_start && is_word(static_cast<unsigned char>(data[pos - 1]))) ||
                                 (end < line_end && is_word(static_cast<unsigned char>(data[end])))))
                continue;
            if (st.hits >= max_hits) {
                st.truncated = true;
                break;
            }
            size_t visible_end = line_end > line_start && data[line_end - 1] == '\r' ? line_end - 1 : line_end;
            std::string_view line(data.data() + line_start, std::max(visible_end, end) - line_start);
            batch.push_back(make_search_hit(f, line_no, line, pos - line_start, needle.size()));
            st.hits++;
        }
        if (batch.size() >= 512 || Clock::now() - last_flush > std::chrono::milliseconds(50)) flush();
    }
    flush();
    return st;
}

} // namespace oh
