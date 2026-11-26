// Workspaces: the switcher, open/save/close flows, session capture and restore,
// per-workspace settings, the "this folder is in a workspace" hint and the
// welcome screen. A workspace is only ever opened because the user picked it.
#include "app.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>

namespace oh {

namespace {

const char* view_name(int v) {
    static const char* names[] = {"explorer", "search", "scm", "chat", "debug"};
    return names[std::clamp(v, 0, 4)];
}

int view_index(const std::string& name) {
    for (int i = 0; i < 5; i++)
        if (name == view_name(i)) return i;
    return 0;
}

std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
}

size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Cut `s` to at most `max_chars` code points, ending in an ellipsis when shortened.
std::string fit(const std::string& s, float max_w, float char_w) {
    size_t max_chars = max_w <= 0 ? 0 : static_cast<size_t>(max_w / std::max(1.0f, char_w));
    if (utf8_len(s) <= max_chars) return s;
    if (max_chars == 0) return "";
    std::string out;
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80 && ++n > max_chars - 1) break;
        out += s[i];
    }
    return out + "\xE2\x80\xA6";
}

fs::path expand_home(const std::string& text) {
    if (text == "~" || text.rfind("~/", 0) == 0) return platform::home_dir() / text.substr(std::min<size_t>(2, text.size()));
    return fs::path(text);
}

std::string folders_summary(const Workspace& ws) {
    if (ws.folders.empty()) return "";
    std::string s = pretty_folder(ws.folders.front());
    if (ws.folders.size() > 1) s += "  +" + std::to_string(ws.folders.size() - 1);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup and helpers
// ---------------------------------------------------------------------------

void App::setup_workspaces() {
    workspaces_.set_config_dir(config_dir_);
    update_workspace_hint(true);
}

std::string App::folder_uri(const fs::path& p) const {
    std::string s = p.lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return remote_active_ ? ssh_folder(remote_target_.alias, s) : s;
}

void App::update_window_title() {
    std::string base = project_root_.filename().string();
    if (base.empty()) base = project_root_.string();
    std::string title = workspace_ ? workspace_->name + " (Workspace) - Overheat" : base + " - Overheat";
    if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

void App::update_workspace_hint(bool announce) {
    recent_stale_ = true;
    hint_workspace_.reset();
    if (workspace_) return;
    std::string here = folder_uri(project_root_);
    auto matches = workspaces_.containing(here);
    if (!matches.empty()) {
        hint_workspace_ = matches.front();  // the most recently used one
    } else if (!remote_active_) {
        std::error_code ec;
        fs::path pf = project_root_ / WorkspaceStore::PROJECT_FILE;
        if (fs::exists(pf, ec))
            if (auto ws = WorkspaceStore::load_project_file(pf)) hint_workspace_ = std::move(*ws);
    }
    // Only a hint: nothing is opened until the user asks.
    if (announce && hint_workspace_)
        set_status_message("This folder is part of workspace \"" + hint_workspace_->name + "\"  (Ctrl+R to open it)",
                           12.0f);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void App::workspace_command(const std::string& cmd) {
    if (cmd == "workspace_switch") {
        workspace_switcher();
    } else if (cmd == "workspace_save_as") {
        save_workspace_as();
    } else if (cmd == "workspace_add_folder") {
        add_folder_to_workspace();
    } else if (cmd == "workspace_close") {
        if (!workspace_ && extra_roots_.empty()) {
            set_status_message("No workspace is open");
            return;
        }
        confirm_discard("Close Workspace", [this] { close_workspace(false); });
    } else if (cmd == "workspace_delete") {
        delete_workspace();
    } else if (cmd == "workspace_edit") {
        if (!workspace_) {
            set_status_message("No workspace is open");
            return;
        }
        autosave_workspace();
        if (vfs_->is_remote()) {
            set_status_message("Workspace file: " + workspace_->file.string(), 8.0f);
            return;
        }
        open_file(workspace_->source.empty() ? workspace_->file : workspace_->source);
        set_status_message("Edit [settings]; changes apply the next time the workspace opens", 8.0f);
    } else if (cmd == "open_folder") {
        open_folder_prompt();
    }
}

void App::workspace_switcher() {
    std::vector<Workspace> list = workspaces_.list();
    // A project's .overheat-workspace that was never opened shows up as well.
    if (hint_workspace_ && !hint_workspace_->source.empty() &&
        std::none_of(list.begin(), list.end(), [&](const Workspace& w) { return w.source == hint_workspace_->source; }))
        list.push_back(*hint_workspace_);

    std::vector<std::string> items;
    int initial = 0;
    for (size_t i = 0; i < list.size(); i++) {
        const Workspace& w = list[i];
        bool current = workspace_ && w.name == workspace_->name;
        bool here = hint_workspace_ && w.name == hint_workspace_->name;
        std::string tag = current ? "  (current)" : here ? "  (this folder)" : "";
        std::string when = w.last_opened ? time_ago(w.last_opened) : w.source.empty() ? "" : ".overheat-workspace";
        items.push_back(w.name + tag + "   " + folders_summary(w) + (when.empty() ? "" : "   " + when));
        if (here && !workspace_) initial = static_cast<int>(i);
    }
    // Ctrl+R, Enter flips between the two most recent workspaces.
    if (workspace_ && list.size() > 1 && list.front().name == workspace_->name) initial = 1;

    CommandPalette::PickOptions opts;
    opts.placeholder = list.empty() ? "No saved workspaces yet: type a name to save this folder as one"
                                    : "Open workspace (recent first; type to filter)";
    opts.items = items;
    opts.initial = initial;
    opts.allow_free_text = list.empty();
    opts.on_pick = [this, list](int i, const std::string& query) {
        if (i < 0) {
            if (!query.empty()) save_workspace_as(query);
            return;
        }
        const Workspace& w = list[static_cast<size_t>(i)];
        if (workspace_ && w.name == workspace_->name) {
            set_status_message("Workspace \"" + w.name + "\" is already open");
            return;
        }
        open_workspace(w);
    };
    command_palette_.pick(std::move(opts));
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

void App::open_workspace(Workspace ws) {
    // Folders and settings of a project-file workspace come from the project file.
    std::error_code ec;
    if (!ws.source.empty() && fs::exists(ws.source, ec))
        if (auto pf = WorkspaceStore::load_project_file(ws.source)) {
            ws.folders = pf->folders;
            ws.settings = pf->settings;
        }
    if (ws.folders.empty()) {
        set_status_message("Workspace \"" + ws.name + "\" has no folders", 6.0f);
        return;
    }
    confirm_discard("Open Workspace", [this, ws] {
        const std::string& primary = ws.folders.front();
        auto ssh = parse_ssh_folder(primary);
        std::error_code ec;
        if (!ssh && !fs::is_directory(primary, ec)) {
            set_status_message("Folder not found: " + primary, 8.0f);
            return;
        }
        // The workspace being left keeps its session; so does a plain folder.
        autosave_workspace();
        if (!workspace_) save_session();
        revert_workspace_settings();
        workspace_.reset();
        extra_roots_.clear();
        if (ssh) {
            if (remote_active_ && ssh_.is_connected() && remote_target_.alias == ssh->first) {
                open_remote_folder(fs::path(ssh->second));
                finish_open_workspace(ws);
            } else {
                // Connect first; poll_remote_connect() finishes the job.
                ssh_config_.load(SshConfig::default_path());
                pending_workspace_ = ws;
                remote_connect(ssh->first, false, "");
            }
            return;
        }
        switch_local_root(fs::path(primary));
        finish_open_workspace(ws);
    });
}

void App::finish_open_workspace(Workspace ws) {
    pending_workspace_.reset();
    // Extra roots must live where the primary does (same machine or same host).
    auto primary_ssh = parse_ssh_folder(ws.folders.front());
    extra_roots_.clear();
    int skipped = 0;
    for (size_t i = 1; i < ws.folders.size(); i++) {
        auto ssh = parse_ssh_folder(ws.folders[i]);
        std::error_code ec;
        if (primary_ssh && ssh && ssh->first == primary_ssh->first) extra_roots_.push_back(ssh->second);
        else if (!primary_ssh && !ssh && fs::is_directory(ws.folders[i], ec)) extra_roots_.push_back(ws.folders[i]);
        else skipped++;
    }
    // A name clash with a different workspace gets the folder name appended.
    if (!ws.source.empty() && ws.file.empty())
        if (auto existing = workspaces_.load(ws.name); existing && existing->source != ws.source) {
            ws.name += " (" + project_root_.filename().string() + ")";
            if (auto again = workspaces_.load(ws.name)) ws.tabs = again->tabs;
        }
    if (!ws.source.empty() && ws.tabs.empty())  // reopening a project workspace: its session is in the config dir
        if (auto saved = workspaces_.load(ws.name); saved && saved->source == ws.source) {
            ws.tabs = saved->tabs;
            ws.split = saved->split;
            ws.split_ratio = saved->split_ratio;
            ws.active_group = saved->active_group;
            ws.sidebar = saved->sidebar;
            ws.terminal = saved->terminal;
            ws.sidebar_view = saved->sidebar_view;
        }
    workspace_ = ws;
    file_tree_.set_extra_roots(extra_roots_, ws.name);
    if (!extra_roots_.empty()) {
        scan_project_files();
        command_palette_.set_file_list(all_files_);
    }
    apply_workspace_settings(ws);
    restore_workspace_tabs(ws);
    workspace_->last_opened = unix_now();
    workspaces_.save(*workspace_);
    hint_workspace_.reset();
    recent_stale_ = true;
    update_window_title();
    set_status_message("Workspace: " + ws.name +
                       (skipped ? "  (" + std::to_string(skipped) + " folder(s) unavailable)" : ""), 5.0f);
}

void App::switch_local_root(const fs::path& root) {
    if (remote_active_) {
        // Leaving a remote folder: remote_disconnect() rebuilds everything at local_root_.
        local_root_ = root;
        remote_disconnect();
        update_window_title();
        return;
    }
    tab_bar_ = TabBar{};
    tab_bar_.set_on_close_request([this](int i) { request_close_tab(i); });
    tab_bar2_ = TabBar{};
    split_ = active_right_ = false;
    other_state_.reset();
    editor_view_.set_editor(nullptr);
    editor_view2_.set_editor(nullptr);
    if (lsp_.is_running()) lsp_.stop();
    lsp_docs_.clear();
    diagnostics_.clear();
    disk_mtimes_.clear();
    project_root_ = fs::path(root).lexically_normal();
    git_.close();
    git_statuses_.clear();
    git_.open(project_root_);
    file_tree_.set_git_status({});
    scm_.set_state({}, "", {});
    status_bar_.set_git_branch("");
    status_bar_.set_git_sync(0, 0);
    if (git_.is_open()) {
        status_bar_.set_git_branch(git_.branch_name());
        request_git_refresh();
    }
    file_tree_.set_extra_roots({}, "");
    file_tree_.set_root(project_root_, vfs_.get());
    scan_project_files();
    command_palette_.set_root(project_root_);
    command_palette_.set_file_list(all_files_);
    search_.set_root(project_root_);
    terminal_argv_.clear();
    terminal_.spawn(local_shell(), project_root_);
    new_file();
    update_window_title();
}

void App::restore_workspace_tabs(const Workspace& ws) {
    auto resolve = [&](const std::string& p) {
        fs::path path(p);
        return (path.is_absolute() ? path : project_root_ / path).lexically_normal();
    };
    auto readable = [&](const fs::path& p) {
        if (vfs_->is_remote()) return true;  // a failed read reports itself
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    };
    auto place = [](Editor* ed, const WorkspaceTab& t) {
        if (!ed) return;
        size_t l = std::min(t.line, ed->buffer().line_count() - 1);
        ed->cursors().set_primary({l, std::min(t.col, ed->buffer().line_length(l))});
        ed->set_scroll(0, t.scroll_y);
    };
    bool has_right = ws.split && std::any_of(ws.tabs.begin(), ws.tabs.end(), [](auto& t) { return t.group == 1; });
    std::unordered_map<std::string, std::shared_ptr<Editor>> right_eds;
    std::shared_ptr<Editor> right_active, left_active;

    if (has_right) {
        // Load the right group into the active slot, then move it aside.
        for (auto& t : ws.tabs) {
            if (t.group != 1) continue;
            fs::path p = resolve(t.path);
            if (!readable(p)) continue;
            open_file(p);
            auto ed = tab_bar_.active_editor();
            if (!ed || ed->buffer().file_path() != p) continue;
            place(ed.get(), t);
            right_eds[p.string()] = ed;
            if (t.active) right_active = ed;
        }
        if (!tab_bar_.empty() && tab_bar_.active_editor() && !tab_bar_.active_editor()->buffer().file_path().empty()) {
            if (right_active)
                for (size_t i = 0; i < tab_bar_.tabs().size(); i++)
                    if (tab_bar_.tabs()[i].editor == right_active) tab_bar_.set_active(static_cast<int>(i));
            std::swap(tab_bar_, tab_bar2_);
            std::swap(editor_view_, editor_view2_);
            tab_bar_ = TabBar{};
            tab_bar_.set_on_close_request([this](int i) { request_close_tab(i); });
            split_ = true;
            active_right_ = false;
            split_ratio_ = ws.split_ratio;
        }
    }
    std::optional<ViewState> shared_left;
    for (auto& t : ws.tabs) {
        if (has_right && t.group == 1) continue;
        fs::path p = resolve(t.path);
        if (auto it = right_eds.find(p.string()); split_ && it != right_eds.end()) {
            // Same document in both groups: one buffer, separate cursor/scroll per view.
            auto ed = it->second;
            ViewState right_view{ed->cursors(), ed->scroll_x(), ed->scroll_y()};
            place(ed.get(), t);
            if (t.active) shared_left = ViewState{ed->cursors(), ed->scroll_x(), ed->scroll_y()};
            ed->cursors() = right_view.cursors;
            ed->set_scroll(right_view.sx, right_view.sy);
            tab_bar_.add_tab(ed);
            if (t.active) left_active = ed;
            continue;
        }
        if (!readable(p)) continue;
        open_file(p);
        auto ed = tab_bar_.active_editor();
        if (!ed || ed->buffer().file_path() != p) continue;
        place(ed.get(), t);
        if (t.active) left_active = ed;
    }
    if (left_active)
        for (size_t i = 0; i < tab_bar_.tabs().size(); i++)
            if (tab_bar_.tabs()[i].editor == left_active) tab_bar_.set_active(static_cast<int>(i));
    if (split_) {
        if (tab_bar_.empty()) {
            unsplit_if_empty();
        } else if (shared_left && tab_bar_.active_editor() == tab_bar2_.active_editor()) {
            // The active (left) view shows the left state; the right one keeps its own.
            auto ed = tab_bar_.active_editor();
            other_state_ = ViewState{ed->cursors(), ed->scroll_x(), ed->scroll_y()};
            ed->cursors() = shared_left->cursors;
            ed->set_scroll(shared_left->sx, shared_left->sy);
        }
    }
    if (tab_bar_.empty()) new_file();
    editor_view_.set_editor(tab_bar_.active_editor());
    status_bar_.set_editor(tab_bar_.active_editor());
    if (split_ && ws.active_group == 1) focus_group(true);

    sidebar_visible_ = ws.sidebar;
    sidebar_view_ = static_cast<SidebarView>(view_index(ws.sidebar_view));
    terminal_visible_ = ws.terminal;
    if (terminal_visible_ && !terminal_.is_alive()) restart_terminal();
    focus_editor();
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

Workspace App::capture_workspace() const {
    Workspace ws = workspace_ ? *workspace_ : Workspace{};
    ws.folders = {folder_uri(project_root_)};
    for (auto& r : extra_roots_) ws.folders.push_back(folder_uri(r));
    ws.tabs.clear();
    const TabBar& left = active_right_ ? tab_bar2_ : tab_bar_;
    const TabBar& right = active_right_ ? tab_bar_ : tab_bar2_;
    auto rel = [&](const fs::path& p) {
        fs::path r = p.lexically_relative(project_root_);
        bool inside = !r.empty() && r.generic_string().rfind("..", 0) != 0;
        return inside ? r.generic_string() : p.generic_string();
    };
    auto add_group = [&](const TabBar& tb, int group, bool is_active_group) {
        for (size_t i = 0; i < tb.tabs().size(); i++) {
            auto& ed = tb.tabs()[i].editor;
            if (!ed || ed->buffer().file_path().empty()) continue;
            WorkspaceTab t;
            t.path = rel(ed->buffer().file_path());
            t.group = group;
            t.active = static_cast<int>(i) == tb.active_index();
            Position p = ed->cursors().primary_pos();
            float sy = ed->scroll_y();
            // The inactive group's view of a shared document lives in other_state_.
            if (!is_active_group && other_state_ && t.active && ed == tab_bar_.active_editor()) {
                p = other_state_->cursors.primary_pos();
                sy = other_state_->sy;
            }
            t.line = p.line;
            t.col = p.col;
            t.scroll_y = sy;
            ws.tabs.push_back(std::move(t));
        }
    };
    add_group(left, 0, !active_right_);
    if (split_) add_group(right, 1, active_right_);
    ws.split = split_ && !tab_bar2_.empty();
    ws.split_ratio = split_ratio_;
    ws.active_group = split_ && active_right_ ? 1 : 0;
    ws.sidebar = sidebar_visible_;
    ws.terminal = terminal_visible_;
    ws.sidebar_view = view_name(static_cast<int>(sidebar_view_));
    return ws;
}

void App::autosave_workspace() {
    if (!workspace_ || pending_workspace_) return;
    Workspace ws = capture_workspace();
    // [settings] belongs to the file: keep edits made to it while the workspace was open.
    if (auto disk = workspaces_.load(ws.name)) ws.settings = disk->settings;
    ws.last_opened = unix_now();
    if (workspaces_.save(ws)) workspace_ = ws;
}

void App::save_workspace_as(const std::string& suggested) {
    std::string def = workspace_ ? workspace_->name : project_root_.filename().string();
    auto do_save = [this](const std::string& name) {
        Workspace ws = capture_workspace();
        if (!workspace_ || name != workspace_->name) {
            ws.source.clear();
            ws.file.clear();
        }
        ws.name = name;
        ws.last_opened = unix_now();
        if (!workspaces_.save(ws)) {
            set_status_message("Could not save workspace to " + workspaces_.dir().string(), 8.0f);
            return;
        }
        workspace_ = ws;
        file_tree_.set_extra_roots(extra_roots_, ws.name);
        hint_workspace_.reset();
        recent_stale_ = true;
        update_window_title();
        set_status_message("Saved workspace \"" + name + "\"  (it autosaves from now on)", 6.0f);
    };
    auto confirm = [this, do_save](const std::string& raw) {
        std::string name = WorkspaceStore::sanitize_name(raw);
        if (name.empty()) {
            set_status_message("Enter a workspace name");
            return;
        }
        if (workspaces_.exists(name) && !(workspace_ && workspace_->name == name)) {
            CommandPalette::PickOptions ask;
            ask.placeholder = "Workspace \"" + name + "\" exists. Replace it?";
            ask.items = {"Replace", "Cancel"};
            ask.on_pick = [do_save, name](int i, const std::string&) {
                if (i == 0) do_save(name);
            };
            command_palette_.pick(std::move(ask));
            return;
        }
        do_save(name);
    };
    if (!suggested.empty()) {
        confirm(suggested);
        return;
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Save workspace as (Enter = \"" + def + "\")";
    opts.allow_free_text = true;
    opts.on_pick = [confirm, def](int, const std::string& q) { confirm(q.empty() ? def : q); };
    command_palette_.pick(std::move(opts));
}

void App::add_folder_to_workspace() {
    // Suggest sibling folders of the current root; any path can be typed.
    std::vector<fs::path> dirs;
    fs::path parent = project_root_.parent_path();
    std::vector<Vfs::DirEntry> items;
    if (vfs_->list(parent, items))
        for (auto& it : items) {
            fs::path p = parent / it.name;
            if (!it.is_dir || it.name.empty() || it.name[0] == '.' || p == project_root_) continue;
            if (std::find(extra_roots_.begin(), extra_roots_.end(), p) != extra_roots_.end()) continue;
            dirs.push_back(p);
        }
    std::sort(dirs.begin(), dirs.end());
    std::vector<std::string> labels;
    for (auto& d : dirs) labels.push_back(pretty_folder(d.generic_string()));
    CommandPalette::PickOptions opts;
    opts.placeholder = "Add folder to workspace (pick, or type a path)";
    opts.items = labels;
    opts.allow_free_text = true;
    opts.on_pick = [this, dirs](int i, const std::string& q) {
        fs::path p;
        if (i >= 0) p = dirs[static_cast<size_t>(i)];
        else if (!q.empty()) {
            p = vfs_->is_remote() ? fs::path(q) : expand_home(q);
            if (p.is_relative()) p = project_root_.parent_path() / p;
        }
        if (p.empty()) return;
        p = p.lexically_normal();
        std::vector<Vfs::DirEntry> probe;
        if (!vfs_->list(p, probe)) {
            set_status_message("Not a folder: " + p.string(), 6.0f);
            return;
        }
        if (p == project_root_ || std::find(extra_roots_.begin(), extra_roots_.end(), p) != extra_roots_.end()) {
            set_status_message("Already in the workspace");
            return;
        }
        extra_roots_.push_back(p);
        file_tree_.set_extra_roots(extra_roots_, workspace_ ? workspace_->name : "UNTITLED WORKSPACE");
        scan_project_files();
        command_palette_.set_file_list(all_files_);
        show_sidebar_view(SidebarView::Explorer);
        if (workspace_) {
            autosave_workspace();
            set_status_message("Added " + p.filename().string() + " to " + workspace_->name);
        } else {
            set_status_message("Added " + p.filename().string() + ". Save Workspace As... keeps this set", 8.0f);
        }
    };
    command_palette_.pick(std::move(opts));
}

void App::close_workspace(bool keep_tabs) {
    autosave_workspace();
    revert_workspace_settings();
    workspace_.reset();
    extra_roots_.clear();
    file_tree_.set_extra_roots({}, "");
    scan_project_files();
    command_palette_.set_file_list(all_files_);
    if (!keep_tabs) {
        for (int i = static_cast<int>(tab_bar_.tabs().size()) - 1; i >= 0; i--) tab_bar_.close_tab(i);
        if (split_) {
            tab_bar2_ = TabBar{};
            unsplit_if_empty();
        }
        if (tab_bar_.empty()) new_file();
        editor_view_.set_editor(tab_bar_.active_editor());
        status_bar_.set_editor(tab_bar_.active_editor());
    }
    update_workspace_hint(false);
    update_window_title();
    set_status_message("Workspace closed");
}

void App::delete_workspace() {
    auto list = workspaces_.list();
    if (list.empty()) {
        set_status_message("No saved workspaces");
        return;
    }
    std::vector<std::string> items;
    for (auto& w : list) items.push_back(w.name + "   " + folders_summary(w));
    CommandPalette::PickOptions opts;
    opts.placeholder = "Delete which workspace? (its folders and files are not touched)";
    opts.items = items;
    opts.on_pick = [this, list](int i, const std::string&) {
        if (i < 0) return;
        std::string name = list[static_cast<size_t>(i)].name;
        CommandPalette::PickOptions ask;
        ask.placeholder = "Delete workspace \"" + name + "\"?";
        ask.items = {"Delete", "Cancel"};
        ask.on_pick = [this, name](int c, const std::string&) {
            if (c != 0) return;
            if (!workspaces_.remove(name)) {
                set_status_message("Could not delete " + workspaces_.path_for(name).string(), 8.0f);
                return;
            }
            if (workspace_ && workspace_->name == name) {
                // Keep working where we are, as a plain folder.
                revert_workspace_settings();
                workspace_.reset();
                file_tree_.set_extra_roots(extra_roots_, extra_roots_.empty() ? "" : "UNTITLED WORKSPACE");
                update_window_title();
            }
            update_workspace_hint(false);
            set_status_message("Deleted workspace \"" + name + "\"");
        };
        command_palette_.pick(std::move(ask));
    };
    command_palette_.pick(std::move(opts));
}

void App::open_folder_prompt() {
    CommandPalette::PickOptions opts;
    std::string where = remote_active_ ? " on " + remote_target_.alias : "";
    opts.placeholder = "Open folder" + where + " (absolute, ~/..., or relative to " +
                       pretty_folder(project_root_.parent_path().generic_string()) + ")";
    opts.allow_free_text = true;
    opts.on_pick = [this](int, const std::string& q) {
        if (q.empty()) return;
        fs::path p = remote_active_ ? fs::path(q) : expand_home(q);
        if (p.is_relative()) p = project_root_.parent_path() / p;
        p = p.lexically_normal();
        std::vector<Vfs::DirEntry> probe;
        if (!vfs_->list(p, probe)) {
            set_status_message("Not a folder: " + p.string(), 6.0f);
            return;
        }
        confirm_discard("Open Folder", [this, p] {
            // Opening a folder never loads a workspace; it may only point one out.
            if (workspace_) close_workspace(true);
            else save_session();
            extra_roots_.clear();
            if (remote_active_) open_remote_folder(p);
            else switch_local_root(p);
            update_workspace_hint(true);
            update_window_title();
        });
    };
    command_palette_.pick(std::move(opts));
}

// ---------------------------------------------------------------------------
// Per-workspace settings
// ---------------------------------------------------------------------------

void App::apply_workspace_settings(const Workspace& ws) {
    revert_workspace_settings();
    std::string family = font_family_;
    float size = font_size_;
    for (auto& [key, raw] : ws.settings) {
        std::string v = unquote(raw);
        bool on = v == "true";
        if (key == "theme") {
            std::string prev = themes_.current();
            if (themes_.apply(v, theme_)) workspace_undo_.push_back([this, prev] { themes_.apply(prev, theme_); });
        } else if (key == "font_family") {
            family = v;
        } else if (key == "font_size") {
            size = std::clamp(std::strtof(v.c_str(), nullptr), 8.0f, 40.0f);
        } else if (key == "word_wrap") {
            bool prev = editor_view_.wrap();
            editor_view_.set_wrap(on);
            editor_view2_.set_wrap(on);
            workspace_undo_.push_back([this, prev] { editor_view_.set_wrap(prev); editor_view2_.set_wrap(prev); });
        } else if (key == "minimap") {
            bool prev = editor_view_.minimap();
            editor_view_.set_minimap(on);
            workspace_undo_.push_back([this, prev] { editor_view_.set_minimap(prev); });
        } else if (key == "breadcrumbs") {
            bool prev = show_breadcrumbs_;
            show_breadcrumbs_ = on;
            workspace_undo_.push_back([this, prev] { show_breadcrumbs_ = prev; });
        } else if (key == "ligatures") {
            bool prev = font_.ligatures();
            font_.set_ligatures(on);
            workspace_undo_.push_back([this, prev] { font_.set_ligatures(prev); });
        } else if (key == "animations") {
            bool prev = anim::enabled;
            anim::enabled = on;
            workspace_undo_.push_back([prev] { anim::enabled = prev; });
        } else if (key == "bracket_colors" || key == "scope_highlight" || key == "occurrence_underline") {
            bool* flag = key == "bracket_colors" ? &ScopeHighlight::bracket_colors
                       : key == "scope_highlight" ? &ScopeHighlight::scopes : &ScopeHighlight::occurrences;
            bool prev = *flag;
            *flag = on;
            workspace_undo_.push_back([flag, prev] { *flag = prev; });
        } else if (key == "auto_close_brackets") {
            bool prev = Editor::auto_close_pairs;
            Editor::auto_close_pairs = on;
            workspace_undo_.push_back([prev] { Editor::auto_close_pairs = prev; });
        }
    }
    if (family != font_family_ || size != font_size_) {
        std::string prev_family = font_family_;
        float prev_size = font_size_;
        if (reload_font(family, size))
            workspace_undo_.push_back([this, prev_family, prev_size] { reload_font(prev_family, prev_size); });
    }
}

void App::revert_workspace_settings() {
    for (auto it = workspace_undo_.rbegin(); it != workspace_undo_.rend(); ++it) (*it)();
    workspace_undo_.clear();
}

// ---------------------------------------------------------------------------
// Welcome screen: shown in place of a lone, empty Untitled editor
// ---------------------------------------------------------------------------

void App::render_welcome(RenderContext& ctx, Rect r) {
    welcome_hits_.clear();
    auto ed = tab_bar_.active_editor();
    welcome_visible_ = !split_ && tab_bar_.tabs().size() == 1 && ed && ed->buffer().file_path().empty() &&
                       ed->buffer().length() == 0 && !ed->is_modified() && r.w > 260 && r.h > 160 &&
                       !settings_page_.visible();
    if (!welcome_visible_) return;
    if (recent_stale_) {
        recent_workspaces_ = workspaces_.list();
        recent_stale_ = false;
    }
    const Theme& th = theme_;
    float cw = font_.char_width(), lh = font_.line_height();
    float row_h = std::round(lh + 12.0f);
    ctx.r->add_rect(r, th.bg);

    float col_w = std::min(620.0f, r.w - 48.0f);
    float x = std::floor(r.x + (r.w - col_w) * 0.5f);
    float y = r.y + std::floor(std::min(72.0f, r.h * 0.08f));
    float bottom = r.y + r.h - 12.0f;
    auto base = [&](float top, float h) { return top + (h - lh) * 0.5f + font_.ascender(); };
    auto hovered = [&](Rect b) { return b.contains(ctx.mouse_x, ctx.mouse_y); };

    // Title
    if (const GlyphInfo* gi = font_.glyph_sized(0xF0238, 30)) {  // nf-md-fire
        ctx.r->add_glyph(std::round(x - gi->bearing_x), std::round(y + 30.0f), gi, th.accent);
        ctx.r->add_text(x + gi->width + 12.0f, y + 24.0f, "Overheat", th.fg, font_);
    } else {
        ctx.r->add_text(x, y + 24.0f, "Overheat", th.fg, font_);
    }
    y += 52.0f;
    std::string sub = workspace_ ? "Workspace " + workspace_->name : pretty_folder(folder_uri(project_root_));
    ctx.r->add_text(x, base(y, lh), fit(sub, col_w, cw), th.fg_muted, font_);
    y += lh + 22.0f;

    // "This folder belongs to ..." card: the only nudge, and it never opens by itself.
    if (hint_workspace_ && y + row_h * 1.6f < bottom) {
        Rect card{x, y, col_w, std::round(row_h * 1.6f)};
        ctx.r->add_rect(card, hovered(card) ? th.surface_hover : th.surface, 8.0f);
        ctx.r->add_rect({card.x, card.y + 8.0f, 3.0f, card.h - 16.0f}, th.accent, 1.5f);
        std::string action = "Open  Ctrl+R";
        float ax = card.x + card.w - 16.0f - static_cast<float>(utf8_len(action)) * cw;
        std::string msg = "This folder is in workspace \"" + hint_workspace_->name + "\"";
        if (static_cast<float>(utf8_len(msg)) * cw > ax - card.x - 30.0f) msg = "Workspace \"" + hint_workspace_->name + "\"";
        ctx.r->add_text(card.x + 18.0f, base(card.y, card.h), fit(msg, ax - card.x - 30.0f, cw), th.fg, font_);
        ctx.r->add_text(ax, base(card.y, card.h), action, th.accent, font_);
        Workspace w = *hint_workspace_;
        welcome_hits_.push_back({card, [this, w] { open_workspace(w); }});
        y += card.h + 22.0f;
    }

    auto section = [&](const std::string& title) {
        ctx.r->add_text(x, base(y, lh), title, th.fg_muted, font_);
        y += lh + 8.0f;
    };
    // Start actions
    struct Action { const char* label; const char* cmd; };
    static const Action actions[] = {{"Open Workspace...", "workspace_switch"},
                                     {"Open Folder...", "open_folder"},
                                     {"Quick Open File...", "command_palette_files"},
                                     {"Save Workspace As...", "workspace_save_as"}};
    // Recent workspaces
    if (y + lh + row_h < bottom) {
        section("RECENT WORKSPACES");
        if (recent_workspaces_.empty()) {
            ctx.r->add_text(x + 12.0f, base(y, row_h), fit("None yet. Save Workspace As... keeps folders, tabs and layout.",
                                                           col_w - 12.0f, cw), th.fg_muted, font_);
            y += row_h;
        }
        int reserve = r.h > 420 ? 2 + static_cast<int>(std::size(actions)) : 0;  // room for the start list
        for (auto& w : recent_workspaces_) {
            if (y + row_h + reserve * row_h > bottom) break;
            Rect row{x, y, col_w, row_h};
            bool current = workspace_ && w.name == workspace_->name;
            if (hovered(row)) ctx.r->add_rect(row, th.surface_hover, 6.0f);
            float b = base(y, row_h);
            std::string when = time_ago(w.last_opened);
            float when_x = row.x + row.w - 12.0f - static_cast<float>(utf8_len(when)) * cw;
            float name_w = std::min(static_cast<float>(utf8_len(w.name)) * cw, (when_x - row.x) * 0.45f);
            float nx = ctx.r->add_text(row.x + 12.0f, b, fit(w.name, name_w + 1.0f, cw), current ? th.accent : th.fg, font_);
            ctx.r->add_text(nx + 2.0f * cw, b, fit(folders_summary(w), when_x - nx - 4.0f * cw, cw), th.fg_muted, font_);
            ctx.r->add_text(when_x, b, when, th.line_number, font_);
            if (!current) {
                Workspace copy = w;
                welcome_hits_.push_back({row, [this, copy] { open_workspace(copy); }});
            }
            y += row_h;
        }
        y += 18.0f;
    }
    if (y + lh + row_h < bottom) {
        section("START");
        for (auto& a : actions) {
            if (y + row_h > bottom) break;
            Rect row{x, y, col_w, row_h};
            if (hovered(row)) ctx.r->add_rect(row, th.surface_hover, 6.0f);
            float b = base(y, row_h);
            ctx.r->add_text(row.x + 12.0f, b, a.label, th.accent, font_);
            std::string keys = keymap_.binding_for(a.cmd);
            if (!keys.empty())
                ctx.r->add_text(row.x + row.w - 12.0f - static_cast<float>(utf8_len(keys)) * cw, b, keys, th.fg_muted, font_);
            std::string cmd = a.cmd;
            welcome_hits_.push_back({row, [this, cmd] { handle_command(cmd); }});
            y += row_h;
        }
    }
}

bool App::welcome_click(float x, float y) {
    if (!welcome_visible_ || command_palette_.is_visible()) return false;
    for (auto& [rect, fn] : welcome_hits_)
        if (rect.contains(x, y)) {
            auto f = fn;  // the handler may rebuild welcome_hits_
            f();
            return true;
        }
    return false;
}

} // namespace oh
