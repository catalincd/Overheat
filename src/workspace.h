#pragma once
// Workspaces: a named, saved set of root folders plus the session that goes with
// them (tabs, split layout, panels) and optional settings overrides.
//
// Stored as TOML in <config>/workspaces/<name>.toml. A project may also carry a
// `.overheat-workspace` file (same format, folders relative to the file); it
// defines folders and settings, while its session state lives in the config dir.
// Nothing here opens anything by itself: the App decides when to load a workspace.
#include "types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oh {

struct WorkspaceTab {
    std::string path;  // relative to the primary folder when inside it, else absolute
    int group = 0;     // 0 = left editor group, 1 = right
    size_t line = 0, col = 0;
    float scroll_y = 0;
    bool active = false;  // active tab of its group
};

struct Workspace {
    std::string name;
    // Root folders, primary first: "/abs/path" or "ssh://<host>/<abs path>".
    std::vector<std::string> folders;
    std::vector<WorkspaceTab> tabs;
    bool split = false;
    float split_ratio = 0.5f;
    int active_group = 0;
    bool sidebar = true;
    bool terminal = true;
    std::string sidebar_view = "explorer";  // explorer | search | scm | chat
    // Raw settings.toml-style overrides (key -> TOML value text), applied while open.
    std::vector<std::pair<std::string, std::string>> settings;
    int64_t last_opened = 0;  // unix seconds
    fs::path source;          // a project's .overheat-workspace this one comes from (if any)
    fs::path file;            // where it was loaded from / will be saved to
};

// "ssh://host/abs/path" -> {host, "/abs/path"}; nullopt for a local folder.
std::optional<std::pair<std::string, std::string>> parse_ssh_folder(const std::string& folder);
std::string ssh_folder(const std::string& host, const fs::path& path);
// "~/x" for paths under $HOME, ssh folders as "host:/path".
std::string pretty_folder(const std::string& folder);
// "just now", "5 min ago", "3 h ago", "2 days ago", ...
std::string time_ago(int64_t unix_seconds);
int64_t unix_now();

class WorkspaceStore {
public:
    static constexpr const char* PROJECT_FILE = ".overheat-workspace";

    void set_config_dir(const fs::path& dir) { dir_ = dir / "workspaces"; }
    const fs::path& dir() const { return dir_; }

    // All saved workspaces, most recently opened first.
    std::vector<Workspace> list() const;
    std::optional<Workspace> load(const std::string& name) const;
    bool save(Workspace& ws) const;  // sets ws.file
    bool remove(const std::string& name) const;
    bool exists(const std::string& name) const;
    fs::path path_for(const std::string& name) const;

    // Saved workspaces that contain `folder` (local path or ssh:// form) as a root.
    std::vector<Workspace> containing(const std::string& folder) const;

    // A project's .overheat-workspace, folders resolved against its directory.
    static std::optional<Workspace> load_project_file(const fs::path& file);

    // TOML (de)serialization, exposed for tests.
    static std::string serialize(const Workspace& ws);
    static bool parse(const std::string& text, Workspace& out, std::string* error = nullptr);
    // A file-name-safe version of a workspace name ("" if nothing usable is left).
    static std::string sanitize_name(const std::string& name);

private:
    fs::path dir_;
};

} // namespace oh
