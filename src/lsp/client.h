#pragma once
#include "types.h"
#include "platform/platform.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <optional>

namespace oh {

struct LSPDiagnostic {
    Position start, end;
    std::string message;
    int severity; // 1=error, 2=warning, 3=info, 4=hint
    std::string code, source;  // e.g. "-Wunused-variable" / "clang", "E0308" / "rustc"
    std::string raw;           // the diagnostic's JSON as received (code action context)
};

struct LSPCompletion {
    std::string label;
    std::string detail;
    std::string insert_text;
    int kind;
};

struct LSPHover {
    std::string contents;
    Position start, end;
};

struct LSPLocation {
    fs::path path;
    Position pos;
};

class LSPClient {
public:
    LSPClient();
    ~LSPClient();

    bool start(const std::string& command, const std::vector<std::string>& args = {});
    void stop();
    bool is_running() const { return running_; }
    // True once the server answered `initialize` (status bar: starting vs ready).
    bool is_ready() const { return running_ && ready_; }
    // True when the server agreed to UTF-8 byte columns; otherwise columns are UTF-16 units.
    bool utf8_positions() const { return utf8_positions_; }
    bool is_open(const fs::path& path) const { return open_docs_.count(path.string()) != 0; }
    void mark_open(const fs::path& path) { open_docs_.insert(path.string()); }

    // Lifecycle
    void initialize(const fs::path& root_uri);
    void did_open(const fs::path& path, const std::string& language, const std::string& text);
    void did_change(const fs::path& path, const std::string& text);
    void did_save(const fs::path& path);
    void did_close(const fs::path& path);

    // Requests
    using CompletionCallback = std::function<void(std::vector<LSPCompletion>)>;
    void completion(const fs::path& path, Position pos, CompletionCallback cb);

    using HoverCallback = std::function<void(std::optional<LSPHover>)>;
    void hover(const fs::path& path, Position pos, HoverCallback cb);

    using DefinitionCallback = std::function<void(std::vector<LSPLocation>)>;
    void definition(const fs::path& path, Position pos, DefinitionCallback cb);

    using ReferencesCallback = std::function<void(std::vector<LSPLocation>)>;
    void references(const fs::path& path, Position pos, ReferencesCallback cb);

    // Quick fixes / refactorings for a range (textDocument/codeAction). `diagnostics_json`
    // is a JSON array of the diagnostics there (their `raw` text). The callback gets the
    // whole response JSON (tasks_glue.cpp parses it).
    using RawCallback = std::function<void(const std::string& response)>;
    void code_action(const fs::path& path, Position start, Position end, const std::string& diagnostics_json,
                     RawCallback cb);
    void formatting(const fs::path& path, int tab_size, bool insert_spaces, RawCallback cb);
    void execute_command(const std::string& command_json, RawCallback cb);  // a Command object
    // Requests from the server (workspace/applyEdit, ...): the handler returns the
    // result JSON to send back, or "" to leave the request unanswered. Reader thread.
    using ServerRequestHandler = std::function<std::string(const std::string& method, const std::string& json)>;
    void set_on_server_request(ServerRequestHandler h) { on_server_request_ = std::move(h); }
    std::string uri(const fs::path& path) const { return uri_from_path(path); }

    // Diagnostics
    using DiagnosticsCallback = std::function<void(const fs::path&, std::vector<LSPDiagnostic>)>;
    void set_on_diagnostics(DiagnosticsCallback cb) { on_diagnostics_ = cb; }

private:
    std::unique_ptr<platform::Process> process_;
    char rbuf_[8192];
    size_t rpos_ = 0, rlen_ = 0;
    bool read_byte(char& c);
    long read_bytes(char* out, size_t n);
    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    // Writes go through a queue so a large didOpen never blocks the UI thread on
    // the server draining its stdin pipe.
    std::thread writer_thread_;
    std::mutex outbox_mutex_;
    std::condition_variable outbox_cv_;
    std::vector<std::string> outbox_;
    // Messages sent before the initialize handshake finished: the protocol allows
    // nothing but `initialize` until then (rust-analyzer exits on an early didOpen).
    std::vector<std::string> held_;
    void writer_loop();

    int next_id_ = 1;
    std::mutex pending_mutex_;
    std::unordered_map<int, std::function<void(const std::string&)>> pending_requests_;
    DiagnosticsCallback on_diagnostics_;
    ServerRequestHandler on_server_request_;
    std::atomic<bool> utf8_positions_{false};
    std::atomic<bool> ready_{false};
    std::unordered_map<std::string, int> versions_;  // document versions (UI thread only)
    std::unordered_set<std::string> open_docs_;

    void send_message(const std::string& json, bool handshake = false);
    void send_request(const std::string& method, const std::string& params,
                      std::function<void(const std::string&)> callback = {});
    void send_notification(const std::string& method, const std::string& params);
    void reader_loop();
    void handle_message(const std::string& json);
    std::string uri_from_path(const fs::path& path) const;
};

} // namespace oh
