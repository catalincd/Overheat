// 4coder-style cursor motion: boundary seeks over plain strings (every stop of a sweep
// across a line), blank-line hops, the multi-cursor wrappers over a real TextBuffer,
// and a randomized check that seeks are monotonic and never split a UTF-8 sequence.
#include "editor/buffer.h"
#include "editor/cursor.h"
#include "editor/motion.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace oh;
using motion::Boundary;

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

// Sweeps from one end to the other and marks every stop with '|'.
static std::string sweep(const std::string& s, Boundary b, int dir) {
    std::vector<bool> stop(s.size() + 1, false);
    size_t pos = dir > 0 ? 0 : s.size();
    for (int guard = 0; guard < 1000; guard++) {
        size_t next = dir > 0 ? motion::seek_right(s, pos, b) : motion::seek_left(s, pos, b);
        if (next == pos) break;
        pos = next;
        stop[pos] = true;
    }
    std::string out;
    for (size_t i = 0; i <= s.size(); i++) {
        if (stop[i]) out += '|';
        if (i < s.size()) out += s[i];
    }
    return out;
}

static void expect_sweep(const std::string& s, Boundary b, int dir, const std::string& want) {
    std::string got = sweep(s, b, dir);
    CHECK(got == want, "sweep %s of \"%s\": got \"%s\", want \"%s\"", dir > 0 ? "right" : "left", s.c_str(),
          got.c_str(), want.c_str());
}

static void test_seeks() {
    const std::string code = "  auto x = foo.bar(baz_qux, 42);";
    // Ctrl in plain text / Ctrl+Alt: whitespace-separated tokens, ends going right, starts going left.
    expect_sweep(code, Boundary::Whitespace, 1, "  auto| x| =| foo.bar(baz_qux,| 42);|");
    expect_sweep(code, Boundary::Whitespace, -1, "|  |auto |x |= |foo.bar(baz_qux, |42);");
    // Ctrl in code: alphanumeric runs ('_' included, like 4coder).
    expect_sweep(code, Boundary::AlphaNumeric, 1, "  auto| x| = foo|.bar|(baz_qux|, 42|);|");
    expect_sweep(code, Boundary::AlphaNumeric, -1, "|  |auto |x = |foo.|bar(|baz_qux, |42);");

    // Alt: camelCase humps, acronyms, snake_case parts, digits stay with letters.
    expect_sweep("parseHTTPServer", Boundary::Subword, 1, "parse|HTTP|Server|");
    expect_sweep("parseHTTPServer", Boundary::Subword, -1, "|parse|HTTP|Server");
    expect_sweep("snake_case_name", Boundary::Subword, 1, "snake|_case|_name|");
    expect_sweep("snake_case_name", Boundary::Subword, -1, "|snake_|case_|name");
    expect_sweep("utf8Decode(x)", Boundary::Subword, 1, "utf8|Decode|(x|)|");
    expect_sweep("MAX_LEN", Boundary::Subword, 1, "MAX|_LEN|");
    expect_sweep("XMLHttpRequest", Boundary::Subword, 1, "XML|Http|Request|");
    expect_sweep("XMLHttpRequest", Boundary::Subword, -1, "|XML|Http|Request");
    expect_sweep("A", Boundary::Subword, 1, "A|");
    expect_sweep("_private", Boundary::Subword, 1, "_private|");

    // Line breaks are whitespace: the seeks cross them.
    expect_sweep("a.b\n  c", Boundary::Whitespace, 1, "a.b|\n  c|");
    expect_sweep("a.b\n  c", Boundary::Whitespace, -1, "|a.b\n  |c");

    // UTF-8: multibyte letters are word characters and are never split.
    expect_sweep("héllo wörld", Boundary::AlphaNumeric, 1, "héllo| wörld|");
    expect_sweep("€ x", Boundary::Subword, 1, "€| x|");

    // Ends and empty input.
    CHECK(motion::seek_right("", 0, Boundary::Whitespace) == 0, "empty right");
    CHECK(motion::seek_left("", 0, Boundary::Whitespace) == 0, "empty left");
    CHECK(motion::seek_right("abc  ", 3, Boundary::AlphaNumeric) == 5, "trailing space runs to the end");
    CHECK(motion::seek_left("  abc", 2, Boundary::AlphaNumeric) == 0, "leading space runs to the start");
}

static void test_blank_lines() {
    //                                0     1      2   3     4    5   6   7
    std::vector<std::string> lines = {"a", "b", "", "c", " \t", "", "d", "e"};
    auto blank = [&](size_t i) { return lines[i].find_first_not_of(" \t") == std::string::npos; };
    size_t n = lines.size();
    CHECK(motion::blank_line_hop(blank, n, 0, 1) == 2, "down from paragraph start");
    CHECK(motion::blank_line_hop(blank, n, 2, 1) == 4, "down from a blank line skips the next paragraph");
    CHECK(motion::blank_line_hop(blank, n, 4, 1) == n, "down past the last paragraph: none");
    CHECK(motion::blank_line_hop(blank, n, 7, -1) == 5, "up from the end");
    CHECK(motion::blank_line_hop(blank, n, 5, -1) == 2, "up skips adjacent blanks and a paragraph");
    CHECK(motion::blank_line_hop(blank, n, 2, -1) == SIZE_MAX, "up past the first paragraph: none");
    CHECK(motion::blank_line_hop(blank, n, 0, -1) == SIZE_MAX, "up from line 0");
}

static std::string sel_str(const CursorSet& c) {
    std::string out;
    for (auto& s : c.selections())
        out += "[" + std::to_string(s.anchor.line) + ":" + std::to_string(s.anchor.col) + "-" +
               std::to_string(s.head.line) + ":" + std::to_string(s.head.col) + "]";
    return out;
}

static void test_cursor_set() {
    TextBuffer buf;
    buf.load_string("int fooBar = 1;\nint bazQux = 2;\n\nreturn x;\n");
    CursorSet c;
    c.set_primary({0, 4});
    c.add_cursor({1, 4});
    c.move_boundary(buf, 1, Boundary::Subword, false);
    CHECK(sel_str(c) == "[0:7-0:7][1:7-1:7]", "two cursors by subword: %s", sel_str(c).c_str());
    c.move_boundary(buf, 1, Boundary::Subword, true);
    CHECK(sel_str(c) == "[0:7-0:10][1:7-1:10]", "shift extends: %s", sel_str(c).c_str());
    c.move_boundary(buf, -1, Boundary::Whitespace, false);
    CHECK(sel_str(c) == "[0:4-0:4][1:4-1:4]", "token left: %s", sel_str(c).c_str());

    c.set_primary({0, 3});
    c.move_blank_line(buf, 1, false);
    CHECK(sel_str(c) == "[2:0-2:0]", "blank line down: %s", sel_str(c).c_str());
    c.move_blank_line(buf, 1, true);
    CHECK(sel_str(c) == "[2:0-4:0]", "blank line down selecting, to the trailing empty line: %s", sel_str(c).c_str());
    c.move_blank_line(buf, -1, false);
    CHECK(sel_str(c) == "[2:0-2:0]", "blank line up: %s", sel_str(c).c_str());
    c.move_blank_line(buf, -1, false);
    CHECK(sel_str(c) == "[0:0-0:0]", "blank line up to the top: %s", sel_str(c).c_str());

    // Seeking across the line break from the end of line 0.
    c.set_primary({0, 15});
    c.move_boundary(buf, 1, Boundary::AlphaNumeric, false);
    CHECK(sel_str(c) == "[1:3-1:3]", "crosses the newline: %s", sel_str(c).c_str());
}

static void test_random(uint32_t seed) {
    std::mt19937 rng(seed);
    const char* pieces[] = {"a", "Z", "_", " ", "\n", ".", "(", "9", "é", "€", "\t", "xY", "AB"};
    for (int iter = 0; iter < 2000; iter++) {
        std::string s;
        int n = static_cast<int>(rng() % 30);
        for (int i = 0; i < n; i++) s += pieces[rng() % (sizeof(pieces) / sizeof(*pieces))];
        size_t pos = rng() % (s.size() + 1);
        while (pos > 0 && pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) pos--;
        for (Boundary b : {Boundary::Whitespace, Boundary::AlphaNumeric, Boundary::Subword}) {
            size_t r = motion::seek_right(s, pos, b), l = motion::seek_left(s, pos, b);
            CHECK(r >= pos && r <= s.size(), "right out of range");
            CHECK(l <= pos, "left out of range");
            CHECK(r == s.size() || r == pos || (static_cast<unsigned char>(s[r]) & 0xC0) != 0x80, "right splits UTF-8");
            CHECK(l == 0 || (static_cast<unsigned char>(s[l]) & 0xC0) != 0x80, "left splits UTF-8");
            // Progress unless already at the end of everything seekable.
            if (r == pos) CHECK(s.find_first_not_of(" \t\n", pos) == std::string::npos || b != Boundary::Whitespace,
                                "whitespace right stalled");
        }
    }
}

int main(int argc, char** argv) {
    uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 1;
    test_seeks();
    test_blank_lines();
    test_cursor_set();
    test_random(seed);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("motion_test: ok\n");
    return 0;
}
