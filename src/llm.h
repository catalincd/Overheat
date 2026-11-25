#pragma once
#include "types.h"
#include "platform/platform.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oh {

// Draft LLM client. Backends run as child processes (curl for OpenAI-compatible
// HTTP endpoints, the `omp` CLI for the agent) on a worker thread; streamed text
// is handed back through a callback (called on the worker thread).
struct ChatMessage {
    std::string role;  // "user" | "assistant" | "system"
    std::string content;
};

struct LLMBackend {
    enum Kind { OpenAI, Omp } kind = OpenAI;
    std::string name;      // label for the UI
    std::string endpoint;  // OpenAI: base URL, e.g. http://127.0.0.1:8000
    std::string model;     // empty: first model the server lists
    std::string api_key;   // optional, sent as a bearer token
};

class LLMClient {
public:
    ~LLMClient() { cancel(); }
    using OnText = std::function<void(const std::string& delta)>;
    using OnDone = std::function<void(const std::string& error)>;  // empty error = success

    // Starts a reply to `history`; any previous request is cancelled first.
    void send(const LLMBackend& backend, std::vector<ChatMessage> history, const fs::path& cwd,
              OnText on_text, OnDone on_done);
    void cancel();
    bool busy() const { return busy_; }

private:
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<platform::Process*> current_{nullptr};
};

} // namespace oh
