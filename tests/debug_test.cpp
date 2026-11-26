// Debugger building blocks: DAP framing, JSON parsing, breakpoint line shifting
// and persistence, launch configuration parsing (launch.toml, launch.json, Cargo,
// CMake). Exits non-zero on the first failure.
#include "debug/breakpoints.h"
#include "debug/dap_client.h"
#include "debug/json.h"
#include "debug/launch_config.h"
#include <cstdio>
#include <fstream>
#include <random>

using namespace oh;

namespace oh {
void request_redraw() {}  // the DAP reader wakes the UI; nothing to wake here
}

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

static void test_framing() {
    std::string a = R"({"seq":1,"type":"event","event":"initialized"})";
    std::string b = R"({"seq":2,"type":"response","request_seq":1,"success":true,"body":{"x":"éé"}})";
    std::string stream = dap_frame(a) + dap_frame(b);
    CHECK(dap_frame(a).rfind("Content-Length: " + std::to_string(a.size()) + "\r\n\r\n", 0) == 0, "frame header");

    // Every split point of the stream yields the same two messages.
    for (size_t cut = 0; cut <= stream.size(); cut++) {
        DapFramer f;
        std::vector<std::string> got;
        std::string body;
        f.feed(stream.data(), cut);
        while (f.next(body)) got.push_back(body);
        f.feed(stream.data() + cut, stream.size() - cut);
        while (f.next(body)) got.push_back(body);
        CHECK(got.size() == 2 && got[0] == a && got[1] == b, "split at %zu: got %zu messages", cut, got.size());
    }
    // Byte-by-byte, random chunking.
    std::mt19937 rng(7);
    for (int round = 0; round < 50; round++) {
        DapFramer f;
        std::vector<std::string> got;
        std::string body, big = stream + stream + stream;
        for (size_t i = 0; i < big.size();) {
            size_t n = std::min(big.size() - i, static_cast<size_t>(rng() % 9 + 1));
            f.feed(big.data() + i, n);
            i += n;
            while (f.next(body)) got.push_back(body);
        }
        CHECK(got.size() == 6, "random chunks: %zu", got.size());
    }
    // Extra header fields and case-insensitive names.
    {
        DapFramer f;
        std::string m = "content-length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}";
        f.feed(m.data(), m.size());
        std::string body;
        CHECK(f.next(body) && body == "{}", "lowercase header / extra field");
    }
    // Garbage before a header is skipped (an adapter printing to stdout).
    {
        DapFramer f;
        std::string m = "warning: something\r\n\r\n" + dap_frame("{\"a\":1}");
        f.feed(m.data(), m.size());
        std::string body;
        CHECK(f.next(body) && body == "{\"a\":1}", "garbage skipped");
        CHECK(f.error(), "garbage flagged");
    }
    // A body that is not complete yet stays buffered.
    {
        DapFramer f;
        std::string m = dap_frame("{\"long\":\"0123456789\"}");
        f.feed(m.data(), m.size() - 1);
        std::string body;
        CHECK(!f.next(body), "partial body");
        f.feed(m.data() + m.size() - 1, 1);
        CHECK(f.next(body) && body == "{\"long\":\"0123456789\"}", "completed body");
    }
}

static void test_json() {
    Json j;
    std::string err;
    CHECK(Json::parse(R"({"a":1,"b":[true,false,null],"c":{"d":"x\ny\"z\\"},"e":-2.5e3,"u":"é😀"})", j, &err),
          "parse: %s", err.c_str());
    CHECK(j["a"].integer() == 1, "a");
    CHECK(j["b"].size() == 3 && j["b"][0].boolean() && !j["b"][1].boolean(true) && j["b"][2].is_null(), "b");
    CHECK(j["c"]["d"].str() == "x\ny\"z\\", "escapes");
    CHECK(j["e"].number() == -2500.0, "number");
    CHECK(j["u"].str() == "\xc3\xa9\xf0\x9f\x98\x80", "unicode: %s", j["u"].str().c_str());
    CHECK(j["missing"]["deeper"].str().empty() && j["missing"].integer(42) == 42, "lenient access");
    Json back;
    CHECK(Json::parse(j.dump(), back) && back.dump() == j.dump(), "round trip: %s", j.dump().c_str());
    CHECK(!Json::parse("{\"a\":1,}", j), "trailing comma rejected in strict mode");
    CHECK(!Json::parse("[1 2]", j), "missing comma");
    CHECK(!Json::parse("{\"a\":1} x", j), "trailing text");
    // JSONC (launch.json): comments and trailing commas.
    CHECK(Json::parse("// top\n{ /* c */ \"a\": [1, 2,], // x\n \"b\": \"//not a comment\", }", j, &err, true),
          "relaxed: %s", err.c_str());
    CHECK(j["a"].size() == 2 && j["b"].str() == "//not a comment", "relaxed values");
    Json o = Json::object({{"seq", 3}, {"command", "next"}});
    o.set("arguments", Json::object({{"threadId", int64_t{7}}}));
    CHECK(o.dump() == R"({"seq":3,"command":"next","arguments":{"threadId":7}})", "build: %s", o.dump().c_str());
    CHECK(Json(std::string("a\x01")).dump() == "\"a\\u0001\"", "control escape");
}

static LineEdit insert(size_t line, size_t col, size_t new_lines, size_t end_col = 0) {
    return {line, col, line, col, line + new_lines, new_lines ? end_col : col + end_col};
}
static LineEdit erase(size_t l0, size_t c0, size_t l1, size_t c1) { return {l0, c0, l1, c1, l0, c0}; }

static void test_shift() {
    // Enter in the middle of an earlier line: breakpoints below move down.
    CHECK(shift_line_for_edit(10, insert(5, 3, 1)) == 11, "below insert");
    CHECK(shift_line_for_edit(4, insert(5, 3, 1)) == 4, "above insert");
    // Typing on the breakpoint's own line keeps it.
    CHECK(shift_line_for_edit(5, insert(5, 3, 0, 4)) == 5, "same line text");
    CHECK(shift_line_for_edit(5, insert(5, 3, 2)) == 5, "newline after col 0 keeps the line");
    // Enter at column 0 pushes the line (and its breakpoint) down.
    CHECK(shift_line_for_edit(5, insert(5, 0, 1)) == 6, "newline at col 0");
    CHECK(shift_line_for_edit(5, insert(5, 0, 3)) == 8, "paste 3 lines at col 0");
    // Deleting lines 3..5 entirely (3:0 - 6:0).
    CHECK(shift_line_for_edit(2, erase(3, 0, 6, 0)) == 2, "above delete");
    CHECK(shift_line_for_edit(4, erase(3, 0, 6, 0)) == 3, "inside delete collapses");
    CHECK(shift_line_for_edit(6, erase(3, 0, 6, 0)) == 3, "first line after delete");
    CHECK(shift_line_for_edit(9, erase(3, 0, 6, 0)) == 6, "far below delete");
    // Joining lines (Backspace at the start of line 7: 6:len - 7:0).
    CHECK(shift_line_for_edit(7, erase(6, 12, 7, 0)) == 6, "joined line");
    CHECK(shift_line_for_edit(8, erase(6, 12, 7, 0)) == 7, "after join");
    // Replacing two lines with five.
    LineEdit rep{2, 4, 4, 1, 7, 3};
    CHECK(shift_line_for_edit(10, rep) == 13, "replace grows");

    BreakpointStore s;
    fs::path p = "/tmp/x.c", q = "/tmp/y.c";
    s.toggle(p, 3);
    s.toggle(p, 4);
    s.toggle(p, 9);
    s.toggle(q, 3);
    CHECK(s.source().size() == 4, "toggle adds");
    CHECK(!s.toggle(p, 9) && s.source().size() == 3, "toggle removes");
    uint64_t gen = s.generation();
    // Delete lines 3-4 of x.c: both collapse onto line 3 and merge.
    CHECK(s.shift_for_edit(p, erase(3, 0, 5, 0)), "moved");
    CHECK(s.in_file(p).size() == 1 && s.in_file(p)[0]->line == 3, "merged after collapse");
    CHECK(s.in_file(q).size() == 1 && s.in_file(q)[0]->line == 3, "other file untouched");
    CHECK(s.generation() != gen, "generation bumped");
    // The adapter's line moves too.
    s.in_file(p)[0]->actual_line = 4;
    s.shift_for_edit(p, insert(0, 0, 2));
    CHECK(s.in_file(p)[0]->line == 5 && s.in_file(p)[0]->actual_line == 6, "actual line shifted");
    CHECK(!s.shift_for_edit(p, insert(20, 0, 1)), "edit below moves nothing");
}

static void test_persist() {
    BreakpointStore s;
    SourceBreakpoint& b = s.ensure("/src/a b.cpp", 12);
    b.condition = "i == 3\t&& j";
    b.log_message = "x = {x}\nnext";
    b.enabled = false;
    s.ensure("/src/a b.cpp", 2).hit_condition = ">= 5";
    s.functions().push_back(FunctionBreakpoint{"rust_begin_unwind", {}, true, -1, false});
    s.set_filter("rust_panic", true);
    BreakpointStore t;
    t.deserialize(s.serialize() + "watch\tignored by the store\n");
    CHECK(t.source().size() == 2, "count %zu", t.source().size());
    SourceBreakpoint* tb = t.find("/src/a b.cpp", 12);
    CHECK(tb && tb->condition == "i == 3\t&& j" && tb->log_message == "x = {x}\nnext" && !tb->enabled, "fields");
    CHECK(t.find("/src/a b.cpp", 2) && t.find("/src/a b.cpp", 2)->hit_condition == ">= 5", "hit condition");
    CHECK(t.functions().size() == 1 && t.functions()[0].name == "rust_begin_unwind", "function bp");
    CHECK(t.filter_on("rust_panic") && !t.filter_on("throw"), "filters");
}

static void test_launch_toml() {
    std::string toml = R"(# comment
[[configurations]]
name = "Run tests"   # trailing comment
program = "${workspaceFolder}/build/tests"
args = ["--filter", "a b", 'lit\eral']
cwd = "${workspaceFolder}/data"
env = { RUST_LOG = "debug", N = 3 }
stopAtEntry = true
adapter = "lldb-dap"
build = "cmake --build build"

[other]
name = "ignored"

[[configurations]]
name = "attach"
request = "attach"
pid = 4242
program = "bin/server"
args = [
  "multi",   # per-line comments
  "line",
]
)";
    std::vector<LaunchConfig> out;
    std::string err;
    CHECK(parse_launch_toml(toml, out, &err), "parse: %s", err.c_str());
    CHECK(out.size() == 2, "count %zu", out.size());
    if (out.size() != 2) return;
    LaunchConfig& c = out[0];
    CHECK(c.name == "Run tests" && c.adapter == AdapterKind::LldbDap && c.stop_at_entry, "fields");
    CHECK(c.args.size() == 3 && c.args[1] == "a b" && c.args[2] == "lit\\eral", "args");
    CHECK(c.env.size() == 2 && c.env[0].first == "RUST_LOG" && c.env[1].second == "3", "env");
    CHECK(c.build_command == "cmake --build build", "build");
    CHECK(out[1].attach && out[1].pid == 4242 && out[1].args.size() == 2 && out[1].args[1] == "line", "attach");
    LaunchVars v{"/home/u/proj", "/home/u/proj/src/main.c"};
    resolve_config(c, v);
    CHECK(c.program == "/home/u/proj/build/tests", "program %s", c.program.c_str());
    CHECK(c.cwd == "/home/u/proj/data", "cwd %s", c.cwd.c_str());
    resolve_config(out[1], v);
    CHECK(out[1].program == "/home/u/proj/bin/server", "relative program %s", out[1].program.c_str());
    CHECK(substitute_vars("${workspaceFolderBasename}:${fileBasenameNoExtension}:${unknown}", v) == "proj:main:${unknown}",
          "vars");

    std::vector<LaunchConfig> bad;
    CHECK(!parse_launch_toml("[[configurations]]\nname = \"x\nprogram = 1\n", bad, &err), "unterminated string");
    CHECK(!parse_launch_toml("[[configurations]]\nprogram \"x\"\n", bad, &err) && err.find("line 2") != std::string::npos,
          "error line: %s", err.c_str());
}

static void test_vscode_launch() {
    std::string json = R"({
  // VS Code launch.json with comments
  "version": "0.2.0",
  "configurations": [
    {
      "name": "(gdb) Launch",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/app",
      "args": ["-v"],
      "stopAtEntry": true,
      "cwd": "${workspaceFolder}",
      "environment": [{"name": "FOO", "value": "bar"}],
      "MIMode": "gdb",
      "miDebuggerPath": "/opt/gdb/bin/gdb",
      "preLaunchTask": "build",
    },
    {"name": "CodeLLDB", "type": "lldb", "request": "launch", "cargo": {"args": ["build", "--bin=demo"]},
     "program": "${workspaceFolder}/target/debug/demo", "env": {"RUST_BACKTRACE": "1"}, "stopOnEntry": false},
    {"name": "lldb-dap attach", "type": "lldb-dap", "request": "attach", "pid": "${command:pickProcess}",
     "env": ["A=1"]},
    {"name": "Node", "type": "node", "request": "launch"},
    {"name": "Native Debug", "type": "gdb", "request": "launch", "target": "./bin/x", "arguments": "a \"b c\""}
  ]
})";
    std::vector<LaunchConfig> out;
    std::string err;
    CHECK(parse_vscode_launch(json, out, &err), "parse: %s", err.c_str());
    CHECK(out.size() == 4, "node skipped: %zu", out.size());
    if (out.size() != 4) return;
    CHECK(out[0].adapter == AdapterKind::Gdb && out[0].adapter_path == "/opt/gdb/bin/gdb" && out[0].stop_at_entry &&
              out[0].env.size() == 1 && out[0].env[0].second == "bar" && out[0].pre_launch_task == "build",
          "cppdbg");
    CHECK(out[1].adapter == AdapterKind::CodeLldb && out[1].build_command == "cargo build --bin=demo" &&
              out[1].language == "rust" && out[1].env[0].first == "RUST_BACKTRACE",
          "lldb");
    CHECK(out[2].adapter == AdapterKind::LldbDap && out[2].attach && out[2].pid == 0 && out[2].env[0].first == "A",
          "lldb-dap attach");
    CHECK(out[3].program == "./bin/x" && out[3].args.size() == 2 && out[3].args[1] == "b c", "native debug");
    // A Rust program under target/ switches gdb to the Rust pretty printers.
    LaunchConfig r;
    r.program = "/p/target/debug/demo";
    resolve_config(r, {"/p", ""});
    CHECK(r.adapter == AdapterKind::RustGdb && r.language == "rust", "rust detection");
}

static void test_cargo_cmake(const fs::path& tmp) {
    Json md;
    CHECK(Json::parse(R"({"packages":[{"name":"pkg","targets":[{"kind":["lib"],"name":"pkg"},
        {"kind":["bin"],"name":"tool"}]}],"target_directory":"/w/target","workspace_root":"/w"})", md), "metadata");
    auto cargo = cargo_configs_from_metadata(md);
    CHECK(cargo.size() == 1 && cargo[0].program == fs::path("/w/target/debug/tool").string() &&
              cargo[0].build_command == "cargo build -p pkg --bin tool" && cargo[0].adapter == AdapterKind::RustGdb,
          "cargo metadata");
    auto manifest = cargo_configs_from_manifest("[package]\nname = \"demo\"\n\n[[bin]]\nname = \"extra\"\n", "/w");
    CHECK(manifest.size() == 2 && manifest[1].name == "extra (Cargo)", "manifest fallback");

    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "build", ec);
    std::ofstream(tmp / "CMakeLists.txt") << "add_executable(app main.cpp)\n";
    std::ofstream(tmp / "build" / "CMakeCache.txt") << "\n";
    std::ofstream(tmp / "build" / "build.ninja")
        << "build CMakeFiles/app.dir/main.cpp.o: CXX_COMPILER__app_Debug /x/main.cpp\n"
           "build app: CXX_EXECUTABLE_LINKER__app_Debug CMakeFiles/app.dir/main.cpp.o\n"
           "build tools/gen: CXX_EXECUTABLE_LINKER__gen_ tools/x.o\n";
    auto dirs = cmake_build_dirs(tmp);
    CHECK(dirs.size() == 1 && dirs[0] == tmp / "build", "build dirs %zu", dirs.size());
    auto cm = cmake_configs(tmp, tmp / "build");
    CHECK(cm.size() == 2, "ninja targets %zu", cm.size());
    if (cm.size() == 2) {
        CHECK(cm[0].program == (tmp / "build" / "app").string() && cm[0].name == "app (CMake: build)", "app: %s",
              cm[0].name.c_str());
        CHECK(cm[1].program == (tmp / "build" / "tools/gen").string(), "gen");
        CHECK(cm[0].build_command.find("--target app") != std::string::npos, "build command");
    }
    // Makefile generator: executables named like add_executable() targets.
    fs::remove(tmp / "build" / "build.ninja", ec);
    std::ofstream(tmp / "build" / "app") << "ELF";
    cm = cmake_configs(tmp, tmp / "build");
    CHECK(cm.size() == 1 && cm[0].program == (tmp / "build" / "app").string(), "makefile scan %zu", cm.size());
    fs::remove_all(tmp, ec);
}

int main(int argc, char** argv) {
    fs::path tmp = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "overheat_debug_test";
    test_framing();
    test_json();
    test_shift();
    test_persist();
    test_launch_toml();
    test_vscode_launch();
    test_cargo_cmake(tmp);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("debug_test: all passed\n");
    return 0;
}
