#include "theme.h"
#include "theme_builtin.h"
#include <algorithm>
#include <cctype>
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
    {"editor.bracket_match", &Theme::bracket_match},
    {"editor.find_match", &Theme::find_match},
    {"editor.find_match_active", &Theme::find_match_active},
    {"editor.indent_guide", &Theme::indent_guide},
    {"gutter.background", &Theme::gutter_bg},
    {"gutter.line_number", &Theme::line_number},
    {"gutter.line_number_active", &Theme::line_number_active},
    {"sidebar.background", &Theme::sidebar_bg},
    {"sidebar.selection", &Theme::tree_selection},
    {"tabs.background", &Theme::tab_bg},
    {"tabs.active_background", &Theme::tab_active_bg},
    {"tabs.active_foreground", &Theme::tab_active_fg},
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
    {"syntax.keyword_control", &Theme::keyword_control},
    {"syntax.string", &Theme::string_color},
    {"syntax.number", &Theme::number},
    {"syntax.constant", &Theme::constant},
    {"syntax.function", &Theme::function_name},
    {"syntax.type", &Theme::type_color},
    {"syntax.type_builtin", &Theme::type_builtin},
    {"syntax.operator", &Theme::operator_color},
    {"syntax.punctuation", &Theme::punctuation},
    {"syntax.property", &Theme::property},
    {"syntax.variable", &Theme::variable},
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

// Tokens added after the first theme format. A theme that does not set one gets
// it from its parent (alpha < 0 keeps the parent's alpha, otherwise the parent's
// alpha is multiplied by it), but only when the parent itself was set in the same
// file; otherwise the value inherited from the base theme stands. So older user
// themes keep looking as they did, and a theme that only recolours keywords
// recolours control-flow keywords too.
struct Derived { Color Theme::*child; Color Theme::*parent; float alpha; };

const Derived DERIVED[] = {
    {&Theme::keyword_control, &Theme::keyword, -1.0f},
    {&Theme::constant, &Theme::keyword, -1.0f},
    {&Theme::type_builtin, &Theme::type_color, -1.0f},
    {&Theme::variable, &Theme::fg, -1.0f},
    {&Theme::tab_active_fg, &Theme::fg, -1.0f},
    {&Theme::bracket_match, &Theme::fg_muted, 0.6f},
    {&Theme::find_match, &Theme::warning, 0.22f},
    {&Theme::find_match_active, &Theme::warning, 0.5f},
    {&Theme::indent_guide, &Theme::line_number, 0.35f},
};

const char* token_key(Color Theme::*m) {
    for (auto& tok : TOKENS)
        if (tok.member == m) return tok.key;
    return "";
}

void derive(Theme& t, Color Theme::*child, Color Theme::*parent, float alpha) {
    t.*child = t.*parent;
    if (alpha >= 0) (t.*child).a *= alpha;
}

// Fills every derived token from its parent (used for themes defined in code).
void derive_all(Theme& t) {
    for (auto& d : DERIVED) derive(t, d.child, d.parent, d.alpha);
}

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
    derive_all(t);
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
    derive_all(t);
    return t;
}

} // namespace

std::string ThemeManager::serialize(const Theme& t) {
    std::ostringstream o;
    o << "# Overheat theme. Colours are #rrggbb or #rrggbbaa.\n"
      << "# Copy this file, change `name`, and edit; or set `base` to any theme name\n"
      << "# (e.g. base = \"Dracula\") and override a few keys. Keys left out of a file\n"
      << "# come from the base; keyword_control, constant, type_builtin and variable\n"
      << "# follow keyword / type / editor.foreground when only those are set.\n"
      << "# This file is regenerated by Overheat; edit a copy.\n"
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
    // Themes modelled on popular VS Code themes, written in the user theme format.
    for (const char* src : builtin_theme_sources()) {
        std::istringstream in(src);
        Entry e;
        parse_stream(in, "<built-in>", e);
        Theme t;
        if (const Entry* b = e.base.empty() ? nullptr : find(e.base)) {
            t = b->builtin;  // variants (e.g. Gruvbox Dark Hard) override a few keys
            if (!e.has_dark) e.builtin.dark = t.dark;
        } else if (e.has_dark && !e.builtin.dark) {
            t = light_theme();
        }
        t.name = e.name;
        t.dark = e.builtin.dark;
        apply_overrides(t, e.overrides, "built-in theme " + e.name);
        e.overrides.clear();
        e.builtin = t;
        entries_.push_back(std::move(e));
    }
    std::error_code ec;
    fs::create_directories(themes_dir(), ec);
    // Ship editable copies of the built-ins so users have a starting point. They
    // are rewritten when the format gains tokens; they are never loaded.
    for (const char* stem : {"overheat-dark", "overheat-light"}) {
        fs::path p = themes_dir() / (std::string(stem) + ".example.toml");
        std::string text = serialize(std::string(stem) == "overheat-dark" ? Theme{} : light_theme());
        std::ifstream old(p);
        std::string cur((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        if (cur != text) {
            std::ofstream out(p);
            out << text;
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
    parse_stream(in, path.string(), e);
    if (e.name.empty()) e.name = path.stem().string();
    return true;
}

void ThemeManager::parse_stream(std::istream& in, const std::string& label, Entry& e) {
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
            std::fprintf(stderr, "theme %s:%d: expected key = value\n", label.c_str(), lineno);
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = unquote(line.substr(eq + 1));
        if (section.empty() && key == "name") e.name = value;
        else if (section.empty() && key == "base") e.base = value;
        else if (section.empty() && key == "dark") { e.builtin.dark = (value == "true"); e.has_dark = true; }
        else e.overrides.push_back({section.empty() ? key : section + "." + key, value});
    }
}

void ThemeManager::apply_overrides(Theme& out, const std::vector<std::pair<std::string, std::string>>& overrides,
                                   const std::string& label) {
    for (auto& [key, value] : overrides)
        if (!set_token(out, key, value))
            std::fprintf(stderr, "theme %s: unknown key or bad colour: %s = %s\n",
                         label.c_str(), key.c_str(), value.c_str());
    auto has = [&](const char* key) {
        return std::any_of(overrides.begin(), overrides.end(), [&](auto& kv) { return kv.first == key; });
    };
    for (auto& d : DERIVED)
        if (!has(token_key(d.child)) && has(token_key(d.parent))) derive(out, d.child, d.parent, d.alpha);
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
    // User theme: start from its base (default: Overheat Dark, or Overheat Light
    // for `dark = false`), then apply overrides.
    bool based = !e->base.empty() && e->base != e->name && resolve(e->base, out, depth + 1);
    if (!based) out = (e->has_dark && !e->builtin.dark) ? light_theme() : Theme{};
    out.name = e->name;
    if (e->has_dark) out.dark = e->builtin.dark;
    apply_overrides(out, e->overrides, e->file.string());
    return true;
}

std::vector<std::string> ThemeManager::names() const {
    std::vector<std::string> out;
    for (auto& e : entries_) out.push_back(e.name);
    return out;
}

std::vector<ThemeManager::Info> ThemeManager::list() const {
    std::vector<Info> out;
    for (auto& e : entries_) {
        Theme t;
        if (!resolve(e.name, t)) continue;
        out.push_back({e.name, t.dark, !e.file.empty()});
    }
    // Dark themes first, then light; alphabetical within each group.
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Info& a, const Info& b) {
        if (a.dark != b.dark) return a.dark;
        return lower(a.name) < lower(b.name);
    });
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

std::string ThemeManager::setting(const std::string& key) const {
    std::ifstream in(config_dir_ / "settings.toml");
    for (std::string line; std::getline(in, line);) {
        auto eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == key) return unquote(line.substr(eq + 1));
    }
    return "";
}

void ThemeManager::set_setting(const std::string& key, const std::string& raw_value) const {
    fs::path p = config_dir_ / "settings.toml";
    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream in(p);
        for (std::string line; std::getline(in, line);) {
            auto eq = line.find('=');
            if (eq != std::string::npos && trim(line.substr(0, eq)) == key) {
                line = key + " = " + raw_value;
                replaced = true;
            }
            lines.push_back(line);
        }
    }
    if (!replaced) lines.push_back(key + " = " + raw_value);
    std::error_code ec;
    fs::create_directories(config_dir_, ec);
    std::ofstream out(p);
    for (auto& l : lines) out << l << "\n";
}

} // namespace oh
