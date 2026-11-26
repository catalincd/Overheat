#include "tasks/tasks.h"
#include "tasks/json.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace oh {

namespace {

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool on_path(const std::string& program) {
#ifdef _WIN32
    const char sep = ';';
    const char* exts[] = {".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    const char* exts[] = {""};
#endif
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::error_code ec;
    for (std::string dir; std::getline(ss, dir, sep);)
        for (const char* ext : exts)
            if (!dir.empty() && fs::exists(fs::path(dir) / (program + ext), ec)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// A small TOML reader: [[array]] / [table] headers, key = value with strings
// (basic, literal, multi-line basic), integers, booleans, arrays and inline tables.
// ---------------------------------------------------------------------------

struct TomlSection {
    std::string header;  // "[[task]]", "[task.env]", "" for the top level
    json::Value table = json::Value::make_object();
};

class TomlReader {
public:
    explicit TomlReader(const std::string& t) : s(t) {}
    const std::string& s;
    size_t i = 0;
    std::string err;

    int line_no() const { return 1 + static_cast<int>(std::count(s.begin(), s.begin() + static_cast<long>(std::min(i, s.size())), '\n')); }
    bool fail(const std::string& what) {
        if (err.empty()) err = "line " + std::to_string(line_no()) + ": " + what;
        return false;
    }
    void skip_space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    void skip_all() {  // spaces, newlines and comments
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            if (i < s.size() && s[i] == '#') {
                while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            return;
        }
    }
    bool basic_string(std::string& out) {
        bool multi = s.compare(i, 3, "\"\"\"") == 0;
        i += multi ? 3 : 1;
        if (multi && i < s.size() && s[i] == '\n') i++;
        while (i < s.size()) {
            char c = s[i];
            if (multi ? s.compare(i, 3, "\"\"\"") == 0 : c == '"') {
                i += multi ? 3 : 1;
                return true;
            }
            if (!multi && c == '\n') return fail("newline in string");
            if (c == '\\' && i + 1 < s.size()) {
                char e = s[i + 1];
                i += 2;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u': case 'U': {
                        size_t n = e == 'u' ? 4 : 8;
                        uint32_t cp = static_cast<uint32_t>(std::strtoul(s.substr(i, n).c_str(), nullptr, 16));
                        i += n;
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xF0 | (cp >> 18));
                            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e;
                }
                continue;
            }
            out += c;
            i++;
        }
        return fail("unterminated string");
    }
    bool literal_string(std::string& out) {
        size_t end = s.find('\'', i + 1);
        if (end == std::string::npos || s.find('\n', i + 1) < end) return fail("unterminated string");
        out = s.substr(i + 1, end - i - 1);
        i = end + 1;
        return true;
    }
    bool key(std::string& out) {
        skip_space();
        if (i < s.size() && s[i] == '"') return basic_string(out);
        if (i < s.size() && s[i] == '\'') return literal_string(out);
        size_t st = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-')) i++;
        if (i == st) return fail("expected a key");
        out = s.substr(st, i - st);
        return true;
    }
    bool value(json::Value& v) {
        skip_space();
        if (i >= s.size()) return fail("expected a value");
        char c = s[i];
        if (c == '"') {
            std::string str;
            if (!basic_string(str)) return false;
            v = json::Value::make_string(std::move(str));
        } else if (c == '\'') {
            std::string str;
            if (!literal_string(str)) return false;
            v = json::Value::make_string(std::move(str));
        } else if (c == '[') {
            i++;
            v = json::Value::make_array();
            for (;;) {
                skip_all();
                if (i < s.size() && s[i] == ']') { i++; break; }
                json::Value item;
                if (!value(item)) return false;
                v.push(std::move(item));
                skip_all();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                return fail("expected ',' or ']'");
            }
        } else if (c == '{') {
            i++;
            v = json::Value::make_object();
            for (;;) {
                skip_all();
                if (i < s.size() && s[i] == '}') { i++; break; }
                std::string k;
                if (!key(k)) return false;
                skip_space();
                if (i >= s.size() || s[i] != '=') return fail("expected '='");
                i++;
                json::Value item;
                if (!value(item)) return false;
                v.set(k, std::move(item));
                skip_all();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                return fail("expected ',' or '}'");
            }
        } else if (s.compare(i, 4, "true") == 0) {
            v = json::Value::make_bool(true);
            i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            v = json::Value::make_bool(false);
            i += 5;
        } else if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            const char* st = s.c_str() + i;
            char* end = nullptr;
            double n = std::strtod(st, &end);
            i += static_cast<size_t>(end - st);
            v = json::Value::make_number(n);
        } else {
            return fail("unsupported value");
        }
        return true;
    }
    bool parse(std::vector<TomlSection>& out) {
        out.clear();
        out.push_back({});
        for (;;) {
            skip_all();
            if (i >= s.size()) return true;
            if (s[i] == '[') {
                bool arr = s.compare(i, 2, "[[") == 0;
                size_t end = s.find(arr ? "]]" : "]", i);
                size_t nl = s.find('\n', i);
                if (end == std::string::npos || end > nl) return fail("unterminated table header");
                TomlSection sec;
                sec.header = (arr ? "[[" : "[") + trim(s.substr(i + (arr ? 2 : 1), end - i - (arr ? 2 : 1))) + (arr ? "]]" : "]");
                out.push_back(std::move(sec));
                i = end + (arr ? 2 : 1);
            } else {
                std::string k;
                if (!key(k)) return false;
                skip_space();
                if (i >= s.size() || s[i] != '=') return fail("expected '=' after " + k);
                i++;
                json::Value v;
                if (!value(v)) return false;
                out.back().table.set(k, std::move(v));
            }
            skip_space();
            if (i < s.size() && s[i] == '#') continue;
            if (i < s.size() && s[i] != '\n' && s[i] != '\r') return fail("expected end of line");
        }
    }
};

std::vector<std::string> string_list(const json::Value& v) {
    std::vector<std::string> out;
    if (v.is_string()) out.push_back(v.str());
    for (auto& it : v.items())
        if (it.is_string()) out.push_back(it.str());
    return out;
}

std::vector<std::pair<std::string, std::string>> string_map(const json::Value& v) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [k, val] : v.members()) {
        if (val.is_string()) out.emplace_back(k, val.str());
        else if (val.is_number()) out.emplace_back(k, json::dump(val));
        else if (val.is_bool()) out.emplace_back(k, val.boolean() ? "true" : "false");
    }
    return out;
}

std::string shell_quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"'\\$`&|;<>()*?[]{}!#~") == std::string::npos) return a;
#ifdef _WIN32
    std::string out = "\"";
    for (char c : a) out += c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (char c : a) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Variables and argv
// ---------------------------------------------------------------------------

std::string expand_task_vars(const std::string& s, const TaskContext& ctx) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "${") != 0) {
            out += s[i++];
            continue;
        }
        size_t end = s.find('}', i);
        if (end == std::string::npos) {
            out += s.substr(i);
            break;
        }
        std::string name = s.substr(i + 2, end - i - 2);
        std::string val;
        bool known = true;
        fs::path file = ctx.file;
        if (name == "workspaceFolder" || name == "workspaceRoot" || name == "cwd") val = ctx.workspace.string();
        else if (name == "workspaceFolderBasename") val = ctx.workspace.filename().string();
        else if (name == "file") val = file.string();
        else if (name == "fileDirname") val = file.parent_path().string();
        else if (name == "fileBasename") val = file.filename().string();
        else if (name == "fileBasenameNoExtension") val = file.stem().string();
        else if (name == "fileExtname") val = file.extension().string();
        else if (name == "relativeFile") val = file.empty() ? "" : file.lexically_relative(ctx.workspace).string();
        else if (name == "lineNumber") val = std::to_string(ctx.line);
        else if (name == "pathSeparator") val = std::string(1, static_cast<char>(fs::path::preferred_separator));
        else if (starts_with(name, "env:")) {
            const char* e = std::getenv(name.substr(4).c_str());
            val = e ? e : "";
        } else known = false;
        out += known ? val : s.substr(i, end - i + 1);
        i = end + 1;
    }
    return out;
}

fs::path task_cwd(const TaskDef& t, const TaskContext& ctx) {
    if (t.cwd.empty()) return ctx.workspace;
    fs::path p = expand_task_vars(t.cwd, ctx);
    return p.is_absolute() ? p : ctx.workspace / p;
}

std::string task_command_line(const TaskDef& t, const TaskContext& ctx) {
    std::string line = expand_task_vars(t.command, ctx);
    for (auto& a : t.args) line += " " + (t.shell ? shell_quote(expand_task_vars(a, ctx)) : expand_task_vars(a, ctx));
    return line;
}

std::vector<std::string> task_argv(const TaskDef& t, const TaskContext& ctx) {
    if (t.shell) {
        std::string line = expand_task_vars(t.command, ctx);
        for (auto& a : t.args) line += " " + shell_quote(expand_task_vars(a, ctx));
#ifdef _WIN32
        return {"cmd.exe", "/d", "/s", "/c", line};
#else
        return {"/bin/sh", "-c", line};
#endif
    }
    std::vector<std::string> argv{expand_task_vars(t.command, ctx)};
    for (auto& a : t.args) argv.push_back(expand_task_vars(a, ctx));
    return argv;
}

// ---------------------------------------------------------------------------
// Auto-detection
// ---------------------------------------------------------------------------

std::vector<std::string> make_targets(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '\t' || line[0] == ' ' || line[0] == '#' || line[0] == '.') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        if (colon + 1 < line.size() && (line[colon + 1] == '=' || line[colon + 1] == ':')) continue;  // := / ::=
        if (line.find('=') < colon) continue;  // a variable assignment
        std::string names = line.substr(0, colon);
        std::istringstream ns(names);
        for (std::string n; ns >> n;) {
            if (n.find_first_of("%$()") != std::string::npos) continue;
            if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        }
        if (out.size() >= 40) break;
    }
    return out;
}

namespace {

// binaryDir of a configure preset, following `inherits`; "" when it cannot be resolved.
std::string preset_binary_dir(const json::Value& presets, const json::Value& p, const fs::path& root, int depth = 0) {
    std::string dir = p["binaryDir"].str();
    if (dir.empty() && depth < 8) {
        for (auto& parent_name : string_list(p["inherits"]))
            for (auto& q : presets.items())
                if (q["name"].str() == parent_name) {
                    dir = preset_binary_dir(presets, q, root, depth + 1);
                    if (!dir.empty()) break;
                }
    }
    if (dir.empty()) return {};
    auto replace = [&](const std::string& var, const std::string& val) {
        for (size_t at; (at = dir.find(var)) != std::string::npos;) dir.replace(at, var.size(), val);
    };
    replace("${sourceDir}", root.string());
    replace("${sourceParentDir}", root.parent_path().string());
    replace("${sourceDirName}", root.filename().string());
    replace("${presetName}", p["name"].str());
    if (dir.find("${") != std::string::npos) return {};
    fs::path d = dir;
    return (d.is_absolute() ? d : root / d).string();
}

void detect_cmake(const fs::path& root, std::vector<TaskDef>& out) {
    std::error_code ec;
    if (!fs::exists(root / "CMakeLists.txt", ec)) return;
    const std::vector<std::string> build_matchers = {"gcc", "msvc", "cmake"};
    json::Value presets;
    std::string text;
    bool have_presets = false;
    for (const char* name : {"CMakeUserPresets.json", "CMakePresets.json"}) {
        json::Value v;
        if (read_file(root / name, text) && json::parse(text, v) && v.is_object()) {
            if (!have_presets) presets = json::Value::make_object();
            for (const char* kind : {"configurePresets", "buildPresets", "testPresets"}) {
                json::Value merged = presets[kind].is_array() ? presets[kind] : json::Value::make_array();
                for (auto& p : v[kind].items()) merged.push(p);
                presets.set(kind, std::move(merged));
            }
            have_presets = true;
        }
    }
    if (have_presets) {
        const json::Value& configures = presets["configurePresets"];
        bool first_build = true;
        for (auto& p : configures.items()) {
            if (p["hidden"].boolean()) continue;
            TaskDef t;
            t.label = "CMake: configure (" + p["name"].str() + ")";
            t.source = "CMake";
            t.command = "cmake";
            t.args = {"--preset", p["name"].str()};
            t.matchers = {"cmake"};
            t.detail = p["displayName"].str_or("cmake --preset " + p["name"].str());
            out.push_back(std::move(t));
        }
        for (auto& p : presets["buildPresets"].items()) {
            if (p["hidden"].boolean()) continue;
            TaskDef t;
            t.label = "CMake: build (" + p["name"].str() + ")";
            t.source = "CMake";
            t.command = "cmake";
            t.args = {"--build", "--preset", p["name"].str()};
            t.group = "build";
            t.is_default = first_build;
            first_build = false;
            t.matchers = build_matchers;
            t.detail = p["displayName"].str_or("cmake --build --preset " + p["name"].str());
            // Not configured yet: run its configure preset first.
            std::string conf = p["configurePreset"].str();
            for (auto& c : configures.items())
                if (c["name"].str() == conf) {
                    std::string dir = preset_binary_dir(configures, c, root);
                    if (dir.empty() || !fs::exists(fs::path(dir) / "CMakeCache.txt", ec))
                        t.depends_on = {"CMake: configure (" + conf + ")"};
                }
            out.push_back(std::move(t));
        }
        for (auto& p : presets["testPresets"].items()) {
            if (p["hidden"].boolean()) continue;
            TaskDef t;
            t.label = "CTest (" + p["name"].str() + ")";
            t.source = "CMake";
            t.command = "ctest";
            t.args = {"--preset", p["name"].str()};
            t.group = "test";
            t.detail = "ctest --preset " + p["name"].str();
            out.push_back(std::move(t));
        }
        if (!first_build) return;  // presets define the builds
    }
    TaskDef conf;
    conf.label = "CMake: configure";
    conf.source = "CMake";
    conf.command = "cmake";
    conf.args = {"-S", ".", "-B", "build", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    if (on_path("ninja")) conf.args.insert(conf.args.end(), {"-G", "Ninja"});
    conf.matchers = {"cmake"};
    conf.detail = "cmake -S . -B build";
    TaskDef build;
    build.label = "CMake: build";
    build.source = "CMake";
    build.command = "cmake";
    build.args = {"--build", "build"};
    build.group = "build";
    build.is_default = true;
    build.matchers = build_matchers;
    build.detail = "cmake --build build";
    if (!fs::exists(root / "build" / "CMakeCache.txt", ec)) build.depends_on = {conf.label};
    TaskDef clean = build;
    clean.label = "CMake: clean";
    clean.args = {"--build", "build", "--target", "clean"};
    clean.group.clear();
    clean.is_default = false;
    clean.depends_on.clear();
    clean.detail = "cmake --build build --target clean";
    TaskDef test;
    test.label = "CTest";
    test.source = "CMake";
    test.command = "ctest";
    test.args = {"--test-dir", "build", "--output-on-failure"};
    test.group = "test";
    test.detail = "ctest --test-dir build";
    out.push_back(std::move(build));
    out.push_back(std::move(conf));
    out.push_back(std::move(clean));
    out.push_back(std::move(test));
}

void detect_make(const fs::path& root, std::vector<TaskDef>& out) {
    std::string text;
    bool found = false;
    for (const char* name : {"GNUmakefile", "makefile", "Makefile"})
        if (read_file(root / name, text)) {
            found = true;
            break;
        }
    if (!found) return;
    TaskDef build;
    build.label = "make";
    build.source = "Make";
    build.command = "make";
    build.group = "build";
    build.is_default = true;
    build.matchers = {"gcc", "generic"};
    build.detail = "make (default target)";
    out.push_back(build);
    for (auto& target : make_targets(text)) {
        TaskDef t = build;
        t.label = "make " + target;
        t.args = {target};
        t.is_default = false;
        t.group = target == "run" ? "run" : (target == "test" || target == "check") ? "test" : "";
        t.detail = "make " + target;
        if (t.group == "run" || t.group == "test") t.matchers = {"gcc", "rustc"};
        out.push_back(std::move(t));
    }
}

void detect_cargo(const fs::path& root, std::vector<TaskDef>& out) {
    std::error_code ec;
    if (!fs::exists(root / "Cargo.toml", ec)) return;
    const std::string fmt = "--message-format=json-diagnostic-rendered-ansi";
    struct Spec { const char* cmd; const char* group; bool def; const char* detail; };
    for (auto s : {Spec{"build", "build", true, "Compile the package"},
                   Spec{"check", "build", false, "Check for errors without building"},
                   Spec{"clippy", "build", false, "Lint with Clippy"},
                   Spec{"test", "test", true, "Run the tests"},
                   Spec{"run", "run", true, "Build and run the binary"},
                   Spec{"clean", "", false, "Remove the target directory"}}) {
        TaskDef t;
        t.label = std::string("cargo ") + s.cmd;
        t.source = "Cargo";
        t.command = "cargo";
        t.args = {s.cmd};
        if (std::string(s.cmd) != "clean") {
            t.args.push_back(fmt);
            t.matchers = {"cargo-json", "rustc"};
        }
        t.group = s.group;
        t.is_default = s.def;
        t.detail = s.detail;
        out.push_back(std::move(t));
    }
}

TaskDef task_from_table(const json::Value& v, const std::string& source) {
    TaskDef t;
    t.source = source;
    t.label = v["label"].str();
    if (v["program"].is_string()) {
        t.command = v["program"].str();
        t.shell = false;
    } else {
        t.command = v["command"].str();
        t.shell = true;
    }
    t.args = string_list(v["args"]);
    t.cwd = v["cwd"].str();
    t.env = string_map(v["env"]);
    t.group = v["group"].str();
    t.is_default = v["default"].boolean();
    for (auto& m : string_list(v["matcher"]))
        if (m != "none") t.matchers.push_back(m);
    t.depends_on = string_list(v["depends_on"]);
    t.detail = v["detail"].str();
    return t;
}

} // namespace

std::vector<TaskDef> detect_tasks(const fs::path& root) {
    std::vector<TaskDef> out;
    detect_cargo(root, out);
    detect_cmake(root, out);
    detect_make(root, out);
    return out;
}

// ---------------------------------------------------------------------------
// tasks.toml
// ---------------------------------------------------------------------------

bool parse_tasks_toml(const std::string& text, std::vector<TaskDef>& out, std::string* error) {
    std::vector<TomlSection> sections;
    TomlReader r(text);
    if (!r.parse(sections)) {
        if (error) *error = r.err;
        return false;
    }
    std::vector<json::Value> tables;
    for (auto& sec : sections) {
        if (sec.header == "[[task]]" || sec.header == "[[tasks]]") {
            tables.push_back(sec.table);
        } else if (starts_with(sec.header, "[task.") && !tables.empty()) {
            std::string sub = sec.header.substr(6, sec.header.size() - 7);  // [task.env] -> env
            tables.back().set(sub, sec.table);
        }
    }
    for (auto& t : tables) {
        TaskDef d = task_from_table(t, "tasks.toml");
        if (d.label.empty() || d.command.empty()) {
            if (error) *error = "a [[task]] needs a label and a command or program";
            return false;
        }
        out.push_back(std::move(d));
    }
    return true;
}

// ---------------------------------------------------------------------------
// .vscode/tasks.json (the common subset)
// ---------------------------------------------------------------------------

bool parse_vscode_tasks(const std::string& text, std::vector<TaskDef>& out, std::string* error) {
    json::Value root;
    if (!json::parse(text, root, error)) return false;
#ifdef _WIN32
    const char* os_key = "windows";
#elif defined(__APPLE__)
    const char* os_key = "osx";
#else
    const char* os_key = "linux";
#endif
    // Global options (cwd, env) and the per-OS overrides apply to every task.
    auto apply = [&](const json::Value& src, TaskDef& t, bool& shell_type) {
        if (src["type"].is_string()) shell_type = src["type"].str() == "shell";
        if (src["command"].is_string()) t.command = src["command"].str();
        if (src["args"].is_array()) {
            t.args.clear();
            for (auto& a : src["args"].items())
                t.args.push_back(a.is_string() ? a.str() : a["value"].str());  // {value, quoting}
        }
        const json::Value& opt = src["options"];
        if (opt["cwd"].is_string()) t.cwd = opt["cwd"].str();
        for (auto& kv : string_map(opt["env"])) t.env.push_back(kv);
    };
    std::vector<TaskDef> parsed;
    for (auto& v : root["tasks"].items()) {
        TaskDef t;
        t.source = "tasks.json";
        bool shell = true;
        apply(root, t, shell);  // top-level options
        apply(root[os_key], t, shell);
        shell = true;
        apply(v, t, shell);
        apply(v[os_key], t, shell);
        std::string type = v["type"].str_or("process");
        t.label = v["label"].str_or(v["taskName"].str());
        if (type == "cargo") {  // rust-analyzer's task type: {"type":"cargo","command":"build"}
            t.args.insert(t.args.begin(), t.command);
            t.command = "cargo";
            t.shell = false;
            if (t.label.empty()) t.label = "cargo " + t.args.front();
        } else {
            t.shell = type == "shell";
        }
        if (t.label.empty()) t.label = t.command;
        const json::Value& g = v["group"];
        if (g.is_string()) {
            t.group = g.str();
        } else if (g.is_object()) {
            t.group = g["kind"].str();
            t.is_default = g["isDefault"].boolean() || g["isDefault"].is_string();
        }
        if (t.group != "build" && t.group != "test") t.group.clear();
        const json::Value& pm = v["problemMatcher"];
        auto add_matcher = [&](const json::Value& m) {
            if (m.is_string()) t.matchers.push_back(m.str());
            else if (m.is_object()) t.matchers.push_back(m["base"].is_string() ? m["base"].str() : "generic");
        };
        if (pm.is_array()) for (auto& m : pm.items()) add_matcher(m);
        else add_matcher(pm);
        t.depends_on = string_list(v["dependsOn"]);
        t.detail = v["detail"].str();
        if (t.command.empty() && t.depends_on.empty()) continue;  // npm scripts etc.: not supported
        parsed.push_back(std::move(t));
    }
    out.insert(out.end(), parsed.begin(), parsed.end());
    return true;
}

std::vector<TaskDef> load_tasks(const fs::path& root, std::vector<std::string>* errors) {
    std::vector<TaskDef> out;
    std::string text, err;
    if (read_file(tasks_file(root), text)) {
        if (!parse_tasks_toml(text, out, &err) && errors) errors->push_back(".overheat/tasks.toml: " + err);
    }
    if (read_file(root / ".vscode" / "tasks.json", text)) {
        err.clear();
        if (!parse_vscode_tasks(text, out, &err) && errors) errors->push_back(".vscode/tasks.json: " + err);
    }
    std::set<std::string> labels;
    std::set<std::string> groups_with_default;
    for (auto& t : out) {
        labels.insert(t.label);
        if (t.is_default) groups_with_default.insert(t.group);
    }
    for (auto& t : detect_tasks(root)) {
        if (labels.count(t.label)) continue;
        if (groups_with_default.count(t.group)) t.is_default = false;  // the project's choice wins
        out.push_back(std::move(t));
    }
    return out;
}

const TaskDef* default_task(const std::vector<TaskDef>& tasks, const std::string& group) {
    for (auto& t : tasks)
        if (t.group == group && t.is_default) return &t;
    for (auto& t : tasks)
        if (t.group == group) return &t;
    return nullptr;
}

const TaskDef* find_task(const std::vector<TaskDef>& tasks, const std::string& label) {
    for (auto& t : tasks)
        if (t.label == label) return &t;
    return nullptr;
}

} // namespace oh
