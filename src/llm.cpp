#include "llm.h"
#include <cstring>

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
                std::atomic<platform::Process*>& current) {
    platform::Process proc;
    if (!proc.start(argv, cwd)) return -1;
    current = &proc;
    proc.write(input.data(), input.size());  // small request bodies
    proc.close_stdin();
    char buf[4096];
    while (!cancel) {
        long n = proc.read(buf, sizeof buf);
        if (n <= 0) break;
        on_chunk(buf, static_cast<size_t>(n));
    }
    current = nullptr;
    if (cancel) proc.terminate();
    return proc.wait();
}

std::string first_model(const LLMBackend& b, std::atomic<bool>& cancel, std::atomic<platform::Process*>& pid) {
    std::string out;
    std::vector<std::string> argv = {"curl", "-s", "-m", "10", b.endpoint + "/v1/models"};
    if (!b.api_key.empty()) argv.insert(argv.end(), {"-H", "Authorization: Bearer " + b.api_key});
    run_process(argv, {}, "", [&](const char* d, size_t n) { out.append(d, n); }, cancel, pid);
    return json_string(out, "id");
}

} // namespace

void LLMClient::cancel() {
    cancel_ = true;
    if (platform::Process* p = current_) p->terminate();  // unblocks the worker's read
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
                                 [&](const char* d, size_t n) { on_text(std::string(d, n)); }, cancel_, current_);
            if (rc != 0 && !cancel_) error = "omp exited with status " + std::to_string(rc);
        } else {
            std::string model = backend.model.empty() ? first_model(backend, cancel_, current_) : backend.model;
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
            }, cancel_, current_);
            if (!cancel_ && !got_any)
                error = rc != 0 ? "request failed (curl status " + std::to_string(rc) + ") - is the endpoint running?"
                                : "no reply: " + raw.substr(0, 200);
        }
        busy_ = false;
        on_done(cancel_ ? "stopped" : error);
    });
}

} // namespace oh
