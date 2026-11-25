#include "llm.h"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oh {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Value of the first "key":"..." string in a JSON fragment, unescaped.
std::string json_string(const std::string& json, const std::string& key, size_t from = 0) {
    std::string needle = "\"" + key + "\":";
    size_t p = json.find(needle, from);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size() || json[p] != '"') return {};
    std::string out;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        char c = json[p];
        if (c != '\\' || p + 1 >= json.size()) { out += c; continue; }
        char n = json[++p];
        switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'u': {
                if (p + 4 >= json.size()) break;
                uint32_t cp = static_cast<uint32_t>(std::strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16));
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < json.size() && json[p + 1] == '\\' && json[p + 2] == 'u') {
                    uint32_t lo = static_cast<uint32_t>(std::strtoul(json.substr(p + 3, 4).c_str(), nullptr, 16));
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += n;
        }
    }
    return out;
}

// Runs argv with `input` on stdin, calling on_chunk for stdout data as it arrives.
// Returns the exit status (or -1). Stops early when `cancel` is set.
int run_process(const std::vector<std::string>& argv, const fs::path& cwd, const std::string& input,
                const std::function<void(const char*, size_t)>& on_chunk, std::atomic<bool>& cancel,
                std::atomic<int>& pid_out) {
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return -1;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {}
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    pid_out = pid;
    close(in_pipe[0]);
    close(out_pipe[1]);
    // Small request bodies: write all, then close so the child sees EOF.
    size_t off = 0;
    while (off < input.size()) {
        ssize_t n = write(in_pipe[1], input.data() + off, input.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    close(in_pipe[1]);
    char buf[4096];
    for (;;) {
        if (cancel) { kill(pid, SIGTERM); break; }
        ssize_t n = read(out_pipe[0], buf, sizeof buf);
        if (n > 0) on_chunk(buf, static_cast<size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }
    close(out_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    pid_out = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string first_model(const LLMBackend& b, std::atomic<bool>& cancel, std::atomic<int>& pid) {
    std::string out;
    std::vector<std::string> argv = {"curl", "-s", "-m", "10", b.endpoint + "/v1/models"};
    if (!b.api_key.empty()) argv.insert(argv.end(), {"-H", "Authorization: Bearer " + b.api_key});
    run_process(argv, {}, "", [&](const char* d, size_t n) { out.append(d, n); }, cancel, pid);
    return json_string(out, "id");
}

} // namespace

void LLMClient::cancel() {
    cancel_ = true;
    if (int pid = child_pid_; pid > 0) kill(pid, SIGTERM);
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    busy_ = false;
}

void LLMClient::send(const LLMBackend& backend, std::vector<ChatMessage> history, const fs::path& cwd,
                     OnText on_text, OnDone on_done) {
    cancel();
    busy_ = true;
    worker_ = std::thread([this, backend, history = std::move(history), cwd, on_text, on_done]() {
        std::string error;
        if (backend.kind == LLMBackend::Omp) {
            // The agent runs non-interactively in the project folder; its stdout is the reply.
            std::string prompt = history.empty() ? "" : history.back().content;
            int rc = run_process({"omp", "-p", "--no-session", prompt}, cwd, "",
                                 [&](const char* d, size_t n) { on_text(std::string(d, n)); }, cancel_, child_pid_);
            if (rc != 0 && !cancel_) error = "omp exited with status " + std::to_string(rc);
        } else {
            std::string model = backend.model.empty() ? first_model(backend, cancel_, child_pid_) : backend.model;
            std::string body = "{\"stream\":true,";
            if (!model.empty()) body += "\"model\":\"" + json_escape(model) + "\",";
            body += "\"messages\":[";
            for (size_t i = 0; i < history.size(); i++) {
                if (i) body += ",";
                body += "{\"role\":\"" + history[i].role + "\",\"content\":\"" + json_escape(history[i].content) + "\"}";
            }
            body += "]}";
            std::vector<std::string> argv = {"curl", "-sN", "-m", "600", "-H", "Content-Type: application/json",
                                             "--data-binary", "@-", backend.endpoint + "/v1/chat/completions"};
            if (!backend.api_key.empty()) argv.insert(argv.end() - 1, {"-H", "Authorization: Bearer " + backend.api_key});
            std::string pending, raw;
            bool got_any = false;
            int rc = run_process(argv, {}, body, [&](const char* d, size_t n) {
                pending.append(d, n);
                raw.append(d, std::min<size_t>(n, 4096 - std::min<size_t>(raw.size(), 4096)));
                // Server-sent events: one JSON chunk per "data:" line.
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                    std::string line = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (line.rfind("data:", 0) != 0) continue;
                    std::string payload = line.substr(line[5] == ' ' ? 6 : 5);
                    if (payload.rfind("[DONE]", 0) == 0) continue;
                    size_t delta = payload.find("\"delta\"");
                    std::string text = json_string(payload, "content", delta == std::string::npos ? 0 : delta);
                    if (!text.empty()) {
                        got_any = true;
                        on_text(text);
                    }
                }
            }, cancel_, child_pid_);
            if (!cancel_ && !got_any)
                error = rc != 0 ? "request failed (curl status " + std::to_string(rc) + ") - is the endpoint running?"
                                : "no reply: " + raw.substr(0, 200);
        }
        busy_ = false;
        on_done(cancel_ ? "stopped" : error);
    });
}

} // namespace oh
