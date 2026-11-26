// Large / binary file handling: the open verdict (filecheck::classify), the
// Vfs size + head probe on a sparse 1 GB file, UTF-8 boundaries on invalid
// bytes, and byte-exact round trips through the piece table.
// Usage: file_check_test SCRATCH_DIR [extra files, e.g. /usr/bin/*]
#include "editor/buffer.h"
#include "editor/file_check.h"
#include "editor/utf8.h"
#include "vfs.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>

using namespace oh;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                               \
        }                                                                             \
    } while (0)

// Every boundary walk forward and back must agree, make progress and decode
// to a code point; invalid bytes are characters of one byte.
static void check_boundaries(const std::string& s) {
    std::vector<size_t> fwd;
    for (size_t i = 0; i < s.size(); i = utf8::next_boundary(s, i)) {
        fwd.push_back(i);
        size_t n = utf8::next_boundary(s, i);
        CHECK(n > i && n - i <= 4);
        char32_t cp = utf8::decode(s, i);
        CHECK(cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF));
        if (n - i > 1) CHECK(cp >= 0x80);  // (a real U+FFFD in the file decodes to itself)
    }
    size_t k = fwd.size();
    for (size_t i = s.size(); i > 0;) {
        i = utf8::prev_boundary(s, i);
        if (k == 0) { CHECK(false); break; }
        CHECK(fwd[--k] == i);
        if (fwd[k] != i) break;
    }
    CHECK(k == 0);
    // display_col / byte_col_at stay inverse at every boundary.
    if (s.size() < 4096 && s.find('\t') == std::string::npos) {
        for (size_t i : fwd) CHECK(utf8::byte_col_at(s, utf8::display_col(s, i)) == i);
    }
}

// Edits at character boundaries keep every other byte exactly as it was.
static void check_roundtrip(const std::string& s, std::mt19937& rng) {
    TextBuffer buf;
    buf.load_string(s);
    CHECK(buf.text() == s);
    std::string expect = s;
    buf.begin_undo_group();
    for (int n = 0; n < 20 && !s.empty(); n++) {
        size_t at = utf8::next_boundary(expect, rng() % expect.size());
        buf.insert(at, "X\xC3\xA9");
        expect.insert(at, "X\xC3\xA9");
    }
    buf.end_undo_group();
    CHECK(buf.text() == expect);
    buf.undo();
    CHECK(buf.text() == s);
    buf.set_read_only(true);
    buf.insert(0, "nope");
    buf.erase(0, std::min<size_t>(3, s.size()));
    CHECK(buf.text() == s);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s SCRATCH_DIR [files...]\n", argv[0]);
        return 2;
    }
    using filecheck::Verdict;
    // Verdicts
    CHECK(filecheck::classify(10, "hello\n") == Verdict::Open);
    CHECK(filecheck::classify(10, std::string("ab\0cd", 5)) == Verdict::Binary);
    std::string late(9000, 'a');
    late[8500] = '\0';  // NUL past the 8 KiB probe: text, as in search
    CHECK(filecheck::classify(late.size(), late) == Verdict::Open);
    CHECK(filecheck::classify(filecheck::ASK_SIZE + 1, "text") == Verdict::AskLarge);
    CHECK(filecheck::classify(filecheck::HARD_CAP + 1, "text") == Verdict::TooLarge);
    CHECK(filecheck::classify(filecheck::HARD_CAP + 1, std::string(4, '\0')) == Verdict::TooLarge);
    CHECK(filecheck::large_mode(filecheck::LARGE_MODE + 1) && !filecheck::large_mode(1 << 20));
    CHECK(filecheck::human_size(1ull << 30) == "1.0 GB");

    // Sparse 1 GB file: sized and sniffed without reading it.
    fs::path dir = argv[1];
    fs::create_directories(dir);
    fs::path big = dir / "sparse_1g.bin";
    { std::ofstream(big, std::ios::binary); }
    fs::resize_file(big, 1ull << 30);
    LocalVfs vfs;
    uint64_t size = 0;
    std::string head;
    CHECK(vfs.file_size(big, size) && size == (1ull << 30));
    CHECK(vfs.read_head(big, filecheck::BINARY_PROBE, head) && head.size() == filecheck::BINARY_PROBE);
    CHECK(filecheck::classify(size, head) == Verdict::TooLarge);
    fs::remove(big);
    CHECK(!vfs.file_size(big, size));

    // Hand-picked invalid UTF-8
    const char* cases[] = {
        "caf\xE9 na\xEFve", "\xFF\xFE", "\x80\x80\x80", "\xC3\xA9\x80", "\xE2\x82", "\xE2\x82\xAC\x80",
        "\xC0\xAF overlong", "\xED\xA0\x80 surrogate", "\xF4\x90\x80\x80 > 10FFFF", "\xF0\x9F\x94\xA5 fire",
        "a\xF0\x9F\x94", "\xF8\x88\x80\x80\x80",
    };
    for (const char* c : cases) check_boundaries(c);
    CHECK(utf8::next_boundary(std::string("\xC3\xA9"), 0) == 2);
    CHECK(utf8::next_boundary(std::string("\xE2\x82"), 0) == 1);       // truncated: byte by byte
    CHECK(utf8::prev_boundary(std::string("\xE2\x82\xAC\x80"), 4) == 3);  // stray continuation
    CHECK(utf8::decode(std::string("\xE2\x82\xAC"), 0) == 0x20AC);
    CHECK(utf8::decode(std::string("\xC0\xAF"), 0) == 0xFFFD);

    std::mt19937 rng(1234);
    for (int t = 0; t < 200; t++) {
        std::string s(rng() % 300, '\0');
        for (auto& ch : s) ch = static_cast<char>(rng());
        check_boundaries(s);
        check_roundtrip(s, rng);
    }

    // Real binaries (CTest passes a few from /usr/bin).
    for (int i = 2; i < argc; i++) {
        std::ifstream in(argv[i], std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        if (data.size() > (4u << 20)) data.resize(4u << 20);
        CHECK(filecheck::looks_binary(data) || data.find('\0') == std::string::npos ||
              data.find('\0') >= filecheck::BINARY_PROBE);
        check_boundaries(data);
        check_roundtrip(data, rng);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("file_check_test: ok\n");
    return 0;
}
