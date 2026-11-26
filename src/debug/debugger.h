#pragma once
// The debugger's session model: one debug session at a time over a DAP adapter
// (gdb -i dap, lldb-dap, CodeLLDB), plus the state that outlives sessions
// (breakpoints, watch expressions, the selected launch configuration), persisted
// per project. Everything here runs on the UI thread: the DapClient queues adapter
// messages and update() dispatches them.
#include "debug/breakpoints.h"
#include "debug/dap_client.h"
#include "debug/launch_config.h"
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oh {

struct DebugVariable {
    std::string name, value, type, evaluate_name;
    int64_t ref = 0;  // variablesReference; 0 = no children
    int64_t named = 0, indexed = 0;
    bool has_children() const { return ref > 0; }
};

struct DebugFrame {
    int64_t id = 0;
    std::string name;
    fs::path path;    // empty when the frame has no source
    long line = -1;   // 0-based
    long column = 0;
    std::string hint;  // presentationHint: "normal", "label", "subtle"
};

struct DebugThread {
    int64_t id = 0;
    std::string name;
    std::vector<DebugFrame> frames;
    bool frames_loaded = false, loading = false;
    bool expanded = false;
};

struct DebugScope {
    std::string name;
    int64_t ref = 0;
    bool expensive = false;
    std::string hint;  // "locals", "arguments", "registers"
};

struct DebugWatch {
    std::string expr;
    std::string value, type;
    int64_t ref = 0;
    bool error = false;
};

struct ConsoleEntry {
    enum Kind { Out, Err, Info, Input, Result, Error, Important } kind = Out;
    std::string text;
};

enum class DebugState { Idle, Building, Launching, Running, Stopped };
const char* debug_state_name(DebugState s);

class Debugger {
public:
    // What the debugger needs from the application (set once by App).
    struct Host {
        std::function<void(const fs::path& path, long line)> reveal;  // open + show a 0-based line
        std::function<void(const std::string& msg)> status;
        // Free-text prompt (palette): calls back with the text, or nothing on cancel.
        std::function<void(const std::string& placeholder, const std::string& initial,
                           std::function<void(const std::string&)> done)> prompt;
        std::function<fs::path()> current_file;
        std::function<std::string(const std::string& key)> setting;  // settings.toml
        std::function<void()> save_all;  // before a pre-launch build
        std::function<void()> session_started;
        std::function<void()> session_ended;
    };
    // Pre-launch build hook: runs the configuration's build and calls done(ok).
    // Default: a plain subprocess whose output goes to the debug console. The task
    // system can replace it to reuse "the build task" (keep calling done exactly once).
    using BuildHook = std::function<void(const LaunchConfig&, std::function<void(bool ok)> done)>;

    Debugger();
    ~Debugger();

    void set_host(Host h) { host_ = std::move(h); }
    void set_build_hook(BuildHook h) { build_hook_ = std::move(h); }
    // Loads the persisted state for a project (breakpoints, watches, selection).
    void set_root(const fs::path& root, const fs::path& config_dir);
    const fs::path& root() const { return root_; }
    // Dispatches adapter messages and build output; call every frame.
    void update();
    void shutdown();  // ends any session (app exit)

    // ---- launch configurations
    const std::vector<LaunchConfig>& configs();
    void refresh_configs();
    const std::vector<std::string>& config_problems() const { return config_problems_; }
    int selected_config() const { return selected_config_; }
    void select_config(int i);
    std::string selected_config_name();

    // ---- session control
    void start(int config_index);
    void start_or_continue();  // F5
    void continue_();
    void step_over();
    void step_into();
    void step_out();
    void pause();
    void stop();
    void restart();
    DebugState state() const { return state_; }
    bool active() const { return state_ != DebugState::Idle; }
    bool stopped() const { return state_ == DebugState::Stopped; }
    const std::string& stop_description() const { return stop_description_; }
    const std::string& session_name() const { return session_.name; }
    const LaunchConfig& session_config() const { return session_; }
    AdapterKind adapter() const { return session_.adapter; }
    // Adapter command line for a configuration (exposed for tests and the log).
    std::vector<std::string> adapter_command(const LaunchConfig& c);

    // ---- breakpoints
    BreakpointStore& breakpoints() { return bps_; }
    void toggle_breakpoint(const fs::path& path, size_t line);
    // Sends the file's breakpoints again (after any change to them).
    void breakpoints_changed(const fs::path& path);
    void function_breakpoints_changed();
    void set_exception_filter(const std::string& id, bool on);
    // Filters to offer: the adapter's (or gdb's defaults before a session) plus "rust_panic".
    struct Filter { std::string id, label; };
    std::vector<Filter> exception_filters() const;
    // Breakpoint lines of a file follow an edit.
    void on_edit(const fs::path& path, const LineEdit& e);

    // ---- threads, frames, variables
    std::vector<DebugThread>& threads() { return threads_; }
    int64_t selected_thread() const { return sel_thread_; }
    int selected_frame() const { return sel_frame_; }
    const DebugFrame* current_frame() const;
    const DebugThread* thread(int64_t id) const;
    void expand_thread(int64_t id, bool expand);
    void select_frame(int64_t thread_id, int index, bool reveal = true);
    const std::vector<DebugScope>& scopes() const { return scopes_; }
    struct Children {
        std::vector<DebugVariable> vars;
        bool loading = false, loaded = false;
        std::string error;
        int64_t total_indexed = 0;  // when only the first page was fetched
    };
    // Children of a reference, fetching them if needed (nullptr until requested).
    const Children* children(int64_t ref, bool fetch = true);
    // Display value: the adapter's value, a summary (Option -> "Some(..)"), or the type.
    std::string display_value(const DebugVariable& v) const;
    // Expansion state by tree path ("Locals/inv/items_"), kept across steps.
    bool expanded(const std::string& path) const { return expanded_.count(path) != 0; }
    void set_expanded(const std::string& path, bool on);
    // Top-level variables of the selected frame (locals, arguments) for inline values.
    std::vector<DebugVariable> frame_variables() const;
    // Bumped when frames, variables or watches change (UI caches key on it).
    uint64_t generation() const { return generation_; }

    // ---- watch expressions
    std::vector<DebugWatch>& watches() { return watches_; }
    void add_watch(const std::string& expr);
    void remove_watch(size_t i);
    void edit_watch(size_t i, const std::string& expr);

    // ---- debug console
    const std::deque<ConsoleEntry>& console() const { return console_; }
    uint64_t console_generation() const { return console_gen_; }
    void evaluate_repl(const std::string& expr);
    void clear_console();
    void log(ConsoleEntry::Kind k, const std::string& text);

private:
    Host host_;
    BuildHook build_hook_;
    fs::path root_, state_file_;
    std::vector<LaunchConfig> configs_;
    bool configs_loaded_ = false;
    std::vector<std::string> config_problems_;
    int selected_config_ = -1;
    std::string selected_name_;  // persisted by name

    BreakpointStore bps_;
    uint64_t saved_generation_ = 0;
    std::vector<DebugWatch> watches_;
    bool state_dirty_ = false;
    void load_state();
    void save_state();

    // session
    DebugState state_ = DebugState::Idle;
    LaunchConfig session_;
    std::unique_ptr<DapClient> dap_;
    Json caps_;
    bool configured_ = false;   // configurationDone sent
    bool teardown_ = false;     // end the session after this update
    std::optional<int> pending_start_;  // restart: start this config once the old session ended
    TimePoint stop_deadline_{};
    bool stopping_ = false;
    long debuggee_pid_ = 0;
    bool exited_reported_ = false;
    std::string stop_description_;
    std::vector<int64_t> panic_bp_ids_;  // our synthetic rust_panic function breakpoints
    void launch_adapter();
    void on_event(const Json& ev);
    void on_reverse_request(const Json& req);
    void on_stopped(const Json& body);
    void end_session();
    void finish_teardown();
    void configure();  // breakpoints + configurationDone
    void send_breakpoints(const fs::path& path);
    void send_function_breakpoints();
    void send_exception_breakpoints();
    void apply_breakpoint_update(const Json& bp);
    void set_running();
    void step(const char* command);
    std::string rust_sysroot_;
    bool uses_panic_filter() const;

    // build
    struct BuildJob;
    std::unique_ptr<BuildJob> build_;
    void run_build(const LaunchConfig& c, std::function<void(bool)> done);
    void poll_build();

    // runInTerminal
    struct TerminalJob;
    std::unique_ptr<TerminalJob> term_;

    // stopped state
    std::vector<DebugThread> threads_;
    int64_t sel_thread_ = 0;
    int sel_frame_ = 0;
    std::vector<DebugScope> scopes_;
    std::unordered_map<int64_t, Children> children_;
    std::unordered_map<int64_t, std::string> summaries_;  // ref -> synthesized display value
    std::unordered_set<std::string> expanded_;
    uint64_t generation_ = 1;
    uint64_t stop_epoch_ = 0;  // responses from before the last resume are dropped
    void clear_stopped_state();
    void load_threads(int64_t focus_thread);
    void load_frames(int64_t thread_id, std::function<void()> then = {});
    void load_scopes();
    void evaluate_watches();
    void fetch_children(int64_t ref, int64_t indexed);

    // console
    std::deque<ConsoleEntry> console_;
    uint64_t console_gen_ = 1;
    bool console_open_line_ = false;  // last entry did not end with a newline
    ConsoleEntry::Kind console_open_kind_ = ConsoleEntry::Out;
    void append_output(ConsoleEntry::Kind k, const std::string& text);
};

} // namespace oh
