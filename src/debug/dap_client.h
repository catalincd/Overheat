#pragma once
// Debug Adapter Protocol transport: an adapter process speaking Content-Length
// framed JSON over stdio (the same framing as LSP). A reader thread parses
// messages and queues them; poll() runs callbacks on the caller's (UI) thread, so
// the session model needs no locking. Writes go through a queue and a writer
// thread, so a slow adapter never blocks the UI.
#include "debug/json.h"
#include "platform/platform.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace oh {

// Incremental "Content-Length: N\r\n\r\n<body>" parser; exposed for tests.
class DapFramer {
public:
    void feed(const char* data, size_t n);
    // Next complete message body, if one is buffered.
    bool next(std::string& body);
    bool error() const { return error_; }  // a header without Content-Length

private:
    std::string buf_;
    size_t pos_ = 0;
    bool error_ = false;
};

std::string dap_frame(const std::string& body);

class DapClient {
public:
    using ResponseFn = std::function<void(const Json& response)>;  // the whole response message
    using EventFn = std::function<void(const Json& event)>;
    using RequestFn = std::function<void(const Json& request)>;    // reverse requests (runInTerminal)

    DapClient();
    ~DapClient();
    DapClient(const DapClient&) = delete;
    DapClient& operator=(const DapClient&) = delete;

    bool start(const std::vector<std::string>& argv, const fs::path& cwd = {});
    // Stops the adapter (terminates the process) and joins the threads.
    void stop();
    bool running() const { return running_; }
    // The adapter closed its output (exited); poll() reports it once via on_exit.
    bool exited() const { return exited_; }

    void set_on_event(EventFn fn) { on_event_ = std::move(fn); }
    void set_on_request(RequestFn fn) { on_request_ = std::move(fn); }
    void set_on_exit(std::function<void()> fn) { on_exit_ = std::move(fn); }
    // Raw traffic for the debug log (setting debug.trace); both directions.
    void set_trace(std::function<void(bool outgoing, const std::string&)> fn) { trace_ = std::move(fn); }

    int request(const std::string& command, Json arguments = Json::object(), ResponseFn on_response = {});
    void respond(const Json& request, bool success, Json body = Json(), const std::string& message = {});

    // Runs queued callbacks on the calling thread; returns true if anything ran.
    // Callbacks may call stop() but must not destroy the client.
    bool poll();

private:
    std::unique_ptr<platform::Process> process_;
    std::atomic<bool> running_{false};
    std::atomic<bool> exited_{false};
    bool exit_reported_ = false;
    std::thread reader_, writer_;
    std::mutex out_mutex_;
    std::condition_variable out_cv_;
    std::vector<std::string> outbox_;
    std::mutex in_mutex_;
    std::deque<Json> inbox_;
    int next_seq_ = 1;
    std::unordered_map<int, ResponseFn> pending_;
    EventFn on_event_;
    RequestFn on_request_;
    std::function<void()> on_exit_;
    std::function<void(bool, const std::string&)> trace_;

    void send(const Json& message);
    void reader_loop();
    void writer_loop();
};

} // namespace oh
