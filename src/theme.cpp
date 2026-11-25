#include "theme.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace oh {

namespace {

struct Token { const char* key; Color Theme::*member; };

const Token TOKENS[] = {
    {"editor.background", &Theme::bg},
    {"editor.foreground", &Theme::fg},
    {"editor.current_line", &Theme::current_line},
    {"editor.selection", &Theme::selection},
    {"editor.cursor", &Theme::cursor_color},
    {"gutter.background", &Theme::gutter_bg},
    {"gutter.line_number", &Theme::line_number},
    {"gutter.line_number_active", &Theme::line_number_active},
    {"sidebar.background", &Theme::sidebar_bg},
    {"sidebar.selection", &Theme::tree_selection},
    {"tabs.background", &Theme::tab_bg},
    {"tabs.active_background", &Theme::tab_active_bg},
    {"tabs.inactive_foreground", &Theme::tab_inactive_fg},
    {"status.background", &Theme::status_bg},
    {"status.foreground", &Theme::status_fg},
    {"terminal.background", &Theme::terminal_bg},
    {"terminal.foreground", &Theme::terminal_fg},
    {"ui.accent", &Theme::accent},
    {"ui.foreground_muted", &Theme::fg_muted},
    {"ui.surface", &Theme::surface},
    {"ui.surface_hover", &Theme::surface_hover},
    {"ui.input_background", &Theme::input_bg},
    {"ui.border", &Theme::border},
    {"ui.shadow", &Theme::shadow},
    {"ui.scrollbar", &Theme::scrollbar},
    {"syntax.comment", &Theme::comment},
    {"syntax.keyword", &Theme::keyword},
    {"syntax.string", &Theme::string_color},
    {"syntax.number", &Theme::number},
    {"syntax.function", &Theme::function_name},
    {"syntax.type", &Theme::type_color},
    {"syntax.operator", &Theme::operator_color},
    {"syntax.punctuation", &Theme::punctuation},
    {"syntax.property", &Theme::property},
    {"syntax.preprocessor", &Theme::preprocessor},
    {"diagnostic.error", &Theme::error},
    {"diagnostic.warning", &Theme::warning},
    {"diagnostic.info", &Theme::info},
    {"git.added", &Theme::git_added},
    {"git.modified", &Theme::git_modified},
    {"git.deleted", &Theme::git_deleted},
    {"git.untracked", &Theme::git_untracked},
    {"git.ignored", &Theme::git_ignored},
};

bool parse_color(const std::string& v, Color& out) {
    if (v.size() < 4 || v[0] != '#') return false;
    std::string h = v.substr(1);
    if (h.size() == 3) h = {h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() != 6 && h.size() != 8) return false;
    unsigned long n = 0;
    try { n = std::stoul(h, nullptr, 16); } catch (...) { return false; }
    if (h.size() == 6) out = Color::hex(static_cast<uint32_t>(n));
    else out = Color::hex(static_cast<uint32_t>(n >> 8), (n & 0xFF) / 255.0f);
    return true;
}

std::string format_color(const Color& c) {
    auto b = [](float f) { return static_cast<int>(f * 255.0f + 0.5f); };
    char buf[16];
    if (c.a >= 0.999f) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", b(c.r), b(c.g), b(c.b), b(c.a));
    return buf;
}

bool set_token(Theme& t, const std::string& key, const std::string& value) {
    Color c;
    if (!parse_color(value, c)) return false;
    for (auto& tok : TOKENS)
        if (key == tok.key) { t.*tok.member = c; return true; }
    if (key.rfind("terminal.ansi", 0) == 0) {
        int i = std::atoi(key.c_str() + 13);
        if (i >= 0 && i < 16 && key.size() > 13) { t.ansi[i] = c; return true; }
    }
    return false;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

Theme light_theme() {
    Theme t;
    t.name = "Overheat Light"; t.dark = false;
    t.bg = Color::hex(0xfbfbfc); t.sidebar_bg = Color::hex(0xf3f3f5); t.gutter_bg = t.bg;
    t.tab_bg = Color::hex(0xf3f3f5); t.tab_active_bg = t.bg; t.status_bg = Color::hex(0xf3f3f5);
    t.terminal_bg = Color::hex(0xf3f3f5); t.surface = Color::hex(0xffffff);
    t.surface_hover = Color::hex(0xe9eaee); t.input_bg = Color::hex(0xf3f3f5);
    t.border = Color::hex(0xe3e4e8); t.shadow = Color::hex(0x1a1c24, 0.16f);
    t.fg = Color::hex(0x24262b); t.fg_muted = Color::hex(0x6b6f78);
    t.line_number = Color::hex(0xb3b6bd); t.line_number_active = Color::hex(0x44474e);
    t.tab_inactive_fg = Color::hex(0x6b6f78); t.status_fg = Color::hex(0x6b6f78);
    t.terminal_fg = Color::hex(0x2c2f35);
    t.accent = Color::hex(0x2f6fec); t.selection = Color::hex(0x2f6fec, 0.20f);
    t.current_line = Color::hex(0xf3f4f7); t.cursor_color = t.accent;
    t.tree_selection = Color::hex(0x2f6fec, 0.12f); t.scrollbar = Color::hex(0x000000, 0.12f);
    t.comment = Color::hex(0x8b8f98); t.keyword = Color::hex(0x8a3ffc);
    t.string_color = Color::hex(0x2b8a3e); t.number = Color::hex(0xb5651d);
    t.function_name = Color::hex(0x1f5fd1); t.type_color = Color::hex(0x0e7c86);
    t.operator_color = Color::hex(0x555a63); t.punctuation = Color::hex(0x6b6f78);
    t.property = Color::hex(0x3a3d44); t.preprocessor = t.keyword;
    t.error = Color::hex(0xd93636); t.warning = Color::hex(0xb7791f); t.info = t.accent;
    t.git_added = Color::hex(0x2b8a3e); t.git_modified = Color::hex(0x2f6fec);
    t.git_deleted = Color::hex(0xd93636); t.git_untracked = t.git_added; t.git_ignored = Color::hex(0xb3b6bd);
    const uint32_t ansi[16] = {0x2c2f35, 0xc92a2a, 0x2b8a3e, 0xa86b00, 0x1f5fd1, 0x8a3ffc, 0x0b7285, 0xb3b6bd,
                               0x6b6f78, 0xe03131, 0x37b24d, 0xc78a00, 0x3b7cff, 0xa463ff, 0x1098ad, 0xf3f3f5};
    for (int i = 0; i < 16; i++) t.ansi[i] = Color::hex(ansi[i]);
    return t;
}

Theme high_contrast_theme() {
    Theme t;
    t.name = "High Contrast (debug)";
    t.bg = Color::hex(0x000000); t.fg = Color::hex(0xffffff);
    t.sidebar_bg = Color::hex(0x002266); t.gutter_bg = Color::hex(0x002266);
    t.tab_bg = Color::hex(0x550000); t.tab_active_bg = Color::hex(0xcc0000);
    t.status_bg = Color::hex(0x006600); t.terminal_bg = Color::hex(0x333300);
    t.surface = Color::hex(0x000000); t.surface_hover = Color::hex(0x333333);
    t.input_bg = Color::hex(0xcc0000); t.border = Color::hex(0xffffff);
    t.fg_muted = Color::hex(0xaaaaaa); t.line_number = Color::hex(0x00ffff);
    t.line_number_active = Color::hex(0xffffff); t.tab_inactive_fg = Color::hex(0xaaaaaa);
    t.status_fg = Color::hex(0xffffff); t.terminal_fg = Color::hex(0xffffff);
    t.accent = Color::hex(0xffff00); t.selection = Color::hex(0x0066ff, 0.65f);
    t.current_line = Color::hex(0x262626); t.cursor_color = Color::hex(0xffff00);
    t.tree_selection = Color::hex(0xcc0000);
    t.comment = Color::hex(0x999999); t.keyword = Color::hex(0xff00ff);
    t.string_color = Color::hex(0x00ff00); t.number = Color::hex(0xff8800);
    t.function_name = Color::hex(0x00bbff); t.type_color = Color::hex(0xffff00);
    t.operator_color = Color::hex(0xff4444); t.punctuation = Color::hex(0xff4444);
    t.property = Color::hex(0x00bbff); t.preprocessor = t.keyword;
    return t;
}

} // namespace

std::string ThemeManager::serialize(const Theme& t) {
    std::ostringstream o;
    o << "# Overheat theme. Colours are #rrggbb or #rrggbbaa.\n"
      << "# Copy this file, change `name`, and edit; or set `base` and override a few keys.\n"
      << "name = \"" << t.name << "\"\n"
      << "dark = " << (t.dark ? "true" : "false") << "\n";
    std::string section;
    for (auto& tok : TOKENS) {
        std::string key = tok.key;
        auto dot = key.find('.');
        std::string sec = key.substr(0, dot);
        if (sec != section) { o << "\n[" << sec << "]\n"; section = sec; }
        o << key.substr(dot + 1) << " = \"" << format_color(t.*tok.member) << "\"\n";
        if (key == "terminal.foreground")
            for (int i = 0; i < 16; i++) o << "ansi" << i << " = \"" << format_color(t.ansi[i]) << "\"\n";
    }
    return o.str();
}

void ThemeManager::init(const fs::path& config_dir) {
    config_dir_ = config_dir;
    entries_.clear();
    for (const Theme& t : {Theme{}, light_theme(), high_contrast_theme()}) {
        Entry e;
        e.name = t.name;
        e.builtin = t;
        entries_.push_back(std::move(e));
    }
    std::error_code ec;
    fs::create_directories(themes_dir(), ec);
    // Ship editable copies of the built-ins so users have a starting point.
    for (const char* stem : {"overheat-dark", "overheat-light"}) {
        fs::path p = themes_dir() / (std::string(stem) + ".example.toml");
        if (!fs::exists(p, ec)) {
            std::ofstream out(p);
            out << serialize(std::string(stem) == "overheat-dark" ? Theme{} : light_theme());
        }
    }
    scan_user_themes();
}

void ThemeManager::scan_user_themes() {
    std::error_code ec;
    for (auto& de : fs::directory_iterator(themes_dir(), ec)) {
        const fs::path& p = de.path();
        if (p.extension() != ".toml" || p.stem().extension() == ".example") continue;
        Entry e;
        if (!parse_file(p, e)) continue;
        e.file = p;
        e.mtime = fs::last_write_time(p, ec);
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& x) { return x.name == e.name; });
        if (it != entries_.end()) *it = std::move(e);  // user file overrides same-named theme
        else entries_.push_back(std::move(e));
    }
}

bool ThemeManager::parse_file(const fs::path& path, Entry& e) const {
    std::ifstream in(path);
    if (!in) return false;
    std::string section, line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        auto hash = line.find('#');
        // '#' starts a comment unless it is inside a quoted colour value
        if (hash != std::string::npos && line.find('"') > hash) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "theme %s:%d: expected key = value\n", path.c_str(), lineno);
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = unquote(line.substr(eq + 1));
        if (section.empty() && key == "name") e.name = value;
        else if (section.empty() && key == "base") e.base = value;
        else if (section.empty() && key == "dark") e.builtin.dark = (value == "true");
        else e.overrides.push_back({section.empty() ? key : section + "." + key, value});
    }
    if (e.name.empty()) e.name = path.stem().string();
    return true;
}

const ThemeManager::Entry* ThemeManager::find(const std::string& name) const {
    for (auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ThemeManager::resolve(const std::string& name, Theme& out, int depth) const {
    const Entry* e = find(name);
    if (!e || depth > 8) return false;
    if (e->file.empty()) {
        out = e->builtin;
        return true;
    }
    // User theme: start from its base (default: Overheat Dark), then apply overrides.
    if (e->base.empty() || e->base == e->name || !resolve(e->base, out, depth + 1)) out = Theme{};
    out.name = e->name;
    out.dark = e->builtin.dark;
    for (auto& [key, value] : e->overrides)
        if (!set_token(out, key, value))
            std::fprintf(stderr, "theme %s: unknown key or bad colour: %s = %s\n",
                         e->file.c_str(), key.c_str(), value.c_str());
    return true;
}

std::vector<std::string> ThemeManager::names() const {
    std::vector<std::string> out;
    for (auto& e : entries_) out.push_back(e.name);
    return out;
}

bool ThemeManager::apply(const std::string& name, Theme& out) {
    if (!resolve(name, out)) return false;
    current_ = name;
    return true;
}

bool ThemeManager::poll_reload(Theme& out) {
    bool changed = false;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(themes_dir(), ec)) {
        const fs::path& p = de.path();
        if (p.extension() != ".toml" || p.stem().extension() == ".example") continue;
        auto mtime = fs::last_write_time(p, ec);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.file == p; });
        if (it != entries_.end() && it->mtime == mtime) continue;
        changed = true;
    }
    if (!changed) return false;
    // Rebuild the user part of the list; keep built-ins.
    std::erase_if(entries_, [](const Entry& e) { return !e.file.empty(); });
    init(config_dir_);
    return resolve(current_, out);
}

std::string ThemeManager::saved_choice() const {
    std::ifstream in(config_dir_ / "settings.toml");
    for (std::string line; std::getline(in, line);) {
        auto eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == "theme")
            return unquote(line.substr(eq + 1));
    }
    return "";
}

void ThemeManager::save_choice(const std::string& name) const {
    fs::path p = config_dir_ / "settings.toml";
    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream in(p);
        for (std::string line; std::getline(in, line);) {
            auto eq = line.find('=');
            if (eq != std::string::npos && trim(line.substr(0, eq)) == "theme") {
                line = "theme = \"" + name + "\"";
                replaced = true;
            }
            lines.push_back(line);
        }
    }
    if (!replaced) lines.push_back("theme = \"" + name + "\"");
    std::ofstream out(p);
    for (auto& l : lines) out << l << "\n";
}

} // namespace oh
