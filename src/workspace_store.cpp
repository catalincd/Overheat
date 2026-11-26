// Workspace files: a small TOML reader/writer for <config>/workspaces/<name>.toml.
// Only the subset the format uses: tables, arrays of tables, strings, numbers,
// booleans and (multi-line) arrays of strings.
#include "workspace.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace oh {

namespace {

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

struct Value {
    enum Kind { None, String, Number, Bool, Array } kind = None;
    std::string str;
    double num = 0;
    bool b = false;
    std::vector<Value> items;
    std::string raw;  // source text of the value
};

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
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
}

void skip_ws(std::string_view s, size_t& i) {
    for (;;) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i < s.size() && s[i] == '#') {  // comment inside a multi-line array
            while (i < s.size() && s[i] != '\n') i++;
            continue;
        }
        return;
    }
}

bool parse_value(std::string_view s, size_t& i, Value& v) {
    skip_ws(s, i);
    if (i >= s.size()) return false;
    size_t start = i;
    char c = s[i];
    if (c == '"') {
        v.kind = Value::String;
        for (i++; i < s.size() && s[i] != '"'; i++) {
            if (s[i] != '\\') { v.str += s[i]; continue; }
            if (++i >= s.size()) return false;
            switch (s[i]) {
                case 'n': v.str += '\n'; break;
                case 't': v.str += '\t'; break;
                case 'r': v.str += '\r'; break;
                case 'b': v.str += '\b'; break;
                case 'f': v.str += '\f'; break;
                case 'u': case 'U': {
                    size_t n = s[i] == 'u' ? 4 : 8;
                    if (i + n >= s.size()) return false;
                    uint32_t cp = static_cast<uint32_t>(std::strtoul(std::string(s.substr(i + 1, n)).c_str(), nullptr, 16));
                    append_utf8(v.str, cp);
                    i += n;
                    break;
                }
                default: v.str += s[i];
            }
        }
        if (i >= s.size()) return false;
        i++;
    } else if (c == '\'') {
        v.kind = Value::String;
        size_t end = s.find('\'', i + 1);
        if (end == std::string_view::npos) return false;
        v.str = std::string(s.substr(i + 1, end - i - 1));
        i = end + 1;
    } else if (c == '[') {
        v.kind = Value::Array;
        i++;
        for (;;) {
            skip_ws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ']') { i++; break; }
            Value item;
            if (!parse_value(s, i, item)) return false;
            v.items.push_back(std::move(item));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') i++;
        }
    } else {
        size_t end = i;
        while (end < s.size() && s[end] != ',' && s[end] != ']' && s[end] != '#' && s[end] != '\n') end++;
        std::string word = trim(s.substr(i, end - i));
        i = end;
        if (word == "true" || word == "false") {
            v.kind = Value::Bool;
            v.b = word == "true";
        } else {
            std::erase(word, '_');
            char* stop = nullptr;
            v.num = std::strtod(word.c_str(), &stop);
            if (word.empty() || (stop && *stop)) return false;
            v.kind = Value::Number;
        }
    }
    v.raw = trim(s.substr(start, i - start));
    return true;
}

// Bracket depth of a line, ignoring brackets inside strings and comments.
int bracket_balance(std::string_view line) {
    int depth = 0;
    char in = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (in) {
            if (c == '\\' && in == '"') i++;
            else if (c == in) in = 0;
        } else if (c == '"' || c == '\'') in = c;
        else if (c == '#') break;
        else if (c == '[') depth++;
        else if (c == ']') depth--;
    }
    return depth;
}

std::string key_of(std::string k) {
    k = trim(k);
    if (k.size() >= 2 && (k.front() == '"' || k.front() == '\'') && k.back() == k.front()) k = k.substr(1, k.size() - 2);
    return k;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (!h || !*h) h = std::getenv("USERPROFILE");
    return h ? h : "";
}

std::string normalize_folder(const std::string& f) {
    if (parse_ssh_folder(f)) {
        std::string s = f;
        while (s.size() > 1 && s.back() == '/' && s[s.size() - 2] != '/') s.pop_back();
        return s;
    }
    fs::path p = fs::path(f).lexically_normal();
    std::string s = p.generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

} // namespace

std::optional<std::pair<std::string, std::string>> parse_ssh_folder(const std::string& folder) {
    if (folder.rfind("ssh://", 0) != 0) return std::nullopt;
    std::string rest = folder.substr(6);
    size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (host.empty()) return std::nullopt;
    return std::make_pair(host, path);
}

std::string ssh_folder(const std::string& host, const fs::path& path) {
    std::string p = path.generic_string();
    if (p.empty() || p[0] != '/') p = "/" + p;
    return "ssh://" + host + p;
}

std::string pretty_folder(const std::string& folder) {
    if (auto ssh = parse_ssh_folder(folder)) return ssh->first + ":" + ssh->second;
    std::string home = home_dir();
    if (!home.empty() && folder.rfind(home, 0) == 0 && (folder.size() == home.size() || folder[home.size()] == '/'))
        return "~" + folder.substr(home.size());
    return folder;
}

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string time_ago(int64_t t) {
    if (t <= 0) return "never";
    int64_t d = std::max<int64_t>(0, unix_now() - t);
    auto n = [](int64_t v, const char* unit) { return std::to_string(v) + " " + unit + " ago"; };
    if (d < 60) return "just now";
    if (d < 3600) return n(d / 60, "min");
    if (d < 86400) return n(d / 3600, "h");
    if (d < 86400 * 2) return "yesterday";
    if (d < 86400 * 30) return n(d / 86400, "days");
    if (d < 86400 * 365) return n(d / (86400 * 30), d / (86400 * 30) == 1 ? "month" : "months");
    return n(d / (86400 * 365), d / (86400 * 365) == 1 ? "year" : "years");
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string WorkspaceStore::serialize(const Workspace& ws) {
    std::ostringstream out;
    out << "# Overheat workspace. Opened only when you pick it (Ctrl+R); never on startup.\n";
    out << "name = " << quote(ws.name) << "\n";
    out << "last_opened = " << ws.last_opened << "\n";
    if (!ws.source.empty()) out << "source = " << quote(ws.source.generic_string()) << "\n";
    out << "folders = [\n";
    for (auto& f : ws.folders) out << "  " << quote(f) << ",\n";
    out << "]\n\n";

    out << "[layout]\n";
    out << "split = " << (ws.split ? "true" : "false") << "\n";
    char ratio[32];
    std::snprintf(ratio, sizeof ratio, "%.3f", ws.split_ratio);
    out << "split_ratio = " << ratio << "\n";
    out << "active_group = " << ws.active_group << "\n";
    out << "sidebar = " << (ws.sidebar ? "true" : "false") << "\n";
    out << "sidebar_view = " << quote(ws.sidebar_view) << "\n";
    out << "terminal = " << (ws.terminal ? "true" : "false") << "\n";

    out << "\n# Overrides for settings.toml keys while this workspace is open, e.g.\n"
           "# font_size = 14, word_wrap = true, theme = \"Overheat Light\"\n";
    out << "[settings]\n";
    for (auto& [k, v] : ws.settings) out << k << " = " << v << "\n";

    for (auto& t : ws.tabs) {
        out << "\n[[tabs]]\n";
        out << "path = " << quote(t.path) << "\n";
        if (t.group) out << "group = " << t.group << "\n";
        out << "line = " << t.line << "\n";
        out << "col = " << t.col << "\n";
        char sy[32];
        std::snprintf(sy, sizeof sy, "%.1f", t.scroll_y);
        out << "scroll = " << sy << "\n";
        if (t.active) out << "active = true\n";
    }
    return out.str();
}

bool WorkspaceStore::parse(const std::string& text, Workspace& ws, std::string* error) {
    std::istringstream in(text);
    std::string table;
    WorkspaceTab* tab = nullptr;
    int lineno = 0;
    auto fail = [&](const std::string& msg) {
        if (error) *error = "line " + std::to_string(lineno) + ": " + msg;
        return false;
    };
    for (std::string line; std::getline(in, line);) {
        lineno++;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("[[", 0) == 0) {
            size_t end = t.find("]]");
            if (end == std::string::npos) return fail("unterminated [[table]]");
            table = trim(t.substr(2, end - 2));
            tab = nullptr;
            if (table == "tabs") {
                ws.tabs.emplace_back();
                tab = &ws.tabs.back();
            }
            continue;
        }
        if (t[0] == '[') {
            size_t end = t.find(']');
            if (end == std::string::npos) return fail("unterminated [table]");
            table = trim(t.substr(1, end - 1));
            tab = nullptr;
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        std::string key = key_of(t.substr(0, eq));
        std::string val = t.substr(eq + 1);
        // Multi-line arrays: keep reading until the brackets balance.
        int depth = bracket_balance(val);
        while (depth > 0) {
            std::string more;
            if (!std::getline(in, more)) return fail("unterminated array");
            lineno++;
            val += "\n" + more;
            depth += bracket_balance(more);
        }
        Value v;
        size_t pos = 0;
        if (!parse_value(val, pos, v)) return fail("bad value for " + key);

        auto as_int = [&](int64_t def) { return v.kind == Value::Number ? static_cast<int64_t>(v.num) : def; };
        auto as_bool = [&](bool def) { return v.kind == Value::Bool ? v.b : def; };
        if (table.empty()) {
            if (key == "name" && v.kind == Value::String) ws.name = v.str;
            else if (key == "last_opened") ws.last_opened = as_int(0);
            else if (key == "source" && v.kind == Value::String) ws.source = v.str;
            else if (key == "folders" && v.kind == Value::Array) {
                ws.folders.clear();
                for (auto& item : v.items)
                    if (item.kind == Value::String && !item.str.empty()) ws.folders.push_back(item.str);
            }
        } else if (table == "layout") {
            if (key == "split") ws.split = as_bool(false);
            else if (key == "split_ratio" && v.kind == Value::Number)
                ws.split_ratio = std::clamp(static_cast<float>(v.num), 0.15f, 0.85f);
            else if (key == "active_group") ws.active_group = as_int(0) ? 1 : 0;
            else if (key == "sidebar") ws.sidebar = as_bool(true);
            else if (key == "terminal") ws.terminal = as_bool(true);
            else if (key == "sidebar_view" && v.kind == Value::String) ws.sidebar_view = v.str;
        } else if (table == "settings") {
            ws.settings.emplace_back(key, v.raw);
        } else if (table == "tabs" && tab) {
            if (key == "path" && v.kind == Value::String) tab->path = v.str;
            else if (key == "group") tab->group = as_int(0) ? 1 : 0;
            else if (key == "line") tab->line = static_cast<size_t>(std::max<int64_t>(0, as_int(0)));
            else if (key == "col") tab->col = static_cast<size_t>(std::max<int64_t>(0, as_int(0)));
            else if (key == "scroll" && v.kind == Value::Number) tab->scroll_y = std::max(0.0f, static_cast<float>(v.num));
            else if (key == "active") tab->active = as_bool(false);
        }
        // Unknown keys and tables are ignored, so newer files still load.
    }
    std::erase_if(ws.tabs, [](const WorkspaceTab& t) { return t.path.empty(); });
    return true;
}

std::string WorkspaceStore::sanitize_name(const std::string& name) {
    std::string out;
    for (unsigned char c : trim(name)) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            out += '-';
        else
            out += static_cast<char>(c);
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(0, 1);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.size() > 100) out.resize(100);
    return out;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

fs::path WorkspaceStore::path_for(const std::string& name) const {
    return dir_ / (sanitize_name(name) + ".toml");
}

bool WorkspaceStore::exists(const std::string& name) const {
    return load(name).has_value();
}

static std::optional<Workspace> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    Workspace ws;
    std::string err;
    if (!WorkspaceStore::parse(ss.str(), ws, &err)) {
        std::fprintf(stderr, "workspace: %s: %s\n", p.string().c_str(), err.c_str());
        return std::nullopt;
    }
    if (ws.name.empty()) ws.name = p.stem().string();
    ws.file = p;
    return ws;
}

std::vector<Workspace> WorkspaceStore::list() const {
    std::vector<Workspace> out;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(dir_, ec)) {
        if (de.path().extension() != ".toml") continue;
        if (auto ws = read_file(de.path())) out.push_back(std::move(*ws));
    }
    std::sort(out.begin(), out.end(), [](const Workspace& a, const Workspace& b) {
        if (a.last_opened != b.last_opened) return a.last_opened > b.last_opened;
        return lower(a.name) < lower(b.name);
    });
    return out;
}

std::optional<Workspace> WorkspaceStore::load(const std::string& name) const {
    // Usually <name>.toml, but a hand-made file may carry a different `name`.
    if (auto ws = read_file(path_for(name)); ws && ws->name == name) return ws;
    for (auto& ws : list())
        if (ws.name == name) return ws;
    return std::nullopt;
}

bool WorkspaceStore::save(Workspace& ws) const {
    if (sanitize_name(ws.name).empty()) return false;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    // Keep writing to the file it came from; a new or renamed workspace gets <name>.toml.
    fs::path p = !ws.file.empty() && ws.file.parent_path() == dir_ ? ws.file : path_for(ws.name);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << serialize(ws);
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);  // atomic: a crash never leaves a half-written workspace
    if (ec) return false;
    ws.file = p;
    return true;
}

bool WorkspaceStore::remove(const std::string& name) const {
    auto ws = load(name);
    std::error_code ec;
    return ws && fs::remove(ws->file, ec);
}

std::vector<Workspace> WorkspaceStore::containing(const std::string& folder) const {
    std::string want = normalize_folder(folder);
    std::vector<Workspace> out;
    for (auto& ws : list())
        for (auto& f : ws.folders)
            if (normalize_folder(f) == want) {
                out.push_back(ws);
                break;
            }
    return out;
}

std::optional<Workspace> WorkspaceStore::load_project_file(const fs::path& file) {
    auto ws = read_file(file);
    if (!ws) return std::nullopt;
    fs::path base = file.parent_path();
    if (ws->folders.empty()) ws->folders.push_back(".");
    for (auto& f : ws->folders)
        if (!parse_ssh_folder(f) && fs::path(f).is_relative()) f = normalize_folder((base / f).string());
    if (ws->name.empty() || ws->name == file.stem().string()) ws->name = base.filename().string();
    ws->source = file;
    ws->tabs.clear();  // session state is personal; it lives in the config dir
    ws->file.clear();
    return ws;
}

} // namespace oh
