#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace oh {

struct LSPDiagnostic {
    Position start, end;
    std::string message;
    int severity; // 1=error, 2=warning, 3=info, 4=hint
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

    // Diagnostics
    using DiagnosticsCallback = std::function<void(const fs::path&, std::vector<LSPDiagnostic>)>;
    void set_on_diagnostics(DiagnosticsCallback cb) { on_diagnostics_ = cb; }

private:
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    pid_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_thread_;

    int next_id_ = 1;
    std::mutex pending_mutex_;
    std::unordered_map<int, std::function<void(const std::string&)>> pending_requests_;
    DiagnosticsCallback on_diagnostics_;

    void send_message(const std::string& json);
    void send_request(const std::string& method, const std::string& params,
                      std::function<void(const std::string&)> callback = {});
    void send_notification(const std::string& method, const std::string& params);
    void reader_loop();
    void handle_message(const std::string& json);
    std::string uri_from_path(const fs::path& path) const;
};

} // namespace oh
