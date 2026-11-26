// Remote-SSH: connect flow, remote folder switching, and the remote terminal.
#include "app.h"
#include "ssh/sftp_vfs.h"
#include <cstdio>

namespace oh {

namespace {
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}
} // namespace

std::vector<SearchHit> App::search_remote(const SearchQuery& q) {
    // Same exclusions and limits as local search: skip binaries (-I), VCS and build
    // output folders, cap matches per file and in total, and cut very long lines so a
    // minified file cannot flood the SSH channel.
    std::string cmd = "grep -rnI -m 500 -F";
    for (const char* d : {".git", ".hg", ".svn", "node_modules", "__pycache__", "build", "dist", "target", "out",
                          ".cache", ".venv"})
        cmd += std::string(" --exclude-dir=") + d;
    if (!q.case_sensitive) cmd += " -i";
    if (q.whole_word) cmd += " -w";
    cmd += " -e " + shell_quote(q.text) + " -- " + shell_quote(project_root_.generic_string()) +
           " 2>/dev/null | head -n " + std::to_string(SEARCH_MAX_RESULTS + 1) + " | cut -b 1-8192";
    std::vector<SearchHit> hits;
    std::string out = ssh_.exec(cmd).stdout_data;
    size_t start = 0;
    while (start < out.size() && hits.size() <= SEARCH_MAX_RESULTS) {
        size_t nl = out.find('\n', start);
        std::string_view row(out.data() + start, (nl == std::string::npos ? out.size() : nl) - start);
        start = nl == std::string::npos ? out.size() : nl + 1;
        size_t c1 = row.find(':'), c2 = c1 == std::string::npos ? c1 : row.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        std::string_view text = row.substr(c2 + 1);
        fs::path file(std::string(row.substr(0, c1)));
        size_t line = static_cast<size_t>(std::strtoul(std::string(row.substr(c1 + 1, c2 - c1 - 1)).c_str(), nullptr, 10));
        line = line ? line - 1 : 0;
        auto matches = find_in_line(text, q);
        // The match may lie past the cut; still list the line.
        if (matches.empty()) hits.push_back(make_search_hit(file, line, text, 0, 0));
        for (auto [col, len] : matches) hits.push_back(make_search_hit(file, line, text, col, len));
    }
    return hits;
}

void App::remote_connect_prompt() {
    pending_workspace_.reset();  // a manual connect does not open a workspace
    ssh_config_.load(SshConfig::default_path());
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select a host from ssh config, or type user@host[:port]";
    opts.items = ssh_config_.hosts();
    opts.allow_free_text = true;
    opts.on_pick = [this](int i, const std::string& query) {
        std::string target = i >= 0 ? ssh_config_.hosts()[static_cast<size_t>(i)] : query;
        if (!target.empty()) remote_connect(target, false, "");
    };
    command_palette_.pick(std::move(opts));
}

void App::remote_connect(const std::string& target, bool trust_host, const std::string& secret) {
    if (connect_job_.valid()) {
        set_status_message("Already connecting...");
        return;
    }
    if (remote_active_) remote_disconnect();
    remote_target_ = ssh_config_.resolve(target);
    SSHConfig cfg;
    cfg.host = remote_target_.hostname;
    cfg.port = remote_target_.port;
    cfg.user = remote_target_.user;
    cfg.identity_files = remote_target_.identity_files;
    cfg.known_hosts = remote_target_.known_hosts;
    cfg.trust_new_host = trust_host;
    cfg.password = secret;
    cfg.key_passphrase = secret;
    set_status_message("Connecting to " + remote_target_.display() + "...", 30.0f);
    connect_job_ = std::async(std::launch::async, [this, cfg] {
        auto st = ssh_.connect(cfg);  // the session is only touched by this thread until the job lands
        request_redraw();
        return st;
    });
}

void App::poll_remote_connect() {
    if (!connect_job_.valid() || connect_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    SSHConnectStatus st = connect_job_.get();
    std::string target = remote_target_.alias;
    switch (st) {
    case SSHConnectStatus::Ok: {
        std::string home = ssh_.home_dir();
        set_status_message("Connected to " + remote_target_.display());
        if (pending_workspace_) {  // opening a Remote-SSH workspace: its folder is known
            Workspace ws = *pending_workspace_;
            open_remote_folder(fs::path(parse_ssh_folder(ws.folders.front())->second));
            finish_open_workspace(ws);
            break;
        }
        CommandPalette::PickOptions opts;
        opts.placeholder = "Remote folder to open (Enter = " + home + ")";
        opts.allow_free_text = true;
        opts.on_pick = [this, home](int, const std::string& query) {
            fs::path p = query.empty() ? fs::path(home) : fs::path(query);
            if (p.is_relative()) p = fs::path(home) / p;
            confirm_discard("Open Remote Folder", [this, p] { open_remote_folder(p.lexically_normal()); });
        };
        opts.on_cancel = [this] {
            std::string home = ssh_.home_dir();
            confirm_discard("Open Remote Folder", [this, home] { open_remote_folder(home); });
        };
        command_palette_.pick(std::move(opts));
        break;
    }
    case SSHConnectStatus::HostUnknown: {
        std::string fp = ssh_.host_fingerprint();
        CommandPalette::PickOptions opts;
        opts.placeholder = "Unknown host " + remote_target_.display() + "  key " + fp;
        opts.items = {"Trust this host key and connect", "Cancel"};
        opts.on_pick = [this, target](int i, const std::string&) {
            if (i == 0) remote_connect(target, true, "");
            else set_status_message("Connection cancelled");
        };
        command_palette_.pick(std::move(opts));
        break;
    }
    case SSHConnectStatus::AuthFailed: {
        CommandPalette::PickOptions opts;
        opts.placeholder = ssh_.needs_passphrase() ? "Passphrase for your SSH key"
                                                   : "Password for " + remote_target_.display();
        opts.allow_free_text = true;
        opts.password = true;
        opts.on_pick = [this, target](int, const std::string& secret) {
            if (!secret.empty()) remote_connect(target, false, secret);
        };
        command_palette_.pick(std::move(opts));
        break;
    }
    case SSHConnectStatus::HostChanged:
    case SSHConnectStatus::NetworkError:
        pending_workspace_.reset();
        set_status_message(ssh_.last_error(), 12.0f);
        break;
    }
}

void App::open_remote_folder(const fs::path& path) {
    if (workspace_) close_workspace(true);  // saves its session first
    if (!remote_active_) local_root_ = project_root_;
    remote_active_ = true;
    for (int i = static_cast<int>(tab_bar_.tabs().size()) - 1; i >= 0; i--) tab_bar_.close_tab(i);
    vfs_ = std::make_unique<SftpVfs>(ssh_);
    project_root_ = path;
    if (lsp_.is_running()) lsp_.stop();
    git_.close();
    git_statuses_.clear();
    file_tree_.set_git_status({});
    scm_.set_state({}, "", {});
    status_bar_.set_git_branch("");
    status_bar_.set_git_sync(0, 0);
    status_bar_.set_remote("SSH: " + remote_target_.alias);
    file_tree_.set_root(project_root_, vfs_.get());
    scan_project_files();
    command_palette_.set_root(project_root_);
    command_palette_.set_file_list(all_files_);
    search_.set_root(project_root_);
    new_file();

    // Remote shell via the system ssh client, so agent, ProxyJump etc. just work.
    std::vector<std::string> argv = {"ssh"};
    std::error_code ec;
    if (!remote_target_.config_file.empty() && fs::exists(remote_target_.config_file, ec))
        argv.insert(argv.end(), {"-F", remote_target_.config_file.string()});
    argv.insert(argv.end(), {"-o", "UserKnownHostsFile=" + remote_target_.known_hosts, "-t"});
    // A config alias carries its own User/Port/IdentityFile; a typed target needs them spelled out.
    if (remote_target_.from_config) argv.push_back(remote_target_.alias);
    else argv.insert(argv.end(), {"-p", std::to_string(remote_target_.port),
                                  remote_target_.user + "@" + remote_target_.hostname});
    argv.push_back("cd " + shell_quote(path.generic_string()) + " && exec \"$SHELL\" -l");
    terminal_argv_ = argv;
    terminal_.spawn(argv);
    set_status_message("Opened " + path.string() + " on " + remote_target_.display());
    if (!pending_workspace_) update_workspace_hint(true);
    update_window_title();
}

// Starts, retargets or stops the system monitor to match the current state, and
// hands its latest snapshot to the strip. Cheap enough to run every frame.
void App::update_monitor() {
    std::string key;
    if (remote_active_ && ssh_.is_connected() && remote_monitor_) key = "ssh:" + remote_target_.display();
    else if (!remote_active_ && local_monitor_) key = "local";
    std::string folder = project_root_.generic_string();
    if (key != monitor_key_) {
        monitor_.stop();
        monitor_bar_.clear();
        monitor_key_ = key;
        monitor_folder_ = folder;
        if (key == "local") {
            monitor_bar_.set_label("local");
            monitor_.start_local(folder);
        } else if (!key.empty()) {
            // A second connection with the main session's credentials, pinned to
            // the host key the user already accepted; it never shares ssh_.
            SSHConfig cfg = ssh_.config();
            cfg.trust_new_host = false;
            cfg.pinned_fingerprint = ssh_.host_fingerprint();
            cfg.timeout_ms = 8000;
            monitor_bar_.set_label(remote_target_.user + "@" + remote_target_.alias);
            monitor_.start_remote(cfg, folder);
        }
        request_redraw();
    }
    monitor_.reap();
    if (!monitor_.running()) return;
    if (folder != monitor_folder_) {
        monitor_folder_ = folder;
        monitor_.set_folder(folder);
    }
    monitor_.set_detail(monitor_bar_.detail_open());
    SysMonitor::Snapshot snap;
    if (monitor_.poll(monitor_bar_.version(), snap)) monitor_bar_.set_snapshot(snap);
}

void App::remote_disconnect() {
    if (!remote_active_) return;
    if (workspace_) close_workspace(true);
    remote_active_ = false;
    for (int i = static_cast<int>(tab_bar_.tabs().size()) - 1; i >= 0; i--) tab_bar_.close_tab(i);
    ssh_.disconnect();
    vfs_ = std::make_unique<LocalVfs>();
    project_root_ = local_root_;
    status_bar_.set_remote("");
    git_.open(project_root_);
    file_tree_.set_root(project_root_, vfs_.get());
    scan_project_files();
    command_palette_.set_root(project_root_);
    command_palette_.set_file_list(all_files_);
    search_.set_root(project_root_);
    terminal_argv_.clear();
    terminal_.spawn({}, project_root_);
    new_file();
    if (git_.is_open()) {
        status_bar_.set_git_branch(git_.branch_name());
        request_git_refresh();
    }
    set_status_message("Disconnected");
    update_workspace_hint(false);
    update_window_title();
}

} // namespace oh
