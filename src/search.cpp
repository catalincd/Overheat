#include "search.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace oh {

namespace {
bool is_word(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }
}

std::vector<std::pair<size_t, size_t>> find_in_line(const std::string& line, const SearchQuery& q) {
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

std::vector<SearchHit> search_files(const std::vector<fs::path>& files, const SearchQuery& q,
                                    const std::atomic<bool>& cancel, size_t max_hits) {
    std::vector<SearchHit> hits;
    if (q.text.empty()) return hits;
    constexpr std::uintmax_t MAX_FILE = 8u << 20;
    for (const fs::path& f : files) {
        if (cancel.load(std::memory_order_relaxed) || hits.size() >= max_hits) break;
        std::error_code ec;
        if (fs::file_size(f, ec) > MAX_FILE || ec) continue;
        std::ifstream in(f, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        if (data.find('\0', 0) < std::min<size_t>(data.size(), 8192)) continue;  // binary
        size_t line_no = 0, start = 0;
        while (start <= data.size() && hits.size() < max_hits) {
            size_t nl = data.find('\n', start);
            size_t end = nl == std::string::npos ? data.size() : nl;
            std::string line = data.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            for (auto [col, len] : find_in_line(line, q)) {
                hits.push_back({f, line_no, col, len, line.size() > 400 ? line.substr(0, 400) : line});
                if (hits.size() >= max_hits) break;
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
            line_no++;
        }
    }
    return hits;
}

} // namespace oh
