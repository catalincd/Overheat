#include "debug/launch_config.h"
#include "debug/json.h"
#include "platform/platform.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace oh {

namespace {
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

const char* exe_suffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

// ---- a small TOML subset: tables of arrays, strings, numbers, booleans, arrays and
// inline tables, parsed into Json values.
struct Toml {
    std::string_view s;
    size_t i = 0;
    std::string error;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    // Whitespace, newlines and comments (inside arrays).
    void ws_nl() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i < s.size() && s[i] == '#') {
                while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            return;
        }
    }
    bool fail(const std::string& m) {
        if (error.empty()) error = m;
        return false;
    }
    bool key(std::string& out) {
        ws();
        if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
            Json v;
            if (!string(v)) return false;
            out = v.str();
            return true;
        }
        size_t start = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-' || s[i] == '.'))
            i++;
        out = std::string(s.substr(start, i - start));
        return !out.empty() || fail("expected a key");
    }
    bool string(Json& out) {
        char q = s[i++];
        std::string v;
        while (i < s.size() && s[i] != q) {
            if (s[i] == '\n') return fail("newline in string");
            if (q == '"' && s[i] == '\\' && i + 1 < s.size()) {
                char e = s[i + 1];
                i += 2;
                switch (e) {
                    case 'n': v += '\n'; break;
                    case 't': v += '\t'; break;
                    case 'r': v += '\r'; break;
                    case '"': v += '"'; break;
                    case '\\': v += '\\'; break;
                    default: v += '\\'; v += e; break;
                }
                continue;
            }
            v += s[i++];
        }
        if (i >= s.size()) return fail("unterminated string");
        i++;
        out = Json(v);
        return true;
    }
    bool value(Json& out) {
        ws();
        if (i >= s.size()) return fail("missing value");
        char c = s[i];
        if (c == '"' || c == '\'') return string(out);
        if (c == '[') {
            i++;
            out = Json::array();
            for (;;) {
                ws_nl();
                if (i < s.size() && s[i] == ']') { i++; return true; }
                Json v;
                if (!value(v)) return false;
                out.push(std::move(v));
                ws_nl();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '{') {
            i++;
            out = Json::object();
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '}') { i++; return true; }
                std::string k;
                if (!key(k)) return false;
                ws();
                if (i >= s.size() || s[i] != '=') return fail("expected '=' in inline table");
                i++;
                Json v;
                if (!value(v)) return false;
                out.set(k, std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        size_t start = i;
        while (i < s.size() && s[i] != '\n' && s[i] != '#' && s[i] != ',' && s[i] != ']' && s[i] != '}') i++;
        std::string tok = trim(s.substr(start, i - start));
        if (tok == "true") out = Json(true);
        else if (tok == "false") out = Json(false);
        else {
            char* end = nullptr;
            double d = std::strtod(tok.c_str(), &end);
            if (tok.empty() || !end || *end) return fail("bad value '" + tok + "'");
            out = Json(d);
        }
        return true;
    }
};

std::vector<std::string> string_list(const Json& v) {
    std::vector<std::string> out;
    if (v.is_string()) return split_command(v.str());
    for (auto& item : v.items())
        if (item.is_string()) out.push_back(item.str());
        else if (item.is_number()) out.push_back(item.dump());
    return out;
}

// Fills the fields shared by launch.toml and launch.json from a Json object.
void apply_common(LaunchConfig& c, const Json& o) {
    if (o["name"].is_string()) c.name = o["name"].str();
    if (o["program"].is_string()) c.program = o["program"].str();
    if (o.has("args")) c.args = string_list(o["args"]);
    if (o["cwd"].is_string()) c.cwd = o["cwd"].str();
    if (o["request"].is_string()) c.attach = o["request"].str() == "attach";
    for (const char* k : {"stopAtEntry", "stopOnEntry", "stop_at_entry"})
        if (o[k].is_bool()) c.stop_at_entry = o[k].boolean();
    for (const char* k : {"pid", "processId"}) {
        if (o[k].is_number()) c.pid = static_cast<long>(o[k].integer());
        else if (o[k].is_string()) c.pid = std::strtol(o[k].str().c_str(), nullptr, 10);  // "${command:pickProcess}" -> 0
    }
    // env: {K = V} / {"K": "V"}, [{name, value}] (cppdbg) or ["K=V"] (lldb-dap)
    for (const char* k : {"env", "environment"}) {
        const Json& e = o[k];
        for (auto& [name, val] : e.members()) c.env.emplace_back(name, val.is_string() ? val.str() : val.dump());
        for (auto& item : e.items()) {
            if (item.is_object()) c.env.emplace_back(item["name"].str(), item["value"].str());
            else if (item.is_string()) {
                auto eq = item.str().find('=');
                if (eq != std::string::npos) c.env.emplace_back(item.str().substr(0, eq), item.str().substr(eq + 1));
            }
        }
    }
    if (o["language"].is_string()) c.language = o["language"].str();
    for (auto& l : o["sourceLanguages"].items())
        if (l.str() == "rust") c.language = "rust";
}
} // namespace

const char* adapter_name(AdapterKind k) {
    switch (k) {
        case AdapterKind::Gdb: return "gdb";
        case AdapterKind::RustGdb: return "rust-gdb";
        case AdapterKind::LldbDap: return "lldb-dap";
        case AdapterKind::CodeLldb: return "codelldb";
    }
    return "gdb";
}

bool parse_adapter(const std::string& s, AdapterKind& out) {
    if (s == "gdb" || s == "cppdbg" || s == "cppvsdbg") out = AdapterKind::Gdb;
    else if (s == "rust-gdb") out = AdapterKind::RustGdb;
    else if (s == "lldb-dap" || s == "lldb-vscode") out = AdapterKind::LldbDap;
    else if (s == "lldb" || s == "codelldb") out = AdapterKind::CodeLldb;
    else return false;
    return true;
}

std::vector<std::string> split_command(const std::string& cmd) {
    std::vector<std::string> argv;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : cmd) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if ((c == ' ' || c == '\t') && !quoted) {
            if (any) argv.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) argv.push_back(cur);
    return argv;
}

std::string substitute_vars(const std::string& s, const LaunchVars& v) {
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
        if (name == "workspaceFolder" || name == "workspaceRoot") val = v.workspace.string();
        else if (name == "workspaceFolderBasename") val = v.workspace.filename().string();
        else if (name == "userHome") val = platform::home_dir().string();
        else if (name == "file") val = v.file.string();
        else if (name == "fileDirname") val = v.file.parent_path().string();
        else if (name == "fileBasename") val = v.file.filename().string();
        else if (name == "fileBasenameNoExtension") val = v.file.stem().string();
        else if (name == "relativeFile") val = v.file.lexically_relative(v.workspace).string();
        else if (name == "pathSeparator") val = std::string(1, static_cast<char>(fs::path::preferred_separator));
        else if (name.rfind("env:", 0) == 0) val = platform::getenv_str(name.substr(4).c_str());
        else if (name.rfind("command:", 0) == 0) val = "";  // pickProcess etc.: asked for later
        else known = false;
        out += known ? val : s.substr(i, end - i + 1);
        i = end + 1;
    }
    return out;
}

void resolve_config(LaunchConfig& c, const LaunchVars& v) {
    auto sub = [&](std::string& s) { s = substitute_vars(s, v); };
    sub(c.program);
    sub(c.cwd);
    sub(c.build_command);
    sub(c.build_cwd);
    sub(c.adapter_path);
    for (auto& a : c.args) sub(a);
    for (auto& [k, val] : c.env) sub(val);
    auto absolute = [&](std::string& p) {
        if (p.empty()) return;
        fs::path fp(p);
        if (!fp.is_absolute()) fp = v.workspace / fp;
        p = fp.lexically_normal().string();
    };
    absolute(c.program);
    if (c.cwd.empty()) c.cwd = v.workspace.string();
    absolute(c.cwd);
    if (c.build_cwd.empty()) c.build_cwd = v.workspace.string();
    absolute(c.build_cwd);
#ifdef _WIN32
    if (!c.program.empty() && !fs::exists(c.program) && fs::exists(c.program + ".exe")) c.program += ".exe";
#endif
    if (c.language.empty()) {
        std::string g = fs::path(c.program).generic_string();
        if (g.find("/target/debug/") != std::string::npos || g.find("/target/release/") != std::string::npos)
            c.language = "rust";
    }
    if (c.language == "rust" && c.adapter == AdapterKind::Gdb) c.adapter = AdapterKind::RustGdb;
}

bool parse_launch_toml(const std::string& text, std::vector<LaunchConfig>& out, std::string* error) {
    Toml t{text, 0, {}};
    Json cur;  // current [[configurations]] table
    bool in_config = false;
    int line_no = 1;
    auto flush = [&] {
        if (!in_config) return;
        LaunchConfig c;
        c.origin = "launch.toml";
        apply_common(c, cur);
        for (const char* k : {"adapter", "type"})
            if (cur[k].is_string() && !parse_adapter(cur[k].str(), c.adapter)) c.adapter_path = cur[k].str();
        for (const char* k : {"adapter_path", "debugger_path"})
            if (cur[k].is_string()) c.adapter_path = cur[k].str();
        for (const char* k : {"build", "build_command"})
            if (cur[k].is_string()) c.build_command = cur[k].str();
        if (cur["build_cwd"].is_string()) c.build_cwd = cur["build_cwd"].str();
        if (c.name.empty()) c.name = c.program.empty() ? "Configuration " + std::to_string(out.size() + 1)
                                                        : fs::path(c.program).filename().string();
        out.push_back(std::move(c));
        cur = Json::object();
    };
    while (t.i < text.size()) {
        t.ws();
        if (t.i >= text.size()) break;
        char c = text[t.i];
        if (c == '\n' || c == '\r') {
            if (c == '\n') line_no++;
            t.i++;
            continue;
        }
        if (c == '#') {
            while (t.i < text.size() && text[t.i] != '\n') t.i++;
            continue;
        }
        if (c == '[') {
            size_t end = text.find('\n', t.i);
            std::string header = trim(std::string_view(text).substr(t.i, end == std::string::npos ? std::string::npos : end - t.i));
            if (auto h = header.find('#'); h != std::string::npos) header = trim(header.substr(0, h));
            flush();
            if (header == "[[configurations]]" || header == "[[configuration]]") {
                in_config = true;
                cur = Json::object();
            } else {
                in_config = false;  // other tables are ignored
            }
            t.i = end == std::string::npos ? text.size() : end;
            continue;
        }
        std::string k;
        if (!t.key(k)) break;
        t.ws();
        if (t.i >= text.size() || text[t.i] != '=') {
            t.fail("expected '=' after " + k);
            break;
        }
        t.i++;
        Json v;
        size_t before = t.i;
        if (!t.value(v)) break;
        line_no += static_cast<int>(std::count(text.begin() + static_cast<ptrdiff_t>(before),
                                               text.begin() + static_cast<ptrdiff_t>(t.i), '\n'));
        if (in_config) cur.set(k, std::move(v));
        t.ws();
        if (t.i < text.size() && text[t.i] == '#')
            while (t.i < text.size() && text[t.i] != '\n') t.i++;
        if (t.i < text.size() && text[t.i] != '\n' && text[t.i] != '\r') {
            t.fail("unexpected text after value of " + k);
            break;
        }
    }
    if (!t.error.empty()) {
        if (error) *error = "launch.toml line " + std::to_string(line_no) + ": " + t.error;
        return false;
    }
    flush();
    return true;
}

bool parse_vscode_launch(const std::string& text, std::vector<LaunchConfig>& out, std::string* error) {
    Json root;
    std::string err;
    if (!Json::parse(text, root, &err, true)) {
        if (error) *error = "launch.json: " + err;
        return false;
    }
    for (const Json& o : root["configurations"].items()) {
        LaunchConfig c;
        c.origin = "launch.json";
        const std::string& type = o["type"].str();
        if (type == "cppdbg") {
            c.adapter = o["MIMode"].str() == "lldb" ? AdapterKind::LldbDap : AdapterKind::Gdb;
            if (c.adapter == AdapterKind::Gdb && o["miDebuggerPath"].is_string())
                c.adapter_path = o["miDebuggerPath"].str();
        } else if (type == "gdb") {
            c.adapter = AdapterKind::Gdb;  // gdb's own DAP type, or Native Debug (target/arguments)
            if (o["target"].is_string()) c.program = o["target"].str();
            if (o["arguments"].is_string()) c.args = split_command(o["arguments"].str());
            if (o["gdbpath"].is_string()) c.adapter_path = o["gdbpath"].str();
        } else if (!parse_adapter(type, c.adapter)) {
            continue;  // not a native debugger configuration (node, python, ...)
        }
        apply_common(c, o);
        if (o["preLaunchTask"].is_string()) c.pre_launch_task = o["preLaunchTask"].str();
        // CodeLLDB "cargo": {"args": ["build", "--bin=x"]}: build with cargo first.
        if (o["cargo"].is_object()) {
            std::string cmd = "cargo";
            for (auto& a : o["cargo"]["args"].items()) cmd += " " + a.str();
            c.build_command = cmd;
            c.language = "rust";
        }
        if (c.name.empty()) c.name = type;
        out.push_back(std::move(c));
    }
    return true;
}

std::vector<LaunchConfig> cargo_configs_from_metadata(const Json& md) {
    std::vector<LaunchConfig> out;
    fs::path target_dir = md["target_directory"].str();
    std::string ws_root = md["workspace_root"].str();
    for (const Json& pkg : md["packages"].items()) {
        for (const Json& t : pkg["targets"].items()) {
            bool bin = false;
            for (auto& k : t["kind"].items()) bin |= k.str() == "bin";
            if (!bin) continue;
            LaunchConfig c;
            c.origin = "Cargo";
            c.name = t["name"].str() + " (Cargo)";
            c.adapter = AdapterKind::RustGdb;
            c.language = "rust";
            c.program = (target_dir / "debug" / (t["name"].str() + exe_suffix())).string();
            c.build_command = "cargo build -p " + pkg["name"].str() + " --bin " + t["name"].str();
            c.build_cwd = ws_root;
            c.cwd = ws_root;
            out.push_back(std::move(c));
        }
    }
    return out;
}

std::vector<LaunchConfig> cargo_configs_from_manifest(const std::string& toml, const fs::path& root) {
    // Fallback without cargo: [package] name and [[bin]] names.
    std::vector<LaunchConfig> out;
    std::string section;
    std::vector<std::string> names;
    std::istringstream in(toml);
    for (std::string line; std::getline(in, line);) {
        std::string l = trim(line);
        if (!l.empty() && l[0] == '[') {
            section = l;
            continue;
        }
        if ((section == "[package]" || section == "[[bin]]") && l.rfind("name", 0) == 0) {
            auto q1 = l.find('"'), q2 = l.rfind('"');
            if (q1 != std::string::npos && q2 > q1) {
                std::string n = l.substr(q1 + 1, q2 - q1 - 1);
                if (section == "[[bin]]" || std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
            }
        }
    }
    for (auto& n : names) {
        LaunchConfig c;
        c.origin = "Cargo";
        c.name = n + " (Cargo)";
        c.adapter = AdapterKind::RustGdb;
        c.language = "rust";
        c.program = (root / "target" / "debug" / (n + exe_suffix())).string();
        c.build_command = "cargo build --bin " + n;
        c.build_cwd = root.string();
        c.cwd = root.string();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<fs::path> cmake_build_dirs(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    auto consider = [&](const fs::path& d) {
        if (fs::exists(d / "CMakeCache.txt", ec)) out.push_back(d);
    };
    consider(root);
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string n = it->path().filename().string();
        if (n.rfind("build", 0) == 0 || n.rfind("cmake-build", 0) == 0 || n == "out") consider(it->path());
        if (n == "out" || n == "build") {  // out/build/<preset>, build/<config>
            for (auto sub = fs::directory_iterator(it->path(), ec); !ec && sub != fs::directory_iterator(); sub.increment(ec))
                if (sub->is_directory(ec)) consider(sub->path());
            if (n == "out" && fs::exists(it->path() / "build", ec))
                for (auto sub = fs::directory_iterator(it->path() / "build", ec);
                     !ec && sub != fs::directory_iterator(); sub.increment(ec))
                    if (sub->is_directory(ec)) consider(sub->path());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<LaunchConfig> cmake_configs(const fs::path& root, const fs::path& bd) {
    std::vector<std::pair<std::string, fs::path>> exes;  // target name, artifact path
    std::error_code ec;
    // 1. CMake file API reply (written when an IDE queried it before).
    fs::path reply = bd / ".cmake" / "api" / "v1" / "reply";
    if (fs::exists(reply, ec)) {
        fs::path index;
        for (auto it = fs::directory_iterator(reply, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (n.rfind("index-", 0) == 0 && (index.empty() || it->path() > index)) index = it->path();
        }
        Json idx;
        if (!index.empty() && Json::parse(read_file(index), idx)) {
            for (auto& obj : idx["objects"].items()) {
                if (obj["kind"].str() != "codemodel") continue;
                Json cm;
                if (!Json::parse(read_file(reply / obj["jsonFile"].str()), cm)) continue;
                const Json& conf = cm["configurations"][0];
                for (auto& t : conf["targets"].items()) {
                    Json tj;
                    if (!Json::parse(read_file(reply / t["jsonFile"].str()), tj)) continue;
                    if (tj["type"].str() != "EXECUTABLE") continue;
                    const std::string& art = tj["artifacts"][0]["path"].str();
                    if (!art.empty()) exes.emplace_back(tj["name"].str(), bd / art);
                }
            }
        }
    }
    // 2. Ninja: "build <out>: CXX_EXECUTABLE_LINKER__<target>_<config> ..."
    if (exes.empty() && fs::exists(bd / "build.ninja", ec)) {
        std::istringstream in(read_file(bd / "build.ninja"));
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("build ", 0) != 0) continue;
            size_t rule = line.find("_EXECUTABLE_LINKER__");
            if (rule == std::string::npos) continue;
            size_t colon = line.find(": ");
            if (colon == std::string::npos || colon > rule) continue;
            std::string out_path = line.substr(6, colon - 6);
            std::string unesc;
            for (size_t k = 0; k < out_path.size(); k++) {
                if (out_path[k] == '$' && k + 1 < out_path.size()) k++;
                unesc += out_path[k];
            }
            fs::path p = fs::path(unesc).is_absolute() ? fs::path(unesc) : bd / unesc;
            exes.emplace_back(p.stem().string(), p);
        }
    }
    // 3. Other generators: files named like add_executable() targets.
    if (exes.empty()) {
        std::vector<std::string> targets;
        std::regex add_exe(R"(add_executable\s*\(\s*([A-Za-z0-9_.+-]+))", std::regex::icase);
        std::function<void(const fs::path&, int)> scan = [&](const fs::path& dir, int depth) {
            std::string text = read_file(dir / "CMakeLists.txt");
            for (std::sregex_iterator it(text.begin(), text.end(), add_exe), end; it != end; ++it)
                targets.push_back((*it)[1]);
            if (depth >= 3) return;
            for (auto e = fs::directory_iterator(dir, ec); !ec && e != fs::directory_iterator(); e.increment(ec)) {
                std::string n = e->path().filename().string();
                if (e->is_directory(ec) && n[0] != '.' && e->path() != bd && n.rfind("build", 0) != 0 &&
                    fs::exists(e->path() / "CMakeLists.txt", ec))
                    scan(e->path(), depth + 1);
            }
        };
        scan(root, 0);
        for (auto it = fs::recursive_directory_iterator(bd, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 3 || it->path().filename() == "CMakeFiles") {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string stem = it->path().stem().string(), ext = it->path().extension().string();
            if ((ext.empty() || ext == ".exe") && std::find(targets.begin(), targets.end(), stem) != targets.end())
                exes.emplace_back(stem, it->path());
        }
    }
    std::vector<LaunchConfig> out;
    std::string bd_label = bd == root ? "." : bd.lexically_relative(root).generic_string();
    for (auto& [name, path] : exes) {
        if (std::any_of(out.begin(), out.end(), [&](auto& c) { return c.program == path.string(); })) continue;
        LaunchConfig c;
        c.origin = "CMake";
        c.name = name + " (CMake: " + bd_label + ")";
        c.program = path.string();
        c.cwd = root.string();
        c.build_command = "cmake --build \"" + bd.string() + "\" --target " + name;
        c.build_cwd = root.string();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<LaunchConfig> discover_launch_configs(const fs::path& root, std::vector<std::string>* problems) {
    std::vector<LaunchConfig> out;
    std::error_code ec;
    std::string err;
    if (fs::path p = root / ".overheat" / "launch.toml"; fs::exists(p, ec)) {
        if (!parse_launch_toml(read_file(p), out, &err) && problems) problems->push_back(err);
    }
    if (fs::path p = root / ".vscode" / "launch.json"; fs::exists(p, ec)) {
        err.clear();
        if (!parse_vscode_launch(read_file(p), out, &err) && problems) problems->push_back(err);
    }
    if (fs::exists(root / "Cargo.toml", ec)) {
        std::vector<LaunchConfig> cargo;
        platform::Process proc;
        if (proc.start({"cargo", "metadata", "--no-deps", "--offline", "--format-version", "1"}, root)) {
            std::string json;
            char buf[8192];
            long n;
            while ((n = proc.read(buf, sizeof buf)) > 0) json.append(buf, static_cast<size_t>(n));
            Json md;
            if (proc.wait() == 0 && Json::parse(json, md)) cargo = cargo_configs_from_metadata(md);
        }
        if (cargo.empty()) cargo = cargo_configs_from_manifest(read_file(root / "Cargo.toml"), root);
        for (auto& c : cargo) out.push_back(std::move(c));
    }
    if (fs::exists(root / "CMakeLists.txt", ec)) {
        auto dirs = cmake_build_dirs(root);
        for (auto& bd : dirs)
            for (auto& c : cmake_configs(root, bd)) out.push_back(std::move(c));
        if (dirs.empty() && problems)
            problems->push_back("CMake project without a configured build directory (run cmake -B build first)");
    }
    return out;
}

std::string launch_toml_template(const std::vector<LaunchConfig>& detected) {
    std::string s =
        "# Overheat launch configurations (F5 starts the selected one).\n"
        "# Variables: ${workspaceFolder}, ${workspaceFolderBasename}, ${env:NAME}, ${file}, ${userHome}\n"
        "# adapter: gdb (default), rust-gdb (gdb + Rust pretty printers), lldb-dap, codelldb\n\n";
    auto q = [](const std::string& v) { return "\"" + json_escape(v) + "\""; };
    bool any = false;
    for (auto& c : detected) {
        if (c.origin != "Cargo" && c.origin != "CMake") continue;
        any = true;
        s += "[[configurations]]\n";
        s += "name = " + q(c.name) + "\n";
        s += "program = " + q(c.program) + "\n";
        s += "args = []\n";
        s += "cwd = \"${workspaceFolder}\"\n";
        s += "env = {}\n";
        s += "stopAtEntry = false\n";
        s += "adapter = " + q(adapter_name(c.adapter)) + "\n";
        if (!c.build_command.empty()) s += "build = " + q(c.build_command) + "\n";
        s += "\n";
    }
    if (!any)
        s += "[[configurations]]\n"
             "name = \"Debug program\"\n"
             "program = \"${workspaceFolder}/a.out\"\n"
             "args = []\n"
             "cwd = \"${workspaceFolder}\"\n"
             "env = {}\n"
             "stopAtEntry = false\n"
             "adapter = \"gdb\"\n"
             "# build = \"make\"\n\n"
             "# [[configurations]]\n"
             "# name = \"Attach\"\n"
             "# request = \"attach\"\n"
             "# program = \"${workspaceFolder}/a.out\"\n"
             "# pid = 0   # 0 asks for a process id\n";
    return s;
}

} // namespace oh
