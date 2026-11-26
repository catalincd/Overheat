// Indentation engine tests: each snippet is written with its expected indentation.
// The test strips (or scrambles) the leading whitespace, asks the engine for every
// line's indent and compares it with the expected one, both with the lexical rules
// alone and with the tree-sitter brace classifier. Lines marked with a trailing
// "//KEEP" must be left alone (inside strings / free-form comments).
// Exits non-zero on the first failing snippet set.
#include "editor/buffer.h"
#include "editor/indent.h"
#include "editor/syntax.h"
#include <cstdio>
#include <sstream>

namespace oh { void request_redraw() {} }  // syntax.cpp's async parse wakes the UI
using namespace oh;

static int failures = 0;

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string l; std::getline(ss, l);) out.push_back(l);
    return out;
}

// Runs one snippet. `scramble` replaces the expected indentation with junk so the engine
// cannot be reading it.
static void check(const char* name, const std::string& lang, const std::string& expected, bool use_tree,
                  bool scramble) {
    auto lines = split_lines(expected);
    std::string input;
    std::vector<bool> keep(lines.size(), false);
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        keep[i] = l.size() >= 6 && l.compare(l.size() - 6, 6, "//KEEP") == 0;
        if (keep[i]) {
            input += l;
        } else {
            std::string body = l.substr(leading_ws(l));
            input += (scramble && !body.empty() ? std::string(i % 7, i % 2 ? '\t' : ' ') : std::string()) + body;
        }
        input += '\n';
    }
    TextBuffer buf;
    buf.load_string(input);
    SyntaxHighlighter syn;
    IndentEngine eng;
    eng.set_language(lang);
    if (use_tree) {
        syn.set_language(lang);
        syn.update(buf);
        eng.set_classifier([&](size_t byte) {
            auto [p, g] = syn.brace_context(byte);
            return brace_kind_from_tree(p, g);
        });
    }
    for (size_t i = 0; i < lines.size(); i++) {
        int got = eng.indent(buf, i);
        const std::string& l = lines[i];
        if (keep[i]) {
            if (got != IndentEngine::KEEP) {
                std::fprintf(stderr, "FAIL %s [%s%s] line %zu: expected KEEP, got %d\n  |%s\n", name, lang.c_str(),
                             use_tree ? ", tree" : "", i + 1, got, l.c_str());
                failures++;
            }
            continue;
        }
        if (leading_ws(l) == l.size()) continue;  // blank: any indent is fine
        int want = static_cast<int>(leading_ws_cols(l));
        if (got != want) {
            std::fprintf(stderr, "FAIL %s [%s%s] line %zu: expected %d, got %d\n  |%s\n", name, lang.c_str(),
                         use_tree ? ", tree" : "", i + 1, want, got, l.c_str());
            failures++;
        }
    }
}

static void check_all(const char* name, const std::string& lang, const std::string& expected) {
    for (bool tree : {false, true})
        for (bool scramble : {false, true}) check(name, lang, expected, tree, scramble);
}

static const char* C_BASIC = R"(#include <stdio.h>
#define MAX(a, b) \
    ((a) > (b) ? (a) : (b))

/*
 * Block comment lines align under the star.
 */
static int table[] = {
    1, 2,
    3,
};

enum color {
    RED,
    GREEN,
};

struct point {
    int x;
    int y;
};

int sum(int a,
        int b)
{
    int total = a +
        b;
    if (total > 10)
        total = 10;
    else
        total--;
    for (int i = 0; i < 3; i++) {
        total += i;
    }
    switch (total) {
        case 1:
            printf("one\n");
            break;
        case 2: {
            int z = 3;
            break;
        }
        default:
            break;
    }
#ifdef DEBUG
    printf("%d\n",
           total);
#endif
    do {
        total--;
    } while (total > 0);
    return total;
}

int call(void) {
    return sum(
        1,
        2);
}
)";

static const char* CPP_BASIC = R"(#pragma once
#include <vector>

namespace demo {

template <typename T>
class Box : public Base {
public:
    explicit Box(T v)
        : value_(v),
        other_(0) {}

    T get() const { return value_; }

    int compute(int a, int b) {
        auto f = [&](int x) {
            return x * a;
        };
        std::vector<int> v = {
            1,
            2,
        };
        return f(a) +
            f(b);
    }

private:
    T value_;
    int other_;
};

[[nodiscard]]
int answer() {
    const char* raw = R"x(
line one
  line two)x";
    return 42;
}

} // namespace demo
)";

// Raw string bodies are KEEP lines; mark them so the test knows.
static std::string mark_keep(std::string s, const std::vector<std::string>& lines) {
    for (auto& l : lines) {
        auto p = s.find(l + "\n");
        if (p != std::string::npos) s.insert(p + l.size(), "//KEEP");
    }
    return s;
}

static const char* RUST_BASIC = R"(use std::collections::{
    HashMap,
    HashSet,
};

#[derive(Debug, Clone)]
pub struct Point {
    x: i32,
    y: i32,
}

impl Point {
    pub fn new(x: i32, y: i32) -> Self {
        Point { x, y }
    }

    fn describe<T>(&self, t: T) -> String
    where
        T: std::fmt::Display,
        T: Clone,
    {
        let v = vec![
            1,
            2,
        ];
        let label = match self.x {
            0 => "origin",
            1 | 2 => {
                "near"
            }
            _ =>
                "far",
        };
        let total = v
            .iter()
            .sum::<i32>();
        if total > 0 {
            format!("{} {}", label, t)
        } else {
            String::new()
        }
    }
}

fn call(
    a: i32,
    b: i32,
) -> i32 {
    let s = "a string
that spans lines";
    a + b
}
)";

static const char* JS_BASIC = R"(function f(a) {
    if (a) {
        return [
            1,
            2,
        ];
    }
}
)";

int main() {
    check_all("c-basic", "c", C_BASIC);
    check_all("cpp-basic", "cpp", mark_keep(CPP_BASIC, {"line one", "  line two)x\";"}));
    check_all("rust-basic", "rust", mark_keep(RUST_BASIC, {"that spans lines\";"}));
    check("js-generic", "javascript", JS_BASIC, false, true);

    // Namespaces indented when the setting asks for it.
    IndentSettings::indent_namespaces = true;
    IndentSettings::changed();
    check_all("cpp-namespace-indent", "cpp", "namespace a {\n    int x;\n    namespace b {\n        int y;\n    }\n}\n");
    IndentSettings::indent_namespaces = false;
    IndentSettings::changed();

    // Two-space levels.
    IndentSettings::width = 2;
    IndentSettings::changed();
    check_all("c-width2", "c", "int f() {\n  if (x) {\n    y();\n  }\n}\n");
    IndentSettings::width = 4;
    IndentSettings::changed();

    // Unsupported languages keep everything.
    {
        TextBuffer buf;
        buf.load_string("def f():\n    return 1\n");
        IndentEngine eng;
        eng.set_language("python");
        if (eng.indent(buf, 1) != IndentEngine::KEEP) { std::fprintf(stderr, "FAIL python not KEEP\n"); failures++; }
    }

    // Incremental: after an edit near the end only the tail is rescanned.
    {
        std::string big;
        for (int i = 0; i < 2000; i++) big += "void f" + std::to_string(i) + "() {\n    int x;\n}\n";
        TextBuffer buf;
        buf.load_string(big);
        IndentEngine eng;
        eng.set_language("c");
        for (size_t l = 0; l < buf.line_count(); l++) eng.indent(buf, l);
        size_t before = eng.lines_scanned();
        size_t edit_line = buf.line_count() - 10;
        buf.insert(buf.line_start(edit_line), "if (y) {\n");
        eng.invalidate_from(edit_line);
        int got = eng.indent(buf, edit_line + 1);  // the line after the new "if (y) {"
        size_t rescanned = eng.lines_scanned() - before;
        if (rescanned > 80) { std::fprintf(stderr, "FAIL incremental: rescanned %zu lines\n", rescanned); failures++; }
        if (got != 8 && got != 4) { std::fprintf(stderr, "FAIL incremental: indent %d\n", got); failures++; }
        // A query at the top needs no rescan at all.
        size_t b2 = eng.lines_scanned();
        eng.indent(buf, 5);
        if (eng.lines_scanned() != b2) { std::fprintf(stderr, "FAIL cache: top line rescanned\n"); failures++; }
    }

    // indent_string honours tabs.
    IndentSettings::use_tabs = true;
    if (indent_string(10) != "\t\t  ") { std::fprintf(stderr, "FAIL indent_string tabs\n"); failures++; }
    IndentSettings::use_tabs = false;
    if (indent_string(6) != "      ") { std::fprintf(stderr, "FAIL indent_string spaces\n"); failures++; }

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("indent_test: all passed\n");
    return failures ? 1 : 0;
}
