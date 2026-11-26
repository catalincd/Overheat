// Status bar items and their click actions, the Command Center, Quick Open's
// recently-opened list, notifications, and git pull/push.
#include "app.h"
#include "platform/platform.h"
#include <algorithm>
#include <sstream>

namespace oh {

namespace {

std::set<std::string> parse_list(const std::string& v) {
    std::set<std::string> out;
    std::string item;
    std::istringstream ss(v);
    while (std::getline(ss, item, ',')) {
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.insert(item);
    }
    return out;
}

std::string join_list(const std::set<std::string>& ids) {
    std::string out;
    for (auto& id : ids) out += (out.empty() ? "" : ",") + id;
    return out;
}

// Runs git in `dir` and returns {success, combined output}.
std::pair<bool, std::string> run_git(const fs::path& dir, std::vector<std::string> args) {
    std::vector<std::string> argv;
#ifndef _WIN32
    argv = {"env", "GIT_TERMINAL_PROMPT=0"};  // never block on a credential prompt
#endif
    argv.push_back("git");
    argv.insert(argv.end(), args.begin(), args.end());
    platform::Process p;
    if (!p.start(argv, dir, true)) return {false, "could not run git"};
    p.close_stdin();
    std::string out;
    char buf[4096];
    for (long n; (n = p.read(buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
    bool ok = p.wait() == 0;
    request_redraw();
    return {ok, out};
}

// Last non-empty line of command output, for a one-line status message.
std::string last_line(const std::string& s) {
    std::string line, last;
    std::istringstream ss(s);
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    return last;
}

}  // namespace

void App::setup_status_bar() {
    show_command_center_ = themes_.setting("command_center") != "false";
    status_bar_.set_hidden(parse_list(themes_.setting("status_bar_hidden")));
    status_bar_.set_on_click([this](const std::string& id) { status_click(id); });
    command_center_.set_on_click([this] { handle_command("command_palette_files"); });

    auto add = [this](const std::string& name, const std::string& label, const std::string& keys) {
        commands_.push_back({name, label, [this, name]() { handle_command(name); }, keys});
    };
    add("toggle_command_center", "View: Toggle Command Center", "");
    add("status_bar_items", "View: Status Bar Items...", "");
    add("problems", "View: Problems", "");
    add("next_problem", "Go to Next Problem (Error, Warning)", "F8");
    add("change_indentation", "Change Indentation...", "");
    add("change_eol", "Change End of Line Sequence", "");
    add("change_encoding", "Change File Encoding", "");
    add("change_language", "Change Language Mode", "");
    add("lsp_menu", "Language Server: Restart / Stop...", "");
    add("notifications", "Notifications: Show Notifications", "");
    add("remote_menu", "Remote: Show Remote Menu", "");
    add("git_pull", "Git: Pull", "");
    add("git_push", "Git: Push", "");
    add("git_sync", "Git: Sync (Pull, then Push)", "");
    if (keymap_.lookup(SDLK_F8, KeyMod::None).empty() && keymap_.binding_for("next_problem").empty())
        keymap_.bind(SDLK_F8, KeyMod::None, "next_problem");
}

bool App::status_command(const std::string& cmd) {
    if (cmd == "toggle_command_center") {
        show_command_center_ = !show_command_center_;
        themes_.set_setting("command_center", show_command_center_ ? "true" : "false");
    } else if (cmd == "status_bar_items") {
        status_items_menu();
    } else if (cmd == "problems") {
        show_problems();
    } else if (cmd == "next_problem") {
        goto_next_problem();
    } else if (cmd == "change_indentation") {
        pick_indentation();
    } else if (cmd == "change_eol") {
        pick_line_endings();
    } else if (cmd == "change_encoding") {
        pick_encoding();
    } else if (cmd == "change_language") {
        pick_language();
    } else if (cmd == "lsp_menu") {
        lsp_menu();
    } else if (cmd == "notifications") {
        show_notifications();
    } else if (cmd == "remote_menu") {
        remote_menu();
    } else if (cmd == "git_pull" || cmd == "git_push" || cmd == "git_sync") {
        git_remote_op(cmd.substr(4));
    } else {
        return false;
    }
    return true;
}

void App::status_click(const std::string& id) {
    if (id == "remote") remote_menu();
    else if (id == "branch") git_checkout();
    else if (id == "sync") git_remote_op(status_bar_.publish_mode() ? "publish" : "sync");
    else if (id == "problems") show_problems();
    else if (id == "tasks") show_notifications();
    else if (id == "position") handle_command("goto_line");
    else if (id == "indent") pick_indentation();
    else if (id == "encoding") pick_encoding();
    else if (id == "eol") pick_line_endings();
    else if (id == "language") pick_language();
    else if (id == "lsp") lsp_menu();
    else if (id == "bell") show_notifications();
    else if (id == "menu") status_items_menu();
}

void App::note_recent_file(const fs::path& p) {
    if (p.empty()) return;
    if (!recent_files_.empty() && recent_files_.front() == p) return;
    std::erase(recent_files_, p);
    recent_files_.insert(recent_files_.begin(), p);
    if (recent_files_.size() > 50) recent_files_.resize(50);
}

void App::update_status_bar() {
    poll_git_op();
    Editor* ed = current_editor();
    fs::path path = ed ? ed->buffer().file_path() : fs::path();
    note_recent_file(path);
    command_palette_.set_recent(recent_files_);
    command_palette_.set_top(show_command_center_ ? 3.0f : -1.0f);

    // Command Center: "file — workspace", like VS Code's title.
    std::string where = workspace_ ? workspace_->name : project_root_.filename().string();
    if (where.empty()) where = project_root_.string();
    if (remote_active_) where += " [SSH: " + remote_target_.alias + "]";
    std::string file = ed ? (path.empty() ? std::string("Untitled") : path.filename().string()) : std::string();
    command_center_.set_title(file.empty() ? where : file + " — " + where);

    // Background work, newest kind first.
    std::vector<std::string> tasks;
    if (connect_job_.valid()) tasks.push_back("Connecting to " + remote_target_.alias + "...");
    if (git_op_job_.valid()) tasks.push_back(git_op_label_);
    if (lsp_.is_running() && !lsp_.is_ready()) tasks.push_back("Starting " + lsp_server_ + "...");
    else if (std::string p = lsp_.is_running() ? lsp_.progress() : std::string(); !p.empty())
        tasks.push_back(lsp_server_ + ": " + p);  // work-done progress, e.g. indexing
    status_bar_.set_tasks(std::move(tasks));

    if (connect_job_.valid()) status_bar_.set_remote("Opening Remote...");
    else status_bar_.set_remote(remote_active_ ? "SSH: " + remote_target_.alias : std::string());

    // Language server for this file: starting, ready, or stopped after a crash.
    std::string lsp_name;
    int lsp_state = 0;
    if (!lsp_server_.empty() && !path.empty()) {
        if (lsp_.is_running() && !lsp_.is_ready()) lsp_name = lsp_server_;
        else if (lsp_.is_running() && lsp_.is_open(path)) lsp_name = lsp_server_, lsp_state = 1;
        else if (!lsp_.is_running() && lsp_docs_.count(path.string())) lsp_name = lsp_server_, lsp_state = 2;
    }
    status_bar_.set_lsp(lsp_name, lsp_state);
    status_bar_.set_unread(unread_notifications_);
}

// ---------------------------------------------------------------------------
// Click actions
// ---------------------------------------------------------------------------

void App::remote_menu() {
    std::vector<std::string> items, details;
    std::vector<std::function<void()>> actions;
    auto item = [&](std::string label, std::string detail, std::function<void()> fn) {
        items.push_back(std::move(label));
        details.push_back(std::move(detail));
        actions.push_back(std::move(fn));
    };
    if (remote_active_) {
        item("Close Remote Connection", remote_target_.display(), [this] { handle_command("remote_disconnect"); });
        item("Connect to Another Host...", "Remote-SSH", [this] { remote_connect_prompt(); });
        item("Show System Monitor Details", "", [this] { handle_command("monitor_details"); });
        item(remote_monitor_ ? "Hide Remote Monitor" : "Show Remote Monitor", "", [this] { handle_command("toggle_remote_monitor"); });
    } else {
        item("Connect to Host...", "Remote-SSH", [this] { remote_connect_prompt(); });
        item("Open Workspace...", "Ctrl+R", [this] { handle_command("workspace_switch"); });
        item(local_monitor_ ? "Hide Local System Monitor" : "Show Local System Monitor", "",
             [this] { handle_command("toggle_local_monitor"); });
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = remote_active_ ? "Remote: " + remote_target_.alias : "Select an option to open a remote window";
    opts.items = std::move(items);
    opts.details = std::move(details);
    opts.on_pick = [actions](int i, const std::string&) {
        if (i >= 0) actions[static_cast<size_t>(i)]();
    };
    command_palette_.pick(std::move(opts));
}

namespace {
struct Problem {
    std::string file;
    LSPDiagnostic d;
};
bool before(const Position& a, const Position& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }
}  // namespace

void App::show_problems() {
    std::vector<Problem> all;
    for (auto& [file, list] : diagnostics_)
        for (auto& d : list)
            if (d.severity <= 2) all.push_back({file, d});
    if (all.empty()) {
        status_bar_.set_message("No problems have been detected in the workspace.");
        return;
    }
    std::stable_sort(all.begin(), all.end(), [](const Problem& a, const Problem& b) {
        if (a.d.severity != b.d.severity) return a.d.severity < b.d.severity;
        if (a.file != b.file) return a.file < b.file;
        return before(a.d.start, b.d.start);
    });
    std::vector<std::string> items, details;
    for (auto& p : all) {
        std::string msg = p.d.message.substr(0, p.d.message.find('\n'));
        items.push_back((p.d.severity == 1 ? "✖ " : "⚠ ") + msg);
        details.push_back(fs::path(p.file).filename().string() + ":" + std::to_string(p.d.start.line + 1));
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Problems: " + std::to_string(all.size()) + " (pick one to go there)";
    opts.items = std::move(items);
    opts.details = std::move(details);
    opts.on_pick = [this, all](int i, const std::string&) {
        if (i < 0) return;
        auto& p = all[static_cast<size_t>(i)];
        open_at(p.file, p.d.start.line, p.d.start.col, 0);
    };
    command_palette_.pick(std::move(opts));
}

void App::goto_next_problem() {
    Editor* ed = current_editor();
    std::string cur = ed ? ed->buffer().file_path().string() : std::string();
    auto pick_from = [](const std::vector<LSPDiagnostic>& list, Position after, bool strict) -> const LSPDiagnostic* {
        const LSPDiagnostic* best = nullptr;
        for (auto& d : list) {
            if (d.severity > 2 || (strict && !before(after, d.start))) continue;
            if (!best || before(d.start, best->start)) best = &d;
        }
        return best;
    };
    const LSPDiagnostic* d = nullptr;
    std::string file = cur;
    if (auto it = diagnostics_.find(cur); ed && it != diagnostics_.end()) {
        d = pick_from(it->second, ed->cursors().primary_pos(), true);
        if (!d) d = pick_from(it->second, {0, 0}, false);  // wrap around in this file
    }
    if (!d)  // nothing here: the first problem of another file
        for (auto& [f, list] : diagnostics_)
            if ((d = pick_from(list, {0, 0}, false))) {
                file = f;
                break;
            }
    if (!d) {
        status_bar_.set_message("No problems");
        return;
    }
    LSPDiagnostic copy = *d;
    open_at(file, copy.start.line, copy.start.col, 0);
    status_bar_.set_message(copy.message.substr(0, copy.message.find('\n')), 6.0f);
}

void App::pick_indentation() {
    Editor* ed = current_editor();
    if (!ed) return;
    auto cur = ed->indent_style();
    std::vector<std::string> items = {"Indent Using Spaces: 2", "Indent Using Spaces: 4", "Indent Using Spaces: 8",
                                      "Indent Using Tabs", "Convert Indentation to Spaces",
                                      "Convert Indentation to Tabs", "Re-indent to Spaces: 2",
                                      "Re-indent to Spaces: 4"};
    CommandPalette::PickOptions opts;
    opts.placeholder = std::string("Indentation: ") + (cur.tabs ? "Tabs" : "Spaces: " + std::to_string(cur.size));
    opts.items = items;
    opts.initial = cur.tabs ? 3 : cur.size == 2 ? 0 : cur.size == 8 ? 2 : 1;
    opts.on_pick = [this](int i, const std::string&) {
        Editor* ed = current_editor();
        if (!ed || i < 0) return;
        auto s = ed->indent_style();
        switch (i) {
            case 0: case 1: case 2: ed->set_indent_style({false, i == 0 ? 2 : i == 1 ? 4 : 8}); break;
            case 3: ed->set_indent_style({true, s.size}); break;
            case 4: ed->convert_indentation({false, s.size}); break;
            case 5: ed->convert_indentation({true, s.size}); break;
            case 6: case 7: ed->convert_indentation({false, i == 6 ? 2 : 4}); break;
        }
        focus_editor();
    };
    command_palette_.pick(std::move(opts));
}

void App::pick_line_endings() {
    Editor* ed = current_editor();
    if (!ed) return;
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select End of Line Sequence (converts the file)";
    opts.items = {"LF", "CRLF"};
    opts.details = {"\\n  (Linux, macOS)", "\\r\\n  (Windows)"};
    opts.initial = ed->uses_crlf() ? 1 : 0;
    opts.on_pick = [this](int i, const std::string&) {
        if (Editor* ed = current_editor(); ed && i >= 0) ed->set_line_endings(i == 1);
        focus_editor();
    };
    command_palette_.pick(std::move(opts));
}

void App::pick_encoding() {
    Editor* ed = current_editor();
    if (!ed) return;
    CommandPalette::PickOptions opts;
    opts.placeholder = "Save with Encoding (files are read and written as UTF-8)";
    opts.items = {"UTF-8", "UTF-8 with BOM"};
    opts.details = {"utf8", "utf8bom"};
    opts.initial = ed->has_bom() ? 1 : 0;
    opts.on_pick = [this](int i, const std::string&) {
        if (Editor* ed = current_editor(); ed && i >= 0) ed->set_bom(i == 1);
        focus_editor();
    };
    command_palette_.pick(std::move(opts));
}

void App::pick_language() {
    Editor* ed = current_editor();
    if (!ed) return;
    auto& langs = StatusBar::languages();
    std::vector<std::string> items, details;
    std::string cur = ed->syntax().language_id();
    int initial = 0;
    for (size_t i = 0; i < langs.size(); i++) {
        if (langs[i].first == cur) initial = static_cast<int>(i);
        items.push_back(langs[i].second);
        std::string d = langs[i].first;
        if (SyntaxHighlighter::has_grammar(d)) d += "  (highlighted)";
        details.push_back(d);
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select Language Mode";
    opts.items = std::move(items);
    opts.details = std::move(details);
    opts.initial = initial;
    opts.on_pick = [this](int i, const std::string&) {
        Editor* ed = current_editor();
        if (!ed || i < 0) return;
        ed->set_language(StatusBar::languages()[static_cast<size_t>(i)].first);
        focus_editor();
    };
    command_palette_.pick(std::move(opts));
}

void App::lsp_menu() {
    if (lsp_server_.empty()) {
        status_bar_.set_message("No language server for this file");
        return;
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = lsp_server_ + (lsp_.is_ready() ? ": running" : lsp_.is_running() ? ": starting" : ": stopped");
    opts.items = {"Restart " + lsp_server_, "Stop " + lsp_server_, "Show Problems"};
    opts.on_pick = [this](int i, const std::string&) {
        if (i == 2) return show_problems();
        if (i < 0) return;
        lsp_.stop();
        diagnostics_.clear();
        lsp_docs_.clear();
        editor_view_.completion().hide();
        if (i == 0) {
            if (Editor* ed = current_editor(); ed && !ed->buffer().file_path().empty())
                setup_lsp_for_file(ed->buffer().file_path());
            set_status_message("Restarted " + lsp_server_);
        } else {
            set_status_message("Stopped " + lsp_server_);
        }
    };
    command_palette_.pick(std::move(opts));
}

void App::show_notifications() {
    unread_notifications_ = 0;
    if (notifications_.empty()) {
        status_bar_.set_message("No new notifications");
        return;
    }
    std::vector<std::string> items = {"Clear All Notifications"}, details = {""};
    for (auto it = notifications_.rbegin(); it != notifications_.rend(); ++it) {
        items.push_back(it->text);
        details.push_back(time_ago(it->time));
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Notifications (newest first)";
    opts.items = std::move(items);
    opts.details = std::move(details);
    opts.initial = 1;
    opts.on_pick = [this](int i, const std::string&) {
        if (i == 0) notifications_.clear();
    };
    command_palette_.pick(std::move(opts));
}

void App::status_items_menu() {
    auto& names = StatusBar::item_names();
    std::vector<std::string> items;
    const auto& hidden = status_bar_.hidden();
    for (auto& [id, name] : names) items.push_back((hidden.count(id) ? "    " : "✓  ") + name);
    items.push_back(show_command_center_ ? "✓  Command Center (title bar)" : "    Command Center (title bar)");
    CommandPalette::PickOptions opts;
    opts.placeholder = "Show or hide status bar items";
    opts.items = std::move(items);
    opts.on_pick = [this](int i, const std::string&) {
        auto& names = StatusBar::item_names();
        if (i < 0) return;
        if (static_cast<size_t>(i) == names.size()) {
            handle_command("toggle_command_center");
        } else {
            auto hidden = status_bar_.hidden();
            const std::string& id = names[static_cast<size_t>(i)].first;
            if (!hidden.erase(id)) hidden.insert(id);
            themes_.set_setting("status_bar_hidden", "\"" + join_list(hidden) + "\"");
            status_bar_.set_hidden(std::move(hidden));
        }
        status_items_menu();  // stay open to toggle more
    };
    command_palette_.pick(std::move(opts));
}

// ---------------------------------------------------------------------------
// Git pull / push / sync
// ---------------------------------------------------------------------------

void App::git_remote_op(const std::string& op) {
    if (!git_.is_open()) {
        set_status_message("Not a git repository");
        return;
    }
    if (git_op_job_.valid()) {
        set_status_message(git_op_label_);
        return;
    }
    git_op_label_ = op == "pull" ? "Pulling..." : op == "push" ? "Pushing..." : op == "publish" ? "Publishing branch..."
                                                                                              : "Synchronizing...";
    git_op_job_ = std::async(std::launch::async, [dir = git_.workdir(), op]() -> std::pair<bool, std::string> {
        if (op == "publish") return run_git(dir, {"push", "--set-upstream", "origin", "HEAD"});
        if (op == "push") return run_git(dir, {"push"});
        auto pulled = run_git(dir, {"pull"});  // merge or rebase per the user's pull config
        if (op == "pull" || !pulled.first) return pulled;
        auto pushed = run_git(dir, {"push"});
        return {pushed.first, pulled.second + pushed.second};
    });
}

void App::poll_git_op() {
    if (!git_op_job_.valid() || git_op_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto [ok, out] = git_op_job_.get();
    std::string what = git_op_label_.substr(0, git_op_label_.find("..."));
    if (ok) set_status_message(what + " done");
    else set_status_message(what + " failed: " + last_line(out), 10.0f);
    after_head_change();  // pulled changes reach open editors; branch/sync counts refresh
}

}  // namespace oh
