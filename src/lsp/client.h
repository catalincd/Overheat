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

// --- Navigation types (requests in lsp/client_nav.cpp) ---
struct LSPRangeLoc {  // a Location / LocationLink target with its range
    fs::path path;
    Position start{}, end{};
};

struct LSPSymbol {  // DocumentSymbol (flattened, depth-first) or WorkspaceSymbol
    std::string name, detail, container;
    int kind = 0;  // LSP SymbolKind
    fs::path path;
    Position start{}, end{};  // whole range
    Position sel{};           // selectionRange start (the name)
    int depth = 0;
};

struct LSPInlayHint {
    Position pos{};
    std::string label;
    int kind = 0;  // 1 = type, 2 = parameter
    bool pad_left = false, pad_right = false;
    int cells = 0;  // display width incl. padding (filled in by the UI)
};

struct LSPPrepareRename {
    bool ok = false;
    bool use_word = false;  // server only said "default behavior": rename the word at the cursor
    Position start{}, end{};
    std::string placeholder;
    std::string error;
};

struct LSPWorkspaceEdit;
namespace json { struct Value; }

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

    // --- Navigation (lsp/client_nav.cpp) ---
    // Every request returns its id (0 when no server runs) for cancel(). Callbacks run
    // on the reader thread, also with an empty result when the request failed or the
    // server exited, so callers never wait forever.
    void cancel(int id);
    int hover_markdown(const fs::path& path, Position pos, HoverCallback cb);
    using LocationsCallback = std::function<void(std::vector<LSPRangeLoc>)>;
    // method: textDocument/definition, declaration, typeDefinition, implementation, references
    int locations(const std::string& method, const fs::path& path, Position pos, LocationsCallback cb);
    int prepare_rename(const fs::path& path, Position pos, std::function<void(LSPPrepareRename)> cb);
    using RenameCallback = std::function<void(std::optional<LSPWorkspaceEdit>, std::string error)>;
    int rename(const fs::path& path, Position pos, const std::string& new_name, RenameCallback cb);
    using SymbolsCallback = std::function<void(std::vector<LSPSymbol>)>;
    int document_symbols(const fs::path& path, SymbolsCallback cb);
    int workspace_symbols(const std::string& query, SymbolsCallback cb);
    int switch_source_header(const fs::path& path, std::function<void(std::optional<fs::path>)> cb);
    using InlayCallback = std::function<void(std::vector<LSPInlayHint>, bool ok)>;
    int inlay_hints(const fs::path& path, Position start, Position end, InlayCallback cb);
    // Files changed on disk behind the server's back (rename into unopened files).
    void did_change_watched_files(const std::vector<fs::path>& paths);
    // From the initialize result, e.g. has_provider("renameProvider").
    bool has_provider(const std::string& name) const;
    // Work-done progress ($/progress), e.g. "rust-analyzer: Indexing 12/40"; "" when idle.
    std::string progress() const;
    // Bumped when a progress ends or the server asks to refresh inlay hints.
    uint64_t refresh_epoch() const { return refresh_epoch_; }
    // Generic request: `cb` gets the whole response (Null if the server went away).
    int request(const std::string& method, const std::string& params,
                std::function<void(const json::Value& response)> cb);
    std::string text_document_position(const fs::path& path, Position pos) const;

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
    bool hold_until_initialized_ = false;  // queue into held_ until `initialized` is sent
    std::vector<std::string> held_;
    void writer_loop();

    int next_id_ = 1;
    std::mutex pending_mutex_;
    std::unordered_map<int, std::function<void(const std::string&)>> pending_requests_;
    DiagnosticsCallback on_diagnostics_;
    std::atomic<bool> utf8_positions_{false};
    std::atomic<bool> ready_{false};
    std::unordered_map<std::string, int> versions_;  // document versions (UI thread only)
    std::unordered_set<std::string> open_docs_;

    // Navigation state (client_nav.cpp).
    mutable std::mutex nav_mutex_;
    std::string server_caps_;  // raw initialize result
    mutable std::unordered_map<std::string, bool> provider_cache_;  // has_provider() answers
    std::unordered_map<std::string, std::string> progress_;  // token -> "title: message"
    std::atomic<uint64_t> refresh_epoch_{0};
    std::atomic<bool> stopping_{false};
    // Server -> client requests and notifications besides diagnostics; true if handled.
    bool handle_server_message(const std::string& method, const std::string& json);
    void fail_pending();

    void send_message(const std::string& json);
    int send_request(const std::string& method, const std::string& params,
                      std::function<void(const std::string&)> callback = {});
    void send_notification(const std::string& method, const std::string& params);
    void reader_loop();
    void handle_message(const std::string& json);
    std::string uri_from_path(const fs::path& path) const;
};

} // namespace oh
