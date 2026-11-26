// Pure parts of LSP navigation: the hover markdown parser, the JSON DOM, and
// WorkspaceEdit parsing / application (rename across files). Exits non-zero on
// the first failure batch.
#include "lsp/json.h"
#include "lsp/workspace_edit.h"
#include "ui/markdown.h"
#include "nav_history.h"
#include <cstdio>

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

static std::string spans_str(const std::vector<md::Span>& spans) {
    std::string s;
    for (auto& sp : spans) {
        const char* tag = sp.style == md::Style::Code ? "c" : sp.style == md::Style::Bold ? "b"
                        : sp.style == md::Style::Italic ? "i" : sp.style == md::Style::Link ? "l" : "n";
        s += std::string("[") + tag + ":" + sp.text + "]";
    }
    return s;
}

static void test_inline() {
    auto s = spans_str(md::parse_inline("Returns `int` for **all** *x* values"));
    CHECK(s == "[n:Returns ][c:int][n: for ][b:all][n: ][i:x][n: values]", "%s", s.c_str());
    s = spans_str(md::parse_inline("snake_case_name stays _plain_ text"));
    CHECK(s == "[n:snake_case_name stays ][i:plain][n: text]", "%s", s.c_str());
    s = spans_str(md::parse_inline("escaped \\*not italic\\* and \\_x\\_"));
    CHECK(s == "[n:escaped *not italic* and _x_]", "%s", s.c_str());
    s = spans_str(md::parse_inline("see [the docs](https://x.y/z) or <https://a.b>"));
    CHECK(s == "[n:see ][l:the docs][n: or ][l:https://a.b]", "%s", s.c_str());
    s = spans_str(md::parse_inline("``a ` b`` and a * lone star and `unclosed"));
    CHECK(s == "[c:a ` b][n: and a * lone star and `unclosed]", "%s", s.c_str());
    s = spans_str(md::parse_inline("x &lt; y &amp;&amp; **bold `code`**"));
    CHECK(s == "[n:x < y && ][b:bold ][c:code]", "%s", s.c_str());
    s = spans_str(md::parse_inline("a*b*c and 2 * 3 * 4"));
    CHECK(s == "[n:a][i:b][n:c and 2 * 3 * 4]", "%s", s.c_str());
}

static void test_blocks() {
    // clangd-style hover.
    std::string clangd =
        "### function `clamp_int`  \n\n---\n\xe2\x86\x92 `int`  \nParameters:  \n- `int value`\n- `int lo`\n\n"
        "---\n```cpp\nint clamp_int(int value, int lo, int hi)\n```";
    auto b = md::parse(clangd);
    CHECK(b.size() == 7, "clangd blocks: %zu", b.size());
    if (b.size() == 7) {
        CHECK(b[0].kind == md::Block::Heading && b[0].level == 3, "heading");
        CHECK(spans_str(b[0].spans) == "[n:function ][c:clamp_int]", "%s", spans_str(b[0].spans).c_str());
        CHECK(b[1].kind == md::Block::Rule, "rule");
        CHECK(b[2].kind == md::Block::Paragraph, "para");
        CHECK(spans_str(b[2].spans) == "[n:\xe2\x86\x92 ][c:int][n:\n][n:Parameters:]", "%s", spans_str(b[2].spans).c_str());
        CHECK(b[3].kind == md::Block::ListItem && b[3].marker == "\xe2\x80\xa2", "list item");
        CHECK(spans_str(b[4].spans) == "[c:int lo]", "%s", spans_str(b[4].spans).c_str());
        CHECK(b[5].kind == md::Block::Rule, "rule 2");
        CHECK(b[6].kind == md::Block::Code && b[6].lang == "cpp" && b[6].lines.size() == 1, "code");
    }
    // rust-analyzer-style hover.
    std::string ra = "\n```rust\nzoo::animal\n```\n\n```rust\npub trait Animal\n```\n\n---\n\nSomething that lives\nin the zoo.";
    b = md::parse(ra);
    CHECK(b.size() == 4, "ra blocks: %zu", b.size());
    if (b.size() == 4) {
        CHECK(b[0].kind == md::Block::Code && b[0].lines[0] == "zoo::animal", "ra code 1");
        CHECK(b[1].kind == md::Block::Code && b[1].lines[0] == "pub trait Animal", "ra code 2");
        CHECK(b[2].kind == md::Block::Rule, "ra rule");
        CHECK(spans_str(b[3].spans) == "[n:Something that lives in the zoo.]", "%s", spans_str(b[3].spans).c_str());
    }
    // Nested and ordered lists, continuation lines, unterminated fence.
    b = md::parse("1. one\n   more\n   - nested\n2. two\n\n```\nopen fence");
    CHECK(b.size() == 4, "list blocks: %zu", b.size());
    if (b.size() == 4) {
        CHECK(b[0].marker == "1." && b[0].level == 0 && spans_str(b[0].spans) == "[n:one more]", "%s",
              spans_str(b[0].spans).c_str());
        CHECK(b[1].marker == "\xe2\x80\xa2" && b[1].level == 1, "nested level %d", b[1].level);
        CHECK(b[2].marker == "2." && b[2].level == 0, "second item level %d", b[2].level);
        CHECK(b[3].kind == md::Block::Code && b[3].lines.size() == 1 && b[3].lines[0] == "open fence", "fence");
    }
    CHECK(md::parse("").empty(), "empty");
    CHECK(md::parse("***").size() == 1 && md::parse("***")[0].kind == md::Block::Rule, "*** rule");
}

static void test_json() {
    bool ok = false;
    auto v = lsp_json::parse(R"({"a":[1,2.5,-3e2],"s":"x\"\u00e9\ud83d\ude00","o":{"t":true,"n":null}})", &ok);
    CHECK(ok, "parse ok");
    CHECK(v["a"].size() == 3 && v["a"][2].as_int() == -300, "numbers");
    CHECK(v["s"].str() == "x\"\xc3\xa9\xf0\x9f\x98\x80", "unicode %s", v["s"].str().c_str());
    CHECK(v["o"]["t"].as_bool() && v["o"]["n"].is_null() && v["missing"]["deep"].is_null(), "lookups");
    lsp_json::parse("{\"a\":", &ok);
    CHECK(!ok, "truncated input fails");
    CHECK(lsp_json::dump(lsp_json::parse(R"({"id":"x-1","n":[1,true]})")) == R"({"id":"x-1","n":[1,true]})", "dump");
}

static LSPTextEdit edit(size_t l0, size_t c0, size_t l1, size_t c1, std::string t) {
    LSPTextEdit e;
    e.start = {l0, c0};
    e.end = {l1, c1};
    e.new_text = std::move(t);
    return e;
}

static void test_edits() {
    std::string text = "int foo = 1;\nfoo += foo;\r\nreturn foo;";
    std::vector<LSPTextEdit> edits = {edit(1, 7, 1, 10, "bar"), edit(0, 4, 0, 7, "bar"), edit(1, 0, 1, 3, "bar"),
                                      edit(2, 7, 2, 10, "bar")};
    std::string t = text;
    CHECK(apply_text_edits(t, edits, false), "apply");
    CHECK(t == "int bar = 1;\nbar += bar;\r\nreturn bar;", "%s", t.c_str());
    // Overlap is rejected and leaves the text alone.
    t = text;
    std::string err;
    CHECK(!apply_text_edits(t, {edit(0, 0, 0, 5, "x"), edit(0, 3, 0, 6, "y")}, false, &err) && t == text, "overlap");
    CHECK(err == "overlapping edits", "error message %s", err.c_str());
    // UTF-16 columns: an astral character counts as two units, é as one.
    std::string u = "a\xf0\x9f\x98\x80" "b\xc3\xa9" "c foo";
    CHECK(apply_text_edits(u, {edit(0, 7, 0, 10, "bar")}, true), "utf16 apply");
    CHECK(u == "a\xf0\x9f\x98\x80" "b\xc3\xa9" "c bar", "utf16 %s", u.c_str());
    // Past the end clamps; inserts at one point keep their order; an empty edit list is a no-op.
    std::string e = "ab\ncd";
    CHECK(apply_text_edits(e, {edit(1, 99, 1, 99, "X"), edit(5, 0, 5, 0, "Y"), edit(0, 1, 0, 1, "1"), edit(0, 1, 0, 1, "2")}, false), "clamp");
    CHECK(e == "a12b\ncdXY", "%s", e.c_str());
    // Offsets map through edits (cursor preservation in open buffers).
    std::vector<ByteEdit> be;
    CHECK(resolve_text_edits(text, edits, false, be), "resolve");
    CHECK(map_offset_through(0, be) == 0, "before");
    CHECK(map_offset_through(text.size(), be) == text.size(), "same length rename keeps size");
    std::vector<ByteEdit> grow{{2, 1, "xyz"}};
    CHECK(map_offset_through(5, grow) == 7 && map_offset_through(3, grow) == 5 && map_offset_through(2, grow) == 2, "grow");
}

static void test_workspace_edit() {
    auto v = lsp_json::parse(R"({"changes":{
        "file:///tmp/a%20b/x.cpp":[{"range":{"start":{"line":0,"character":1},"end":{"line":0,"character":2}},"newText":"Q"}],
        "file:///tmp/y.h":[]}})");
    auto we = parse_workspace_edit(v);
    CHECK(we.files.size() == 2, "files %zu", we.files.size());
    if (we.files.size() == 2) {
        CHECK(we.files[0].path == fs::path("/tmp/a b/x.cpp"), "percent-decoded %s", we.files[0].path.c_str());
        CHECK(we.files[0].edits.size() == 1 && we.files[0].edits[0].new_text == "Q", "edit");
    }
    v = lsp_json::parse(R"({"documentChanges":[
        {"textDocument":{"uri":"file:///r/main.rs","version":3},"edits":[
            {"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":8}},"newText":"pet"}]},
        {"kind":"rename","oldUri":"file:///r/a.rs","newUri":"file:///r/b.rs"},
        {"textDocument":{"uri":"file:///r/main.rs","version":3},"edits":[
            {"range":{"start":{"line":5,"character":0},"end":{"line":5,"character":4}},"newText":"pet"}]}]})");
    we = parse_workspace_edit(v);
    CHECK(we.files.size() == 1 && we.edit_count() == 2 && we.skipped_ops == 1, "documentChanges %zu %zu %d",
          we.files.size(), we.edit_count(), we.skipped_ops);
}

static void test_history() {
    NavHistory h;
    auto at = [](const char* f, size_t line) { return NavLocation{fs::path(f), {line, 0}}; };
    CHECK(!h.back(at("a", 0)), "empty back");
    h.record(at("a", 10));  // a:10 -> b:5
    h.record(at("b", 5));   // b:5 -> c:1
    h.record(at("c", 2));   // near c:1 -> c:50 ... (a jump within c)
    auto t = h.back(at("c", 50));
    CHECK(t && t->path == "c" && t->pos.line == 2, "back 1");
    t = h.back(*t);
    CHECK(t && t->path == "b", "back 2");
    t = h.forward(*t);
    CHECK(t && t->path == "c" && t->pos.line == 2, "forward 1 %s", t ? t->path.c_str() : "-");
    t = h.forward(*t);
    CHECK(t && t->path == "c" && t->pos.line == 50, "forward 2");
    CHECK(!h.forward(*t), "forward end");
    // A new jump clears the forward list; duplicates of the top are merged.
    h.back(at("c", 50));
    h.record(at("x", 1));
    h.record(at("x", 2));
    CHECK(!h.can_forward() && h.back_size() == 3, "record clears forward (%zu)", h.back_size());
    h.forget("x");
    CHECK(h.back_size() == 2, "forget");
    for (size_t i = 0; i < 200; i++) h.record(at("f", i * 10));
    CHECK(h.back_size() == NavHistory::MAX_ENTRIES, "bounded");
}

int main() {
    test_history();
    test_inline();
    test_blocks();
    test_json();
    test_edits();
    test_workspace_edit();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("nav_test: ok\n");
    return 0;
}
