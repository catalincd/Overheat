// Randomized check of the whole-buffer file search against a naive line-by-line
// reference, plus the limits: result cap, binary/large file skipping, bounded
// UTF-8-safe previews and cancellation. Exits non-zero on the first failure.
#include "search.h"
#include <cstdio>
#include <fstream>
#include <random>

using namespace oh;

static int failures = 0;
#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, __VA_ARGS__);                                   \
            std::fprintf(stderr, "\n");                                          \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void write(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

static std::vector<SearchHit> run(const std::vector<fs::path>& files, const SearchQuery& q, SearchStats* st = nullptr,
                                  size_t max = SEARCH_MAX_RESULTS) {
    std::atomic<bool> cancel{false};
    std::vector<SearchHit> out;
    SearchStats s = search_files(files, q, cancel, [&](std::vector<SearchHit>&& b) {
        for (auto& h : b) out.push_back(std::move(h));
    }, max);
    if (st) *st = s;
    return out;
}

static void check_preview(const SearchHit& h) {
    CHECK(h.text.size() <= SEARCH_PREVIEW_MAX + h.len, "preview too long: %zu", h.text.size());
    CHECK(h.preview_col + h.len <= h.text.size(), "match outside preview");
    if (!h.text.empty())
        CHECK((static_cast<unsigned char>(h.text[0]) & 0xC0) != 0x80, "preview starts mid UTF-8 sequence");
}

int main(int argc, char** argv) {
    unsigned seed = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1;
    std::mt19937 rng(seed);
    fs::path dir = fs::temp_directory_path() / ("oh_search_test_" + std::to_string(seed) + "_" + std::to_string(rng()));
    fs::create_directories(dir);

    // 1. Random files vs. the line-by-line reference (find_in_line on every line).
    const char* alphabet = "aAb\n\r_ \xCF\x89\t";
    std::vector<fs::path> files;
    for (int i = 0; i < 40; i++) {
        std::string s;
        size_t n = rng() % 3000;
        for (size_t k = 0; k < n; k++) s += alphabet[rng() % 10];
        fs::path p = dir / ("f" + std::to_string(i));
        write(p, s);
        files.push_back(p);
    }
    for (const char* text : {"a", "ab", "Ab", "a_a", "\xCF\x89", "b a"}) {
        for (int flags = 0; flags < 4; flags++) {
            SearchQuery q{text, (flags & 1) != 0, (flags & 2) != 0};
            auto got = run(files, q);
            std::vector<SearchHit> want;
            for (const auto& f : files) {
                std::ifstream in(f, std::ios::binary);
                std::string line;
                size_t ln = 0;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    for (auto [c, l] : find_in_line(line, q)) want.push_back({f, ln, c, l, {}, 0});
                    ln++;
                }
            }
            CHECK(got.size() == want.size(), "query '%s' flags %d: %zu hits, want %zu", text, flags, got.size(),
                  want.size());
            for (size_t i = 0; i < std::min(got.size(), want.size()); i++) {
                const auto &g = got[i], &w = want[i];
                if (g.file != w.file || g.line != w.line || g.col != w.col || g.len != w.len) {
                    CHECK(false, "query '%s' flags %d: hit %zu at %s:%zu:%zu, want %s:%zu:%zu", text, flags, i,
                          g.file.c_str(), g.line, g.col, w.file.c_str(), w.line, w.col);
                    break;
                }
                check_preview(g);
            }
        }
    }

    // 2. Limits: cap + truncation flag, binary and large files skipped, long lines clipped.
    std::string many;
    for (int i = 0; i < 100; i++) many += "foo foo\n";
    write(dir / "many", many);
    SearchStats st;
    auto hits = run({dir / "many"}, {"foo"}, &st, 150);
    CHECK(hits.size() == 150 && st.truncated, "cap: %zu hits, truncated=%d", hits.size(), st.truncated);
    hits = run({dir / "many"}, {"foo"}, &st, 200);
    CHECK(hits.size() == 200 && !st.truncated, "exact cap: %zu hits, truncated=%d", hits.size(), st.truncated);

    write(dir / "bin", std::string("foo\0foo", 7));
    write(dir / "large", std::string(SEARCH_MAX_FILE_SIZE + 1, 'x') + "foo");
    hits = run({dir / "bin", dir / "large"}, {"foo"}, &st);
    CHECK(hits.empty() && st.skipped_binary == 1 && st.skipped_large == 1, "binary/large not skipped");

    std::string longline;
    for (int i = 0; i < 300; i++) longline += std::string(997, 'x') + "\xCF\x89" + "foo";
    write(dir / "minified", longline + "\n");
    hits = run({dir / "minified"}, {"foo"});
    CHECK(hits.size() == 300, "minified: %zu hits", hits.size());
    for (const auto& h : hits) {
        check_preview(h);
        CHECK(h.text.substr(h.preview_col, h.len) == "foo", "preview does not show the match");
    }
    CHECK(!hits.empty() && hits.back().col == longline.size() - 3, "column in the full line");

    // 3. Cancellation stops the scan.
    std::atomic<bool> cancel{true};
    st = search_files({dir / "many"}, {"foo"}, cancel, {});
    CHECK(st.hits == 0, "cancelled search found %zu", st.hits);

    fs::remove_all(dir);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("search_test %u: ok\n", seed);
    return failures ? 1 : 0;
}
