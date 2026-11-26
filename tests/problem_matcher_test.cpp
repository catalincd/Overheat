// Problem matchers, terminal links and task files, checked against real compiler
// output captured from tests/projects (tests/projects/capture.sh) plus a few
// hand-written MSVC / edge-case lines. Usage: problem_matcher_test <fixtures-dir>
#include "tasks/json.h"
#include "tasks/links.h"
#include "tasks/matchers.h"
#include "tasks/tasks.h"
#include <cstdio>
#include <fstream>
#include <sstream>

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

static fs::path fixtures;

static std::string slurp(const std::string& name) {
    std::ifstream in(fixtures / name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::vector<Problem> run(const std::vector<std::string>& matchers, const std::string& text) {
    ProblemCollector c(matchers);
    c.feed(text);
    c.finish();
    return c.problems();
}

static const Problem* find(const std::vector<Problem>& ps, const std::string& file_suffix, size_t line) {
    for (auto& p : ps)
        if (p.line == line && p.file.size() >= file_suffix.size() &&
            p.file.compare(p.file.size() - file_suffix.size(), file_suffix.size(), file_suffix) == 0)
            return &p;
    return nullptr;
}

static void dump(const std::vector<Problem>& ps) {
    for (auto& p : ps) {
        std::fprintf(stderr, "  %s:%zu:%zu sev=%d code=%s msg=%s\n", p.file.c_str(), p.line + 1, p.col + 1, p.severity,
                     p.code.c_str(), p.message.c_str());
        for (auto& n : p.notes)
            std::fprintf(stderr, "     note %s:%zu:%zu %s\n", n.file.c_str(), n.line + 1, n.col + 1, n.message.c_str());
        for (auto& f : p.fixes) std::fprintf(stderr, "     fix %s (%zu edits)\n", f.title.c_str(), f.edits.size());
    }
}

static void test_gcc_c() {
    auto ps = run({"gcc"}, slurp("gcc_c.txt"));
    CHECK(ps.size() == 5, "gcc C: %zu problems", ps.size());
    const Problem* p = find(ps, "broken.c", 7);
    CHECK(p && p->severity == 1 && p->col == 23 && p->code == "-Wint-conversion", "int-conversion error");
    // The note in util.h (behind an "In file included from") attaches to that error.
    CHECK(p && p->notes.size() == 1 && p->notes[0].file == "util.h" && p->notes[0].line == 3, "note attached");
    p = find(ps, "broken.c", 9);
    CHECK(p && p->severity == 1 && p->message.find("expected") == 0, "missing semicolon");
    p = find(ps, "broken.c", 6);
    CHECK(p && p->severity == 2 && p->code == "-Wunused-variable", "unused variable warning");
    p = find(ps, "broken.c", 3);
    CHECK(p && p->severity == 2, "unused function warning");
    if (failures) dump(ps);
}

static void test_clang_c() {
    auto ps = run({"gcc"}, slurp("clang_c.txt"));
    CHECK(ps.size() == 3, "clang C: %zu problems", ps.size());
    const Problem* p = find(ps, "broken.c", 7);
    CHECK(p && p->notes.size() == 1 && p->notes[0].file == "./util.h", "clang note attached");
    CHECK(find(ps, "broken.c", 8) && find(ps, "broken.c", 8)->code == "-Wimplicit-function-declaration", "implicit decl");
}

static void test_gcc_cpp() {
    auto ps = run({"gcc", "msvc", "cmake"}, slurp("gcc_cpp.txt"));
    CHECK(ps.size() == 3, "gcc C++: %zu problems", ps.size());
    const Problem* p = find(ps, "src/shapes.h", 14);
    CHECK(p && p->severity == 1 && p->col == 43, "template error in the header");
    // Include chain, instantiation context and "required from here" come first in gcc.
    bool included = false, instantiation = false, required = false;
    if (p)
        for (auto& n : p->notes) {
            included |= n.message.find("In file included from") == 0;
            instantiation |= n.message.find("In instantiation of") == 0;
            required |= n.message.find("required from here") == 0 && n.line == 10 && n.col == 25;
        }
    CHECK(included && instantiation && required, "gcc instantiation notes (%d %d %d)", included, instantiation, required);
    CHECK(find(ps, "src/broken.cpp", 11) && find(ps, "src/broken.cpp", 11)->severity == 1, "undeclared name");
    CHECK(find(ps, "src/broken.cpp", 8) && find(ps, "src/broken.cpp", 8)->severity == 2, "unused warning");
    if (failures) dump(ps);
}

static void test_clang_cpp() {
    auto ps = run({"gcc"}, slurp("clang_cpp.txt"));
    CHECK(ps.size() == 2, "clang C++: %zu problems", ps.size());
    const Problem* p = find(ps, "src/shapes.h", 14);
    // The include chain printed before the error is kept with it; the instantiation
    // note printed after it attaches to it too.
    CHECK(p && p->notes.size() == 2 && p->notes[0].message.find("In file included from") == 0 &&
              p->notes[1].message.find("in instantiation of") == 0 && p->notes[1].line == 10,
          "clang instantiation note attached to the parent");
    if (failures) dump(ps);
}

static void test_cmake() {
    auto ps = run({"cmake"}, slurp("cmake_error.txt"));
    CHECK(ps.size() == 1, "cmake: %zu problems", ps.size());
    if (!ps.empty()) {
        CHECK(ps[0].file == "CMakeLists.txt" && ps[0].line == 14 && ps[0].severity == 1, "cmake location");
        CHECK(ps[0].code == "no_such_command" && ps[0].message.find("Unknown CMake command") == 0, "cmake message: %s",
              ps[0].message.c_str());
    }
    auto w = run({"cmake"}, "CMake Warning (dev) at sub/CMakeLists.txt:3 (project):\n  Policy CMP0048 is not set.\n"
                            "This warning is for project developers.\n-- Done\n"
                            "CMake Error: The source directory \"/nope\" does not exist.\n");
    CHECK(w.size() == 2 && w[0].severity == 2 && w[0].file == "sub/CMakeLists.txt" && w[0].line == 2, "cmake warning");
    CHECK(w.size() == 2 && w[1].file.empty() && w[1].message.find("The source directory") == 0, "cmake plain error");
}

static void test_rustc_human() {
    auto ps = run({"rustc"}, slurp("cargo_human.txt"));
    CHECK(ps.size() == 4, "rustc human: %zu problems", ps.size());
    const Problem* p = find(ps, "src/broken.rs", 4);
    CHECK(p && p->code == "E0425" && p->col == 11 && p->severity == 1, "E0425");
    p = find(ps, "src/main.rs", 14);
    CHECK(p && p->severity == 2 && p->col == 8, "unused mut warning");
    auto panic = run({"rustc"}, "thread 'main' panicked at src/main.rs:5:9:\nattempt to divide by zero\nnote: run with "
                                "`RUST_BACKTRACE=1`\n");
    CHECK(panic.size() == 1 && panic[0].line == 4 && panic[0].message == "panicked: attempt to divide by zero",
          "panic location");
    if (failures) dump(ps);
}

static void test_cargo_json() {
    auto ps = run({"cargo-json", "rustc"}, slurp("cargo_json.txt"));
    CHECK(ps.size() == 4, "cargo json: %zu problems", ps.size());
    const Problem* p = find(ps, "src/broken.rs", 3);
    CHECK(p && p->code == "E0308" && p->col == 20 && p->end_col == 31, "E0308 span");
    CHECK(p && p->notes.size() == 1 && p->notes[0].message == "expected due to this", "secondary label");
    p = find(ps, "src/broken.rs", 10);
    CHECK(p && p->fixes.size() == 1 && p->fixes[0].edits.size() == 1 && p->fixes[0].edits[0].text == ".clone()",
          "clone suggestion");
    p = find(ps, "src/main.rs", 14);
    CHECK(p && p->fixes.size() == 1 && p->fixes[0].edits[0].text.empty() && p->fixes[0].edits[0].col == 8 &&
              p->fixes[0].edits[0].end_col == 12,
          "remove mut fix");
    // Display: rendered diagnostics replace the JSON, artifacts vanish.
    std::string display;
    std::istringstream in(slurp("cargo_json.txt"));
    int shown = 0, hidden = 0;
    for (std::string line; std::getline(in, line);) {
        CHECK(ProblemCollector::cargo_json_line(line, display, nullptr), "json line");
        (display.empty() ? hidden : shown)++;
    }
    CHECK(shown >= 4 && hidden >= 1, "display %d/%d", shown, hidden);
    if (failures) dump(ps);
}

static void test_clippy_json() {
    auto ps = run({"cargo-json"}, slurp("clippy_json.txt"));
    CHECK(ps.size() == 4, "clippy: %zu problems", ps.size());
    const Problem* p = find(ps, "src/main.rs", 5);
    CHECK(p && p->code == "clippy::len_zero" && p->severity == 2, "len_zero");
    CHECK(p && p->fixes.size() == 1 && p->fixes[0].edits[0].text == "names.is_empty()", "len_zero fix");
    p = find(ps, "src/main.rs", 10);
    CHECK(p && p->fixes.size() == 1 && p->fixes[0].edits.size() == 2, "needless_return: two-edit fix");
    if (failures) dump(ps);
}

static void test_msvc() {
    auto ps = run({"msvc"},
                  "C:\\src\\app\\main.cpp(12,5): error C2065: 'x': undeclared identifier\r\n"
                  "  main.cpp(20): warning C4101: 'y': unreferenced local variable [C:\\src\\app\\app.vcxproj]\n"
                  "C:\\src\\app\\util.h(3): note: see declaration of 'foo'\n"
                  "3>C:\\src\\lib.cpp(7,1,7,9): fatal error C1083: Cannot open include file: 'nope.h'\n"
                  "Build FAILED.\n");
    CHECK(ps.size() == 3, "msvc: %zu problems", ps.size());
    if (ps.size() == 3) {
        CHECK(ps[0].file == "C:\\src\\app\\main.cpp" && ps[0].line == 11 && ps[0].col == 4 && ps[0].code == "C2065",
              "msvc error");
        CHECK(ps[1].file == "main.cpp" && ps[1].severity == 2 && ps[1].notes.size() == 1, "msvc warning + note");
        CHECK(ps[2].file == "C:\\src\\lib.cpp" && ps[2].end_col == 8 && ps[2].severity == 1, "msvc range");
    }
}

static void test_generic_and_ansi() {
    std::string colored = "\x1b[01m\x1b[Kmain.c:3:7:\x1b[m\x1b[K \x1b[01;31m\x1b[Kerror: \x1b[m\x1b[Kboom "
                          "[\x1b]8;;https://gcc.gnu.org/x\x07-Wfoo\x1b]8;;\x07]\r";
    auto ps = run({"gcc"}, colored + "\n");
    CHECK(ps.size() == 1 && ps[0].message == "boom" && ps[0].code == "-Wfoo", "ansi + osc8 stripped");
    auto g = run({"generic"}, "lib/x.py:12: warning: old style\nnot a location: 12\nmake: *** [Makefile:3: a] Error 1\n"
                              "src/a.ts:4:2 - missing semicolon\n");
    CHECK(g.size() == 2 && g[0].severity == 2 && g[0].line == 11 && g[1].col == 1, "generic: %zu", g.size());
}

static void test_links() {
    auto l = find_links("src/main.rs:15:9: warning");
    CHECK(l.size() == 1 && l[0].path == "src/main.rs" && l[0].line == 15 && l[0].col == 9 && l[0].begin == 0 &&
              l[0].end == 16,
          "gcc-style link");
    l = find_links("  --> src/broken.rs:5:12");
    CHECK(l.size() == 1 && l[0].path == "src/broken.rs" && l[0].begin == 6, "rustc arrow");
    l = find_links("C:\\src\\main.cpp(12,5): error C2065");
    CHECK(l.size() == 1 && l[0].path == "C:\\src\\main.cpp" && l[0].line == 12 && l[0].col == 5, "msvc link");
    l = find_links("In file included from broken.c:2:");
    CHECK(l.size() == 1 && l[0].path == "broken.c" && l[0].line == 2 && l[0].col == 0, "file:line");
    l = find_links("  File \"/tmp/x.py\", line 3, in <module>");
    CHECK(l.size() == 1 && l[0].path == "/tmp/x.py" && l[0].line == 3, "python");
    l = find_links("see Makefile, version 1.2 and http://host:80/a.c:3");
    bool bogus = false;
    for (auto& k : l) bogus |= k.path == "Makefile" || k.path == "1.2";
    CHECK(!bogus, "no bare words");
    TextLink one;
    CHECK(link_at("x /abs/dir/file.c:7 y", 5, one) && one.path == "/abs/dir/file.c" && one.line == 7, "link_at");
    CHECK(!link_at("x /abs/dir/file.c:7 y", 0, one), "link_at miss");
}

static void test_tasks_toml() {
    std::vector<TaskDef> ts;
    std::string err;
    bool ok = parse_tasks_toml(R"(# comment
[[task]]
label = "configure"
program = "cmake"
args = ["-S", ".",
        "-B", "out"]   # multi-line array
matcher = "cmake"

[[task]]
label = 'build'
command = "cmake --build out"
group = "build"
default = true
depends_on = ["configure"]
env = { CC = "clang", JOBS = 8 }
matcher = ["gcc", "generic"]

[task.env]
EXTRA = "1"
)", ts, &err);
    CHECK(ok, "toml: %s", err.c_str());
    CHECK(ts.size() == 2, "toml tasks: %zu", ts.size());
    if (ts.size() == 2) {
        CHECK(!ts[0].shell && ts[0].command == "cmake" && ts[0].args.size() == 4 && ts[0].matchers.size() == 1, "t0");
        CHECK(ts[1].shell && ts[1].is_default && ts[1].group == "build" && ts[1].depends_on.size() == 1, "t1");
        CHECK(ts[1].env.size() == 1 && ts[1].env[0].first == "EXTRA", "[task.env] replaces the inline table");
    }
    std::vector<TaskDef> bad;
    CHECK(!parse_tasks_toml("[[task]]\nlabel = \"x\"\ncommand = \"unterminated\n", bad, &err), "bad toml");
    CHECK(err.find("line 3") != std::string::npos, "error line: %s", err.c_str());
}

static void test_vscode_tasks() {
    std::vector<TaskDef> ts;
    std::string err;
    bool ok = parse_vscode_tasks(R"({
  // comments and trailing commas are fine
  "version": "2.0.0",
  "options": { "env": { "GLOBAL": "1" } },
  "tasks": [
    {
      "label": "build",
      "type": "shell",
      "command": "make",
      "args": ["-j", {"value": "all", "quoting": "escape"}],
      "options": { "cwd": "${workspaceFolder}/sub", "env": { "CC": "gcc" } },
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": ["$gcc"],
    },
    { "label": "test", "type": "process", "command": "./run_tests", "group": "test",
      "problemMatcher": { "base": "$msCompile", "owner": "cpp" }, "dependsOn": "build",
      "linux": { "command": "./run_tests.sh" }, "windows": { "command": "run_tests.exe" } },
    { "type": "cargo", "command": "clippy", "problemMatcher": "$rustc" },
    { "label": "npm", "type": "npm", "script": "watch" },
  ],
})", ts, &err);
    CHECK(ok, "tasks.json: %s", err.c_str());
    CHECK(ts.size() == 3, "tasks.json tasks: %zu", ts.size());
    if (ts.size() == 3) {
        CHECK(ts[0].shell && ts[0].args.size() == 2 && ts[0].args[1] == "all" && ts[0].is_default, "shell task");
        CHECK(ts[0].env.size() == 2 && ts[0].cwd == "${workspaceFolder}/sub", "env + cwd");
        CHECK(!ts[1].shell && ts[1].group == "test" && ts[1].depends_on == std::vector<std::string>{"build"}, "process");
        CHECK(ts[1].matchers.size() == 1 && ProblemCollector::canonical(ts[1].matchers[0]) == "msvc", "matcher base");
#ifndef _WIN32
        CHECK(ts[1].command == "./run_tests.sh", "os override: %s", ts[1].command.c_str());
#endif
        CHECK(ts[2].command == "cargo" && ts[2].args.size() == 1 && ts[2].label == "cargo clippy", "cargo type");
    }
    TaskContext ctx{"/proj", "/proj/src/a.c", 7};
    CHECK(expand_task_vars("${workspaceFolder}/x ${fileBasenameNoExtension} ${relativeFile}:${lineNumber} ${nope}",
                           ctx) == "/proj/x a src/a.c:7 ${nope}",
          "vars");
    TaskDef t;
    t.shell = true;
    t.command = "echo";
    t.args = {"a b", "c"};
    auto argv = task_argv(t, ctx);
#ifndef _WIN32
    CHECK(argv.size() == 3 && argv[2] == "echo 'a b' c", "shell argv: %s", argv.back().c_str());
#endif
}

static void test_detect(const fs::path& projects) {
    auto c = detect_tasks(projects / "c");
    const TaskDef* d = default_task(c, "build");
    CHECK(d && d->label == "make", "make default build");
    CHECK(find_task(c, "make run") && find_task(c, "make run")->group == "run", "make run target");
    CHECK(find_task(c, "make clean") && !find_task(c, "make .PHONY"), "make targets");
    auto r = detect_tasks(projects / "rust-cargo");
    CHECK(default_task(r, "build") && default_task(r, "build")->label == "cargo build", "cargo build");
    CHECK(find_task(r, "cargo clippy") && find_task(r, "cargo clippy")->matchers[0] == "cargo-json", "cargo clippy");
    CHECK(default_task(r, "run") && default_task(r, "run")->label == "cargo run", "cargo run");
    auto m = detect_tasks(projects / "cpp-cmake");
    d = default_task(m, "build");
    CHECK(d && d->label == "CMake: build (debug)", "preset build: %s", d ? d->label.c_str() : "-");
    CHECK(find_task(m, "CMake: configure (broken)"), "configure presets");
    CHECK(make_targets("all: a b\nCC := gcc\n%.o: %.c\n\t$(CC) x\n.PHONY: all\nx y: z\n") ==
              (std::vector<std::string>{"all", "x", "y"}),
          "make_targets");
}

static void test_json() {
    json::Value v;
    CHECK(json::parse(R"({"a": [1, 2.5, "x\u00e9\ud83d\ude00", true, null], /* c */ "b": {"c": -3e2}})", v), "parse");
    CHECK(v["a"][2].str() == "x\xc3\xa9\xf0\x9f\x98\x80", "unicode");
    CHECK(v["b"]["c"].number() == -300 && v["a"].size() == 5 && v["missing"]["x"].is_null(), "lookups");
    CHECK(json::dump(v) == R"({"a":[1,2.5,"x)" "\xc3\xa9\xf0\x9f\x98\x80" R"(",true,null],"b":{"c":-300}})", "dump: %s",
          json::dump(v).c_str());
    CHECK(!json::parse("{\"a\": }", v), "malformed");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: problem_matcher_test <tests dir>\n");
        return 2;
    }
    fs::path tests = argv[1];
    fixtures = tests / "fixtures" / "matchers";
    test_json();
    test_gcc_c();
    test_clang_c();
    test_gcc_cpp();
    test_clang_cpp();
    test_cmake();
    test_rustc_human();
    test_cargo_json();
    test_clippy_json();
    test_msvc();
    test_generic_and_ansi();
    test_links();
    test_tasks_toml();
    test_vscode_tasks();
    test_detect(tests / "projects");
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("problem_matcher_test: all passed\n");
    return 0;
}
