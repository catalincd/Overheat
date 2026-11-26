#include "debug/debugger.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

namespace oh {

void request_redraw();

const char* debug_state_name(DebugState s) {
    switch (s) {
        case DebugState::Idle: return "idle";
        case DebugState::Building: return "building";
        case DebugState::Launching: return "launching";
        case DebugState::Running: return "running";
        case DebugState::Stopped: return "stopped";
    }
    return "idle";
}

namespace {
constexpr size_t MAX_CONSOLE = 5000;
constexpr int64_t PAGE = 500;  // indexed children fetched per request

std::string run_capture(const std::vector<std::string>& argv) {
    platform::Process p;
    if (!p.start(argv)) return {};
    std::string out;
    char buf[4096];
    long n;
    while ((n = p.read(buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    p.wait();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// "core::option::Option<&rustdemo::Player>" -> "Option<&Player>"
std::string short_type(const std::string& t) {
    static const std::regex ns(R"(\b[A-Za-z_][A-Za-z0-9_]*::)");
    std::string s = std::regex_replace(t, ns, "");
    if (s.size() > 48) s = s.substr(0, 45) + "...";
    return s;
}

// Key/value children ([0]=key, [1]=value ... from libstdc++'s map printers, or
// keyN/valN from Rust's HashMap printer) become one row per entry.
void pair_map_children(const std::string& parent_value, std::vector<DebugVariable>& vars) {
    bool cpp_map = starts_with(parent_value, "std::map") || starts_with(parent_value, "std::unordered_map") ||
                   starts_with(parent_value, "std::multimap") || starts_with(parent_value, "std::unordered_multimap");
    bool rust_map = vars.size() >= 2 && vars[0].name == "key0" && vars[1].name == "val0";
    if ((!cpp_map && !rust_map) || vars.size() % 2) return;
    std::vector<DebugVariable> out;
    for (size_t i = 0; i + 1 < vars.size(); i += 2) {
        DebugVariable v = vars[i + 1];
        v.name = "[" + vars[i].value + "]";
        out.push_back(std::move(v));
    }
    vars = std::move(out);
}

std::string first_line(const std::string& s) {
    std::string out = s.substr(0, s.find('\n'));
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return out;
}
} // namespace

// ---------------------------------------------------------------- build / terminal jobs

struct Debugger::BuildJob {
    platform::Process proc;
    std::thread reader;
    std::mutex mutex;
    std::string pending;  // output not yet shown
    std::atomic<bool> done{false};
    int code = -1;
    std::function<void(bool)> on_done;
    ~BuildJob() {
        if (!done) proc.terminate();
        if (reader.joinable()) reader.join();
    }
};

struct Debugger::TerminalJob {
    platform::Pty pty;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::string pending;
    ~TerminalJob() {
        stop = true;
        pty.kill();
        if (reader.joinable()) reader.join();
    }
};

Debugger::Debugger() = default;
Debugger::~Debugger() { shutdown(); }

void Debugger::shutdown() {
    if (dap_) {
        if (dap_->running() && configured_) {
            dap_->request("disconnect", Json::object({{"terminateDebuggee", !session_.attach}}));
            for (int i = 0; i < 50 && !dap_->exited(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        dap_->stop();
        dap_.reset();
    }
    if (debuggee_pid_ > 0 && !session_.attach && platform::process_alive(debuggee_pid_))
        platform::kill_process(debuggee_pid_);
    debuggee_pid_ = 0;
    build_.reset();
    term_.reset();
    if (state_dirty_ || saved_generation_ != bps_.generation()) save_state();
    state_ = DebugState::Idle;
}

// ---------------------------------------------------------------- persistence

void Debugger::set_root(const fs::path& root, const fs::path& config_dir) {
    if (root == root_ && !state_file_.empty()) return;
    if (!state_file_.empty() && (state_dirty_ || saved_generation_ != bps_.generation())) save_state();
    root_ = root;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : root.string()) h = (h ^ c) * 1099511628211ull;
    char name[40];
    std::snprintf(name, sizeof name, "%016llx.debug", static_cast<unsigned long long>(h));
    state_file_ = config_dir / "debug" / name;
    configs_loaded_ = false;
    configs_.clear();
    selected_config_ = -1;
    load_state();
}

void Debugger::load_state() {
    watches_.clear();
    selected_name_.clear();
    std::ifstream in(state_file_);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    bps_.deserialize(text);
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (starts_with(line, "watch\t")) watches_.push_back(DebugWatch{line.substr(6), {}, {}, 0, false});
        else if (starts_with(line, "config\t")) selected_name_ = line.substr(7);
    }
    // A Rust project starts with "Rust panics" on, as CodeLLDB does.
    std::error_code ec;
    if (!fs::exists(state_file_, ec) && fs::exists(root_ / "Cargo.toml", ec)) bps_.set_filter("rust_panic", true);
    saved_generation_ = bps_.generation();
    state_dirty_ = false;
}

void Debugger::save_state() {
    if (state_file_.empty()) return;
    std::error_code ec;
    fs::create_directories(state_file_.parent_path(), ec);
    std::ofstream out(state_file_);
    out << "root\t" << root_.string() << "\n";
    out << bps_.serialize();
    for (auto& w : watches_) out << "watch\t" << w.expr << "\n";
    if (!selected_name_.empty()) out << "config\t" << selected_name_ << "\n";
    saved_generation_ = bps_.generation();
    state_dirty_ = false;
}

// ---------------------------------------------------------------- configurations

const std::vector<LaunchConfig>& Debugger::configs() {
    if (!configs_loaded_) refresh_configs();
    return configs_;
}

void Debugger::refresh_configs() {
    configs_loaded_ = true;
    config_problems_.clear();
    configs_ = discover_launch_configs(root_, &config_problems_);
    selected_config_ = -1;
    for (size_t i = 0; i < configs_.size(); i++)
        if (configs_[i].name == selected_name_) selected_config_ = static_cast<int>(i);
    if (selected_config_ < 0 && !configs_.empty()) selected_config_ = 0;
}

void Debugger::select_config(int i) {
    configs();
    if (i < 0 || i >= static_cast<int>(configs_.size())) return;
    selected_config_ = i;
    selected_name_ = configs_[static_cast<size_t>(i)].name;
    state_dirty_ = true;
}

std::string Debugger::selected_config_name() {
    configs();
    if (selected_config_ < 0) return {};
    return configs_[static_cast<size_t>(selected_config_)].name;
}

// ---------------------------------------------------------------- console

void Debugger::append_output(ConsoleEntry::Kind k, const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string piece = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!piece.empty() && piece.back() == '\r') piece.pop_back();
        if (console_open_line_ && console_open_kind_ == k && !console_.empty()) console_.back().text += piece;
        else console_.push_back({k, piece});
        console_open_line_ = nl == std::string::npos;
        console_open_kind_ = k;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    while (console_.size() > MAX_CONSOLE) console_.pop_front();
    console_gen_++;
}

void Debugger::log(ConsoleEntry::Kind k, const std::string& text) {
    console_open_line_ = false;
    append_output(k, text + "\n");
}

void Debugger::clear_console() {
    console_.clear();
    console_open_line_ = false;
    console_gen_++;
}

// ---------------------------------------------------------------- session control

std::vector<std::string> Debugger::adapter_command(const LaunchConfig& c) {
    auto setting = [&](const char* key, const char* fallback) {
        std::string v = host_.setting ? host_.setting(key) : "";
        return v.empty() ? std::string(fallback) : v;
    };
    switch (c.adapter) {
        case AdapterKind::Gdb:
        case AdapterKind::RustGdb: {
            std::string gdb = !c.adapter_path.empty() ? c.adapter_path : setting("debug.gdb_path", "gdb");
            std::vector<std::string> argv{gdb, "-q", "-iex", "set debuginfod enabled off"};
            if (c.adapter == AdapterKind::RustGdb) {
                // What rust-gdb does, without needing a shell: the toolchain's printer
                // modules on gdb's path and auto-load list, and std sources mapped.
                if (rust_sysroot_.empty()) rust_sysroot_ = run_capture({setting("debug.rustc_path", "rustc"), "--print", "sysroot"});
                if (!rust_sysroot_.empty()) {
                    fs::path etc = fs::path(rust_sysroot_) / "lib" / "rustlib" / "etc";
                    std::string hash;
                    std::string vv = run_capture({setting("debug.rustc_path", "rustc"), "-vV"});
                    if (auto p = vv.find("commit-hash: "); p != std::string::npos) hash = first_line(vv.substr(p + 13));
                    argv.insert(argv.end(), {"--directory=" + etc.string(), "-iex", "add-auto-load-safe-path " + etc.string(),
                                             "-iex", "python import sys; sys.path.insert(0, r'" + etc.string() + "')"});
                    std::string src = (fs::path(rust_sysroot_) / "lib" / "rustlib" / "src" / "rust").string();
                    if (!hash.empty()) argv.insert(argv.end(), {"-iex", "set substitute-path /rustc/" + hash + " " + src});
                    // Distribution builds (Arch, Fedora) record their own build path instead.
                    if (auto p = vv.find("release: "); p != std::string::npos)
                        argv.insert(argv.end(), {"-iex", "set substitute-path /usr/src/debug/rust/rustc-" +
                                                             first_line(vv.substr(p + 9)) + "-src " + src});
                    // Step Into goes to your code, not through Deref/Iterator glue in std.
                    if (setting("debug.rust_step_into_std", "false") != "true")
                        argv.insert(argv.end(), {"-iex", "skip -rfu ^(core|alloc|std)::"});
                }
            }
            argv.insert(argv.end(), {"-i", "dap"});
            // Loading the program up front lets breakpoints bind (and verify) as soon
            // as they are set, instead of staying pending until launch.
            if (!c.program.empty()) argv.push_back(c.program);
            return argv;
        }
        case AdapterKind::LldbDap:
            return {!c.adapter_path.empty() ? c.adapter_path : setting("debug.lldb_dap_path", "lldb-dap")};
        case AdapterKind::CodeLldb: {
            std::vector<std::string> argv{!c.adapter_path.empty() ? c.adapter_path : setting("debug.codelldb_path", "codelldb")};
            for (auto& a : split_command(setting("debug.codelldb_args", ""))) argv.push_back(a);
            return argv;
        }
    }
    return {"gdb", "-i", "dap"};
}

void Debugger::start_or_continue() {
    if (state_ == DebugState::Stopped) {
        continue_();
        return;
    }
    if (active()) return;
    configs();
    if (configs_.empty()) {
        if (host_.status) host_.status("No launch configuration: add .overheat/launch.toml (Debug: Open launch.toml)");
        log(ConsoleEntry::Error, "No launch configuration found in " + root_.string() +
                                     " (.overheat/launch.toml, .vscode/launch.json, Cargo or a configured CMake build)");
        for (auto& p : config_problems_) log(ConsoleEntry::Error, p);
        return;
    }
    start(selected_config_ < 0 ? 0 : selected_config_);
}

void Debugger::start(int index) {
    configs();
    if (index < 0 || index >= static_cast<int>(configs_.size())) return;
    if (active()) {  // restart with another configuration
        pending_start_ = index;
        stop();
        return;
    }
    select_config(index);
    session_ = configs_[static_cast<size_t>(index)];
    LaunchVars vars{root_, host_.current_file ? host_.current_file() : fs::path()};
    resolve_config(session_, vars);
    clear_stopped_state();
    bps_.clear_adapter_state();
    stop_description_.clear();
    exited_reported_ = false;
    debuggee_pid_ = 0;
    caps_ = Json();
    if (host_.session_started) host_.session_started();
    log(ConsoleEntry::Info, "Starting " + session_.name);

    if (session_.attach && session_.pid <= 0) {
        if (!host_.prompt) return;
        state_ = DebugState::Launching;
        host_.prompt("Process id to attach to", "", [this](const std::string& text) {
            long pid = std::strtol(text.c_str(), nullptr, 10);
            if (pid <= 0) {
                log(ConsoleEntry::Error, "Attach cancelled: no process id");
                state_ = DebugState::Idle;
                if (host_.session_ended) host_.session_ended();
                return;
            }
            session_.pid = pid;
            launch_adapter();
        });
        return;
    }
    if (!session_.build_command.empty() && !session_.attach) {
        state_ = DebugState::Building;
        if (host_.save_all) host_.save_all();
        auto done = [this](bool ok) {
            if (state_ != DebugState::Building) return;  // stopped meanwhile
            if (!ok) {
                log(ConsoleEntry::Error, "Build failed; not launching.");
                if (host_.status) host_.status("Build failed");
                state_ = DebugState::Idle;
                if (host_.session_ended) host_.session_ended();
                return;
            }
            launch_adapter();
        };
        if (build_hook_) build_hook_(session_, done);
        else run_build(session_, done);
        return;
    }
    launch_adapter();
}

void Debugger::run_build(const LaunchConfig& c, std::function<void(bool)> done) {
    auto argv = split_command(c.build_command);
    log(ConsoleEntry::Info, "> " + c.build_command + "   (in " + c.build_cwd + ")");
    build_ = std::make_unique<BuildJob>();
    build_->on_done = std::move(done);
    if (argv.empty() || !build_->proc.start(argv, c.build_cwd, true)) {
        log(ConsoleEntry::Error, "Could not run the build command: " + c.build_command);
        auto fn = std::move(build_->on_done);
        build_->done = true;
        build_.reset();
        fn(false);
        return;
    }
    BuildJob* job = build_.get();
    job->reader = std::thread([job] {
        char buf[4096];
        long n;
        while ((n = job->proc.read(buf, sizeof buf)) > 0) {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->pending.append(buf, static_cast<size_t>(n));
            request_redraw();
        }
        job->code = job->proc.wait();
        job->done = true;
        request_redraw();
    });
}

void Debugger::poll_build() {
    if (!build_) return;
    std::string out;
    {
        std::lock_guard<std::mutex> lock(build_->mutex);
        out.swap(build_->pending);
    }
    if (!out.empty()) append_output(ConsoleEntry::Out, out);
    if (!build_->done) return;
    if (build_->reader.joinable()) build_->reader.join();
    {
        std::lock_guard<std::mutex> lock(build_->mutex);
        if (!build_->pending.empty()) append_output(ConsoleEntry::Out, build_->pending);
    }
    int code = build_->code;
    auto fn = std::move(build_->on_done);
    build_.reset();
    console_open_line_ = false;
    log(code == 0 ? ConsoleEntry::Info : ConsoleEntry::Error, "Build finished with exit code " + std::to_string(code));
    if (fn) fn(code == 0);
}

void Debugger::launch_adapter() {
    if (!session_.attach) {
        std::error_code ec;
        if (session_.program.empty() || !fs::exists(session_.program, ec)) {
            log(ConsoleEntry::Error, "Program not found: " + session_.program);
            if (host_.status) host_.status("Program not found: " + session_.program);
            state_ = DebugState::Idle;
            if (host_.session_ended) host_.session_ended();
            return;
        }
    }
    auto argv = adapter_command(session_);
    std::string cmdline;
    for (auto& a : argv) cmdline += (cmdline.empty() ? "" : " ") + a;
    log(ConsoleEntry::Info, "Adapter: " + cmdline);
    dap_ = std::make_unique<DapClient>();
    if (host_.setting && host_.setting("debug.trace") == "true")
        dap_->set_trace([this](bool out, const std::string& m) { log(ConsoleEntry::Info, (out ? "-> " : "<- ") + m); });
    dap_->set_on_event([this](const Json& ev) { on_event(ev); });
    dap_->set_on_request([this](const Json& req) { on_reverse_request(req); });
    dap_->set_on_exit([this] {
        if (state_ != DebugState::Idle) log(ConsoleEntry::Info, "Debug adapter exited");
        end_session();
    });
    if (!dap_->start(argv, session_.cwd)) {
        log(ConsoleEntry::Error, "Could not start the debug adapter: " + argv[0] +
                                     " (set debug." + (session_.adapter == AdapterKind::LldbDap ? "lldb_dap_path"
                                                       : session_.adapter == AdapterKind::CodeLldb ? "codelldb_path"
                                                       : "gdb_path") + " in settings.toml)");
        dap_.reset();
        state_ = DebugState::Idle;
        if (host_.session_ended) host_.session_ended();
        return;
    }
    state_ = DebugState::Launching;
    configured_ = false;
    Json init = Json::object({{"clientID", "overheat"}, {"clientName", "Overheat"},
                              {"adapterID", adapter_name(session_.adapter)}, {"locale", "en-US"},
                              {"linesStartAt1", true}, {"columnsStartAt1", true}, {"pathFormat", "path"},
                              {"supportsVariableType", true}, {"supportsVariablePaging", true},
                              {"supportsRunInTerminalRequest", true}, {"supportsMemoryReferences", false},
                              {"supportsProgressReporting", false}, {"supportsInvalidatedEvent", false}});
    dap_->request("initialize", init, [this](const Json& r) {
        if (!r["success"].boolean()) {
            log(ConsoleEntry::Error, "initialize failed: " + r["message"].str());
            end_session();
            return;
        }
        caps_ = r["body"];
        // launch / attach; the adapter answers once configurationDone is in (gdb).
        Json args = Json::object({{"name", session_.name}, {"program", session_.program}});
        if (session_.attach) {
            args.set("pid", static_cast<int64_t>(session_.pid));
            if (session_.adapter == AdapterKind::LldbDap) args.set("attachCommands", Json());
        } else {
            Json argv = Json::array();
            for (auto& a : session_.args) argv.push(a);
            args.set("args", argv);
            args.set("cwd", session_.cwd);
            if (session_.adapter == AdapterKind::Gdb || session_.adapter == AdapterKind::RustGdb) {
                // gdb replaces the whole environment with `env`: send ours plus the overrides.
                if (!session_.env.empty()) {
                    Json env = Json::object();
                    for (auto& kv : platform::environment()) {
                        auto eq = kv.find('=');
                        if (eq != std::string::npos && eq > 0) env.set(kv.substr(0, eq), kv.substr(eq + 1));
                    }
                    for (auto& [k, v] : session_.env) env.set(k, v);
                    args.set("env", env);
                }
                args.set("stopAtBeginningOfMainSubprogram", session_.stop_at_entry);
            } else if (session_.adapter == AdapterKind::LldbDap) {
                Json env = Json::array();
                for (auto& [k, v] : session_.env) env.push(k + "=" + v);
                args.set("env", env);
                args.set("stopOnEntry", session_.stop_at_entry);
            } else {
                Json env = Json::object();
                for (auto& [k, v] : session_.env) env.set(k, v);
                args.set("env", env);
                args.set("stopOnEntry", session_.stop_at_entry);
                args.set("terminal", "console");
                if (session_.language == "rust") args.set("sourceLanguages", Json::array({Json("rust")}));
            }
        }
        dap_->request(session_.attach ? "attach" : "launch", args, [this](const Json& lr) {
            if (!lr["success"].boolean()) {
                log(ConsoleEntry::Error, std::string(session_.attach ? "attach" : "launch") + " failed: " + lr["message"].str());
                if (host_.status) host_.status("Debug: " + lr["message"].str());
                end_session();
                return;
            }
            if (state_ == DebugState::Launching) state_ = DebugState::Running;
        });
        // Adapters that never send "initialized" before launch completes still need configuring.
    });
}

void Debugger::configure() {
    if (configured_ || !dap_) return;
    for (auto& f : bps_.files()) send_breakpoints(f);
    send_function_breakpoints();
    send_exception_breakpoints();
    configured_ = true;
    if (caps_["supportsConfigurationDoneRequest"].boolean(true))
        dap_->request("configurationDone", Json::object(), [this](const Json& r) {
            if (!r["success"].boolean()) log(ConsoleEntry::Error, "configurationDone: " + r["message"].str());
            if (state_ == DebugState::Launching) state_ = DebugState::Running;
        });
}

bool Debugger::uses_panic_filter() const {
    for (auto& f : caps_["exceptionBreakpointFilters"].items())
        if (f["filter"].str() == "rust_panic") return true;
    return false;
}

void Debugger::send_breakpoints(const fs::path& path) {
    if (!dap_ || !dap_->running()) return;
    auto list = bps_.in_file(path);
    Json bps = Json::array();
    std::vector<SourceBreakpoint*> sent;
    for (SourceBreakpoint* b : list) {
        if (!b->enabled) {
            b->adapter_id = -1;
            b->verified = false;
            continue;
        }
        Json o = Json::object({{"line", static_cast<int64_t>(b->line + 1)}});
        if (!b->condition.empty()) o.set("condition", b->condition);
        if (!b->hit_condition.empty()) {
            // VS Code's meaning: "N" or ">= N" breaks on the Nth hit. gdb reads the value
            // as an ignore count (it breaks on hit N+1), so send it one lower.
            std::string hc = b->hit_condition;
            bool gdb = session_.adapter == AdapterKind::Gdb || session_.adapter == AdapterKind::RustGdb;
            if (gdb) {
                std::string t = hc;
                int adjust = 1;
                if (starts_with(t, ">=") || starts_with(t, "==")) t = t.substr(2);
                else if (starts_with(t, ">")) t = t.substr(1), adjust = 0;
                char* end = nullptr;
                long n = std::strtol(t.c_str(), &end, 10);
                while (end && *end == ' ') end++;
                if (end && !*end && n > 0) hc = std::to_string(std::max(0L, n - adjust));
            }
            o.set("hitCondition", hc);
        }
        if (!b->log_message.empty()) o.set("logMessage", b->log_message);
        bps.push(o);
        sent.push_back(b);
    }
    // Lines identify our breakpoints in the reply (the vector may change meanwhile).
    std::vector<size_t> lines;
    for (auto* b : sent) lines.push_back(b->line);
    Json args = Json::object({{"source", Json::object({{"path", path.string()}, {"name", path.filename().string()}})},
                              {"breakpoints", bps}, {"sourceModified", false}});
    dap_->request("setBreakpoints", args, [this, path, lines](const Json& r) {
        const Json& res = r["body"]["breakpoints"];
        for (size_t i = 0; i < lines.size(); i++) {
            SourceBreakpoint* b = nullptr;
            for (auto& sb : bps_.source())
                if (sb.path == path && sb.line == lines[i]) b = &sb;
            if (!b) continue;
            if (!r["success"].boolean()) {
                b->verified = false;
                b->message = r["message"].str();
                continue;
            }
            const Json& o = res[i];
            b->adapter_id = o["id"].integer(-1);
            b->verified = o["verified"].boolean();
            b->message = o["message"].str_or(o["reason"].str());
            if (o["line"].is_number()) b->actual_line = static_cast<long>(o["line"].integer()) - 1;
        }
        bps_.bump();
    });
}

void Debugger::send_function_breakpoints() {
    if (!dap_ || !dap_->running()) return;
    if (!caps_["supportsFunctionBreakpoints"].boolean()) return;
    Json list = Json::array();
    std::vector<std::string> names;
    for (auto& f : bps_.functions()) {
        if (!f.enabled) continue;
        Json o = Json::object({{"name", f.name}});
        if (!f.condition.empty()) o.set("condition", f.condition);
        list.push(o);
        names.push_back(f.name);
    }
    bool panic = bps_.filter_on("rust_panic") && !uses_panic_filter();
    if (panic) {
        // Newer toolchains name it __rustc::rust_panic; older ones export plain rust_panic.
        for (const char* n : {"__rustc::rust_panic", "rust_panic"}) {
            list.push(Json::object({{"name", n}}));
            names.push_back("\x01rust_panic");
        }
    }
    dap_->request("setFunctionBreakpoints", Json::object({{"breakpoints", list}}), [this, names](const Json& r) {
        const Json& res = r["body"]["breakpoints"];
        panic_bp_ids_.clear();
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] == "\x01rust_panic") {
                panic_bp_ids_.push_back(res[i]["id"].integer(-1));
                continue;
            }
            for (auto& f : bps_.functions())
                if (f.name == names[i]) {
                    f.adapter_id = res[i]["id"].integer(-1);
                    f.verified = res[i]["verified"].boolean();
                }
        }
        bps_.bump();
    });
}

void Debugger::send_exception_breakpoints() {
    if (!dap_ || !dap_->running()) return;
    const Json& offered = caps_["exceptionBreakpointFilters"];
    if (!offered.is_array()) return;
    Json filters = Json::array();
    for (auto& f : offered.items())
        if (bps_.filter_on(f["filter"].str())) filters.push(f["filter"].str());
    dap_->request("setExceptionBreakpoints", Json::object({{"filters", filters}}));
}

void Debugger::toggle_breakpoint(const fs::path& path, size_t line) {
    bps_.toggle(path, line);
    breakpoints_changed(path);
}

void Debugger::breakpoints_changed(const fs::path& path) {
    bps_.bump();
    if (configured_) send_breakpoints(path);
}

void Debugger::function_breakpoints_changed() {
    bps_.bump();
    if (configured_) send_function_breakpoints();
}

void Debugger::set_exception_filter(const std::string& id, bool on) {
    bps_.set_filter(id, on);
    if (!configured_) return;
    if (id == "rust_panic" && !uses_panic_filter()) send_function_breakpoints();
    else send_exception_breakpoints();
}

std::vector<Debugger::Filter> Debugger::exception_filters() const {
    std::vector<Filter> out;
    const Json& offered = caps_["exceptionBreakpointFilters"];
    if (offered.is_array()) {
        for (auto& f : offered.items()) {
            std::string label = f["label"].str();
            if (starts_with(label, "Ada ")) continue;  // gdb offers Ada filters to every program
            if (f["filter"].str() == "rust_panic") continue;
            out.push_back({f["filter"].str(), label});
        }
    } else {  // gdb's filters, shown before a session starts
        out.push_back({"throw", "C++ exceptions, when thrown"});
        out.push_back({"rethrow", "C++ exceptions, when re-thrown"});
        out.push_back({"catch", "C++ exceptions, when caught"});
    }
    out.push_back({"rust_panic", "Rust panics"});
    return out;
}

void Debugger::on_edit(const fs::path& path, const LineEdit& e) {
    if (bps_.shift_for_edit(path, e)) {
        // Like VS Code: the adapter keeps the old lines until the file is saved and
        // the breakpoints are sent again; the gutter follows the text right away.
    }
}

void Debugger::set_running() {
    state_ = DebugState::Running;
    stop_epoch_++;
    clear_stopped_state();
}

void Debugger::clear_stopped_state() {
    for (auto& t : threads_) {
        t.frames.clear();
        t.frames_loaded = false;
        t.loading = false;
    }
    scopes_.clear();
    children_.clear();
    summaries_.clear();
    for (auto& w : watches_) {
        w.ref = 0;
    }
    generation_++;
}

void Debugger::continue_() {
    if (state_ != DebugState::Stopped || !dap_) return;
    dap_->request("continue", Json::object({{"threadId", sel_thread_}}));
    stop_description_.clear();
    set_running();
}

void Debugger::step(const char* command) {
    if (state_ != DebugState::Stopped || !dap_) return;
    dap_->request(command, Json::object({{"threadId", sel_thread_}}));
    set_running();
}

void Debugger::step_over() { step("next"); }
void Debugger::step_into() { step("stepIn"); }
void Debugger::step_out() { step("stepOut"); }

void Debugger::pause() {
    if (state_ != DebugState::Running || !dap_) return;
    int64_t tid = sel_thread_ ? sel_thread_ : (threads_.empty() ? 1 : threads_.front().id);
    dap_->request("pause", Json::object({{"threadId", tid}}));
}

void Debugger::stop() {
    if (state_ == DebugState::Building) {
        build_.reset();  // terminates the build
        log(ConsoleEntry::Info, "Build cancelled");
        state_ = DebugState::Idle;
        if (host_.session_ended) host_.session_ended();
        if (pending_start_) {
            int i = *pending_start_;
            pending_start_.reset();
            start(i);
        }
        return;
    }
    if (!dap_) {
        if (state_ != DebugState::Idle) end_session();
        return;
    }
    if (stopping_) {  // second Stop: don't wait for the adapter
        end_session();
        return;
    }
    stopping_ = true;
    stop_deadline_ = Clock::now() + std::chrono::seconds(3);
    if (session_.attach)
        dap_->request("disconnect", Json::object({{"terminateDebuggee", false}}), [this](const Json&) { end_session(); });
    else
        dap_->request("disconnect", Json::object({{"terminateDebuggee", true}, {"restart", false}}),
                      [this](const Json&) { end_session(); });
}

void Debugger::restart() {
    if (!active()) {
        start_or_continue();
        return;
    }
    if (caps_["supportsRestartRequest"].boolean() && dap_ && state_ != DebugState::Building) {
        log(ConsoleEntry::Info, "Restarting");
        dap_->request("restart", Json::object());
        set_running();
        return;
    }
    // gdb has no restart request: end this session and launch again (rebuilding).
    pending_start_ = selected_config_ < 0 ? 0 : selected_config_;
    stop();
}

void Debugger::end_session() { teardown_ = true; }

void Debugger::finish_teardown() {
    teardown_ = false;
    stopping_ = false;
    if (dap_) {
        dap_->stop();
        dap_.reset();
    }
    term_.reset();
    if (debuggee_pid_ > 0 && !session_.attach && platform::process_alive(debuggee_pid_)) {
        platform::kill_process(debuggee_pid_);
        log(ConsoleEntry::Info, "Killed debuggee process " + std::to_string(debuggee_pid_));
    }
    debuggee_pid_ = 0;
    bool was_active = state_ != DebugState::Idle;
    state_ = DebugState::Idle;
    configured_ = false;
    threads_.clear();
    sel_thread_ = 0;
    sel_frame_ = 0;
    clear_stopped_state();
    bps_.clear_adapter_state();
    stop_description_.clear();
    panic_bp_ids_.clear();
    if (was_active) {
        log(ConsoleEntry::Info, "Debug session ended");
        if (host_.session_ended) host_.session_ended();
    }
    if (pending_start_) {
        int i = *pending_start_;
        pending_start_.reset();
        start(i);
    }
}

// ---------------------------------------------------------------- adapter messages

void Debugger::update() {
    poll_build();
    if (term_) {
        std::string out;
        {
            std::lock_guard<std::mutex> lock(term_->mutex);
            out.swap(term_->pending);
        }
        if (!out.empty()) append_output(ConsoleEntry::Out, out);
    }
    if (dap_) dap_->poll();
    if (stopping_ && !teardown_ && Clock::now() > stop_deadline_) {
        log(ConsoleEntry::Info, "The adapter did not answer disconnect; stopping it");
        teardown_ = true;
    }
    if (teardown_) finish_teardown();
    if (saved_generation_ != bps_.generation() || state_dirty_) save_state();
}

void Debugger::on_event(const Json& msg) {
    const std::string& ev = msg["event"].str();
    const Json& body = msg["body"];
    if (ev == "initialized") {
        configure();
    } else if (ev == "stopped") {
        on_stopped(body);
    } else if (ev == "continued") {
        if (state_ == DebugState::Stopped) {
            stop_description_.clear();
            set_running();
        }
    } else if (ev == "exited") {
        exited_reported_ = true;
        std::string msg = stopping_ ? "Program stopped" : "Program exited with code " + std::to_string(body["exitCode"].integer());
        log(ConsoleEntry::Info, msg);
        if (host_.status) host_.status(msg);
    } else if (ev == "terminated") {
        end_session();
    } else if (ev == "output") {
        const std::string& cat = body["category"].str();
        if (cat == "telemetry") return;
        ConsoleEntry::Kind k = cat == "stderr" ? ConsoleEntry::Err
                             : cat == "console" ? ConsoleEntry::Info
                             : cat == "important" ? ConsoleEntry::Important : ConsoleEntry::Out;
        std::string text = body["output"].str();
        // gdb decorates its errors with an emoji most fonts lack.
        for (size_t p; (p = text.find("\xE2\x9D\x8C\xEF\xB8\x8F ")) != std::string::npos;) text.erase(p, 7);
        append_output(k, text);
    } else if (ev == "breakpoint") {
        apply_breakpoint_update(body["breakpoint"]);
    } else if (ev == "process") {
        debuggee_pid_ = static_cast<long>(body["systemProcessId"].integer(0));
    } else if (ev == "thread") {
        if (body["reason"].str() == "exited") {
            int64_t id = body["threadId"].integer();
            std::erase_if(threads_, [&](const DebugThread& t) { return t.id == id; });
            generation_++;
        } else if (state_ == DebugState::Stopped) {
            load_threads(sel_thread_);
        }
    }
}

void Debugger::apply_breakpoint_update(const Json& bp) {
    int64_t id = bp["id"].integer(-1);
    if (id < 0) return;
    for (auto& b : bps_.source()) {
        if (b.adapter_id != id) continue;
        b.verified = bp["verified"].boolean(b.verified);
        if (bp["line"].is_number()) b.actual_line = static_cast<long>(bp["line"].integer()) - 1;
        b.message = bp["message"].str_or(bp["reason"].str());
        bps_.bump();
        return;
    }
    for (auto& f : bps_.functions())
        if (f.adapter_id == id) {
            f.verified = bp["verified"].boolean(f.verified);
            bps_.bump();
        }
}

void Debugger::on_stopped(const Json& body) {
    state_ = DebugState::Stopped;
    stop_epoch_++;
    clear_stopped_state();
    std::string reason = body["reason"].str();
    bool panic = false;
    for (auto& id : body["hitBreakpointIds"].items())
        if (std::find(panic_bp_ids_.begin(), panic_bp_ids_.end(), id.integer()) != panic_bp_ids_.end()) panic = true;
    if (reason == "exception" && body["text"].str().find("panic") != std::string::npos) panic = true;
    if (panic) stop_description_ = "Paused on Rust panic";
    else if (reason == "breakpoint" || reason == "function breakpoint") stop_description_ = "Paused on breakpoint";
    else if (reason == "step") stop_description_ = "Paused after step";
    else if (reason == "pause") stop_description_ = "Paused";
    else if (reason == "entry") stop_description_ = "Paused on entry";
    else if (reason == "exception") stop_description_ = "Paused on exception";
    else if (reason == "signal" || reason == "signal-received") stop_description_ = "Paused on signal";
    else stop_description_ = "Paused" + (reason.empty() ? std::string() : " (" + reason + ")");
    if (!body["description"].str().empty() && !panic && reason == "exception")
        stop_description_ = body["description"].str();
    if (!body["text"].str().empty()) log(ConsoleEntry::Important, body["text"].str());
    if (panic) log(ConsoleEntry::Important, "Rust panic: the stopped thread is panicking (see the call stack)");
    int64_t tid = body["threadId"].integer(0);
    if (tid) sel_thread_ = tid;
    load_threads(tid);
    if (host_.status) host_.status(stop_description_);
}

void Debugger::load_threads(int64_t focus) {
    if (!dap_) return;
    uint64_t epoch = stop_epoch_;
    dap_->request("threads", Json::object(), [this, focus, epoch](const Json& r) {
        if (epoch != stop_epoch_) return;
        std::vector<DebugThread> next;
        for (auto& t : r["body"]["threads"].items()) {
            DebugThread th;
            th.id = t["id"].integer();
            th.name = t["name"].str();
            for (auto& old : threads_)
                if (old.id == th.id) {
                    th.expanded = old.expanded;
                    th.frames = std::move(old.frames);
                    th.frames_loaded = old.frames_loaded;
                }
            next.push_back(std::move(th));
        }
        threads_ = std::move(next);
        int64_t tid = focus;
        if (!tid || !thread(tid)) tid = threads_.empty() ? 0 : threads_.front().id;
        sel_thread_ = tid;
        generation_++;
        if (!tid) return;
        for (auto& t : threads_) {
            if (t.id == tid) t.expanded = true;
            else if (t.expanded && !t.frames_loaded && !t.loading) load_frames(t.id);  // still open in the view
        }
        if (thread(tid)->frames_loaded) return;
        load_frames(tid, [this, tid] {
            // Select the first frame with source the user can read: a panic stops in
            // std's rust_panic, where the interesting frame is further down.
            const DebugThread* t = thread(tid);
            if (!t) return;
            int pick = -1, first_src = -1;
            std::error_code ec;
            for (size_t i = 0; i < t->frames.size(); i++) {
                const DebugFrame& f = t->frames[i];
                if (f.path.empty() || !fs::exists(f.path, ec)) continue;
                if (first_src < 0) first_src = static_cast<int>(i);
                std::string p = f.path.generic_string();
                bool in_root = !root_.empty() && p.rfind(root_.generic_string(), 0) == 0;
                if (in_root && pick < 0) pick = static_cast<int>(i);
            }
            bool top_has_source = !t->frames.empty() && first_src == 0;
            int idx = top_has_source ? 0 : (pick >= 0 ? pick : (first_src >= 0 ? first_src : 0));
            select_frame(tid, idx, true);
        });
    });
}

void Debugger::load_frames(int64_t tid, std::function<void()> then) {
    if (!dap_) return;
    for (auto& t : threads_)
        if (t.id == tid) t.loading = true;
    uint64_t epoch = stop_epoch_;
    dap_->request("stackTrace", Json::object({{"threadId", tid}, {"startFrame", 0}, {"levels", 100}}),
                  [this, tid, epoch, then](const Json& r) {
        if (epoch != stop_epoch_) return;
        for (auto& t : threads_) {
            if (t.id != tid) continue;
            t.loading = false;
            t.frames_loaded = true;
            t.frames.clear();
            for (auto& f : r["body"]["stackFrames"].items()) {
                DebugFrame fr;
                fr.id = f["id"].integer();
                fr.name = f["name"].str();
                std::string p = f["source"]["path"].str();
                if (!p.empty()) fr.path = normalize_path(p);
                fr.line = static_cast<long>(f["line"].integer(0)) - 1;
                fr.column = static_cast<long>(f["column"].integer(0));
                fr.hint = f["presentationHint"].str();
                t.frames.push_back(std::move(fr));
            }
        }
        generation_++;
        if (then) then();
    });
}

const DebugThread* Debugger::thread(int64_t id) const {
    for (auto& t : threads_)
        if (t.id == id) return &t;
    return nullptr;
}

const DebugFrame* Debugger::current_frame() const {
    if (state_ != DebugState::Stopped) return nullptr;
    const DebugThread* t = thread(sel_thread_);
    if (!t || sel_frame_ < 0 || sel_frame_ >= static_cast<int>(t->frames.size())) return nullptr;
    return &t->frames[static_cast<size_t>(sel_frame_)];
}

void Debugger::expand_thread(int64_t id, bool expand) {
    for (auto& t : threads_) {
        if (t.id != id) continue;
        t.expanded = expand;
        if (expand && !t.frames_loaded && !t.loading && state_ == DebugState::Stopped) load_frames(id);
    }
    generation_++;
}

void Debugger::select_frame(int64_t tid, int index, bool reveal) {
    const DebugThread* t = thread(tid);
    if (!t || index < 0 || index >= static_cast<int>(t->frames.size())) return;
    sel_thread_ = tid;
    sel_frame_ = index;
    scopes_.clear();
    children_.clear();
    summaries_.clear();
    generation_++;
    const DebugFrame& f = t->frames[static_cast<size_t>(index)];
    std::error_code ec;
    if (reveal && host_.reveal && !f.path.empty() && fs::exists(f.path, ec)) host_.reveal(f.path, f.line);
    load_scopes();
    evaluate_watches();
}

void Debugger::load_scopes() {
    const DebugFrame* f = current_frame();
    if (!f || !dap_) return;
    uint64_t epoch = stop_epoch_;
    int64_t frame_id = f->id;
    dap_->request("scopes", Json::object({{"frameId", frame_id}}), [this, epoch, frame_id](const Json& r) {
        const DebugFrame* cur = current_frame();
        if (epoch != stop_epoch_ || !cur || cur->id != frame_id) return;
        scopes_.clear();
        for (auto& s : r["body"]["scopes"].items()) {
            DebugScope sc;
            sc.name = s["name"].str();
            sc.ref = s["variablesReference"].integer();
            sc.expensive = s["expensive"].boolean();
            sc.hint = s["presentationHint"].str();
            scopes_.push_back(sc);
            // Locals and arguments open by default (and feed inline values).
            bool registers = sc.hint == "registers" || sc.name == "Registers";
            if (!sc.expensive && !registers) {
                if (!expanded_.count(sc.name) && !expanded_.count("!" + sc.name)) expanded_.insert(sc.name);
                fetch_children(sc.ref, 0);
            }
        }
        generation_++;
    });
}

void Debugger::set_expanded(const std::string& path, bool on) {
    if (on) {
        expanded_.insert(path);
        expanded_.erase("!" + path);
    } else {
        expanded_.erase(path);
        expanded_.insert("!" + path);  // remembers that a default-open scope was closed
    }
    generation_++;
}

const Debugger::Children* Debugger::children(int64_t ref, bool fetch) {
    if (ref <= 0) return nullptr;
    auto it = children_.find(ref);
    if (it == children_.end()) {
        if (!fetch || state_ != DebugState::Stopped) return nullptr;
        fetch_children(ref, 0);
        it = children_.find(ref);
        if (it == children_.end()) return nullptr;
    }
    return &it->second;
}

void Debugger::fetch_children(int64_t ref, int64_t indexed) {
    if (!dap_ || ref <= 0) return;
    Children& c = children_[ref];
    if (c.loading || c.loaded) return;
    c.loading = true;
    // Find the parent's value (to pair map entries) and size (to page arrays).
    std::string parent_value;
    for (auto& [r, ch] : children_)
        for (auto& v : ch.vars)
            if (v.ref == ref) {
                parent_value = v.value;
                indexed = std::max(indexed, v.indexed);
            }
    for (auto& w : watches_)
        if (w.ref == ref) parent_value = w.value;
    Json args = Json::object({{"variablesReference", ref}});
    if (indexed > PAGE) {
        args.set("filter", "indexed");
        args.set("start", 0);
        args.set("count", PAGE);
    }
    uint64_t epoch = stop_epoch_;
    dap_->request("variables", args, [this, ref, epoch, parent_value, indexed](const Json& r) {
        if (epoch != stop_epoch_) return;
        auto it = children_.find(ref);
        if (it == children_.end()) return;
        Children& c = it->second;
        c.loading = false;
        c.loaded = true;
        if (!r["success"].boolean()) c.error = r["message"].str().empty() ? "not available" : r["message"].str();
        for (auto& v : r["body"]["variables"].items()) {
            DebugVariable dv;
            dv.name = v["name"].str();
            dv.value = v["value"].str();
            dv.type = v["type"].str();
            dv.evaluate_name = v["evaluateName"].str();
            dv.ref = v["variablesReference"].integer();
            dv.named = v["namedVariables"].integer();
            dv.indexed = v["indexedVariables"].integer();
            c.vars.push_back(std::move(dv));
        }
        pair_map_children(parent_value, c.vars);
        if (indexed > PAGE) c.total_indexed = indexed;
        // Rust enums (Option/Result) print as an empty value with one child named
        // after the variant: fetch it so the row can say "Some(..)" / "None".
        for (auto& v : c.vars) {
            if (!v.value.empty() || v.ref <= 0 || v.named != 1) continue;
            if (v.type.find("Option<") == std::string::npos && v.type.find("Result<") == std::string::npos) continue;
            fetch_children(v.ref, 0);
        }
        // A single variant child decides this value's summary.
        if (c.vars.size() == 1) {
            const DebugVariable& only = c.vars.front();
            if (only.name == "None") {
                summaries_[ref] = "None";
            } else if (only.name == "Some" || only.name == "Ok" || only.name == "Err") {
                summaries_[ref] = only.name + "(" + (only.value.empty() ? std::string("..") : only.value) + ")";
                if (only.ref > 0 && only.value.empty()) fetch_children(only.ref, 0);
            }
        }
        // ... and the variant's payload fills in the "(..)".
        for (auto& [pr, pc] : children_) {
            if (pc.vars.size() != 1 || pc.vars.front().ref != ref || c.vars.size() != 1) continue;
            const DebugVariable& payload = c.vars.front();
            std::string shown = !payload.value.empty() ? payload.value : "{" + short_type(payload.type) + "}";
            // A reference payload reads better as its type than as an address.
            if (starts_with(payload.value, "0x") && (starts_with(payload.type, "&") || starts_with(payload.type, "*")))
                shown = short_type(payload.type);
            if (shown.size() > 40) shown = shown.substr(0, 37) + "...";
            summaries_[pr] = pc.vars.front().name + "(" + shown + ")";
        }
        generation_++;
    });
}

std::string Debugger::display_value(const DebugVariable& v) const {
    if (!v.value.empty()) return v.value;
    if (auto it = summaries_.find(v.ref); it != summaries_.end()) return it->second;
    if (!v.type.empty()) return "{" + short_type(v.type) + "}";
    return "";
}

std::vector<DebugVariable> Debugger::frame_variables() const {
    std::vector<DebugVariable> out;
    for (auto& s : scopes_) {
        if (s.expensive || s.hint == "registers" || s.name == "Registers") continue;
        auto it = children_.find(s.ref);
        if (it == children_.end()) continue;
        for (auto& v : it->second.vars) out.push_back(v);
    }
    return out;
}

// ---------------------------------------------------------------- watches and REPL

void Debugger::add_watch(const std::string& expr) {
    if (expr.empty()) return;
    watches_.push_back(DebugWatch{expr, {}, {}, 0, false});
    state_dirty_ = true;
    generation_++;
    if (state_ == DebugState::Stopped) evaluate_watches();
}

void Debugger::remove_watch(size_t i) {
    if (i >= watches_.size()) return;
    watches_.erase(watches_.begin() + static_cast<ptrdiff_t>(i));
    state_dirty_ = true;
    generation_++;
}

void Debugger::edit_watch(size_t i, const std::string& expr) {
    if (i >= watches_.size()) return;
    if (expr.empty()) {
        remove_watch(i);
        return;
    }
    watches_[i] = DebugWatch{expr, {}, {}, 0, false};
    state_dirty_ = true;
    generation_++;
    if (state_ == DebugState::Stopped) evaluate_watches();
}

void Debugger::evaluate_watches() {
    const DebugFrame* f = current_frame();
    if (!f || !dap_) return;
    uint64_t epoch = stop_epoch_;
    for (size_t i = 0; i < watches_.size(); i++) {
        std::string expr = watches_[i].expr;
        dap_->request("evaluate", Json::object({{"expression", expr}, {"frameId", f->id}, {"context", "watch"}}),
                      [this, i, expr, epoch](const Json& r) {
            if (epoch != stop_epoch_ || i >= watches_.size() || watches_[i].expr != expr) return;
            DebugWatch& w = watches_[i];
            w.error = !r["success"].boolean();
            if (w.error) {
                w.value = r["message"].str().empty() ? "not available" : first_line(r["message"].str());
                w.ref = 0;
            } else {
                w.value = r["body"]["result"].str();
                w.type = r["body"]["type"].str();
                w.ref = r["body"]["variablesReference"].integer();
                if (w.value.empty() && w.ref > 0 && (w.type.find("Option<") != std::string::npos ||
                                                     w.type.find("Result<") != std::string::npos))
                    fetch_children(w.ref, 0);
            }
            generation_++;
        });
    }
}

void Debugger::evaluate_repl(const std::string& input) {
    if (input.empty()) return;
    console_open_line_ = false;
    log(ConsoleEntry::Input, "> " + input);
    if (!dap_ || state_ == DebugState::Building) {
        log(ConsoleEntry::Error, "No debug session");
        return;
    }
    bool gdb = session_.adapter == AdapterKind::Gdb || session_.adapter == AdapterKind::RustGdb;
    std::string expr = input;
    std::string context = "repl";
    // gdb's "repl" context runs CLI commands; plain input is an expression, and
    // "-exec <command>" (as in VS Code's cppdbg) runs a gdb command.
    if (gdb) {
        if (starts_with(expr, "-exec ")) expr = expr.substr(6);
        else if (state_ == DebugState::Stopped) context = "watch";
    }
    Json args = Json::object({{"expression", expr}, {"context", context}});
    if (const DebugFrame* f = current_frame()) args.set("frameId", f->id);
    dap_->request("evaluate", args, [this](const Json& r) {
        console_open_line_ = false;
        if (!r["success"].boolean()) {
            log(ConsoleEntry::Error, r["message"].str().empty() ? "evaluation failed" : r["message"].str());
            return;
        }
        std::string result = r["body"]["result"].str();
        if (result.empty() && r["body"]["variablesReference"].integer() > 0)
            result = "{" + short_type(r["body"]["type"].str()) + "}";
        while (!result.empty() && result.back() == '\n') result.pop_back();
        if (!result.empty()) log(ConsoleEntry::Result, result);
    });
}

// ---------------------------------------------------------------- reverse requests

void Debugger::on_reverse_request(const Json& req) {
    const std::string& cmd = req["command"].str();
    if (cmd != "runInTerminal") {
        dap_->respond(req, false, Json(), "unsupported request: " + cmd);
        return;
    }
    // The debuggee runs in a pseudo-terminal whose output goes to the debug console.
    const Json& a = req["arguments"];
    std::vector<std::string> argv;
#ifndef _WIN32
    if (a["env"].size() > 0) {
        argv.push_back("env");
        for (auto& [k, v] : a["env"].members())
            if (v.is_string()) argv.push_back(k + "=" + v.str());
    }
#endif
    for (auto& x : a["args"].items()) argv.push_back(x.str());
    term_ = std::make_unique<TerminalJob>();
    if (argv.empty() || !term_->pty.spawn(argv, a["cwd"].str(), 120, 30)) {
        term_.reset();
        dap_->respond(req, false, Json(), "could not start the program");
        return;
    }
    TerminalJob* job = term_.get();
    job->reader = std::thread([job] {
        char buf[4096];
        while (!job->stop) {
            long n = job->pty.read(buf, sizeof buf, 100);
            if (n < 0) break;
            if (n == 0) continue;
            std::lock_guard<std::mutex> lock(job->mutex);
            job->pending.append(buf, static_cast<size_t>(n));
            request_redraw();
        }
    });
    log(ConsoleEntry::Info, "runInTerminal: " + a["args"][0].str());
    dap_->respond(req, true, Json::object());
}

} // namespace oh
