#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

// Built-in themes plus user themes from <config>/themes/*.toml. A theme file may
// set `base = "<theme name>"` and override only some tokens. The active theme
// file is watched and re-applied when it changes on disk.
class ThemeManager {
public:
    void init(const fs::path& config_dir);

    std::vector<std::string> names() const;
    const std::string& current() const { return current_; }
    bool apply(const std::string& name, Theme& out);
    // Re-reads theme files whose mtime changed; re-applies if the active one did.
    bool poll_reload(Theme& out);

    // Persisted choice in <config>/settings.toml (`theme = "..."`).
    std::string saved_choice() const { return setting("theme"); }
    void save_choice(const std::string& name) const { set_setting("theme", "\"" + name + "\""); }
    // Raw `key = value` access to <config>/settings.toml (quotes stripped on read).
    std::string setting(const std::string& key) const;
    void set_setting(const std::string& key, const std::string& raw_value) const;
    void set_config_dir(const fs::path& dir) { config_dir_ = dir; }

    fs::path themes_dir() const { return config_dir_ / "themes"; }
    static std::string serialize(const Theme& t);

private:
    struct Entry {
        std::string name;
        fs::path file;  // empty for built-ins
        fs::file_time_type mtime{};
        std::string base;
        std::vector<std::pair<std::string, std::string>> overrides;  // token -> value
        Theme builtin;
    };
    std::vector<Entry> entries_;
    fs::path config_dir_;
    std::string current_;

    void scan_user_themes();
    bool parse_file(const fs::path& path, Entry& e) const;
    bool resolve(const std::string& name, Theme& out, int depth = 0) const;
    const Entry* find(const std::string& name) const;
};

} // namespace oh
