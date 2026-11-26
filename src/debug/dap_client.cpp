#include "debug/dap_client.h"
#include <cctype>
#include <cstring>

namespace oh {

void request_redraw();

// ---------------------------------------------------------------- framing

std::string dap_frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void DapFramer::feed(const char* data, size_t n) {
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {  // drop consumed bytes now and then
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    buf_.append(data, n);
}

bool DapFramer::next(std::string& body) {
    for (;;) {
        size_t hdr_end = buf_.find("\r\n\r\n", pos_);
        if (hdr_end == std::string::npos) return false;
        // Header fields are case-insensitive; Content-Type may also appear.
        long length = -1;
        size_t line = pos_;
        while (line < hdr_end) {
            size_t eol = buf_.find("\r\n", line);
            if (eol == std::string::npos || eol > hdr_end) eol = hdr_end;
            std::string field = buf_.substr(line, eol - line);
            size_t colon = field.find(':');
            if (colon != std::string::npos) {
                std::string name = field.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (name == "content-length") length = std::strtol(field.c_str() + colon + 1, nullptr, 10);
            }
            line = eol + 2;
        }
        if (length < 0) {  // garbage before a header (e.g. stray adapter output): skip it
            error_ = true;
            pos_ = hdr_end + 4;
            continue;
        }
        size_t body_start = hdr_end + 4;
        if (buf_.size() - body_start < static_cast<size_t>(length)) return false;
        body = buf_.substr(body_start, static_cast<size_t>(length));
        pos_ = body_start + static_cast<size_t>(length);
        return true;
    }
}

// ---------------------------------------------------------------- client

DapClient::DapClient() = default;
DapClient::~DapClient() { stop(); }

bool DapClient::start(const std::vector<std::string>& argv, const fs::path& cwd) {
    stop();
    process_ = std::make_unique<platform::Process>();
    if (!process_->start(argv, cwd)) {
        process_.reset();
        return false;
    }
    running_ = true;
    exited_ = false;
    exit_reported_ = false;
    reader_ = std::thread(&DapClient::reader_loop, this);
    writer_ = std::thread(&DapClient::writer_loop, this);
    return true;
}

void DapClient::stop() {
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        running_ = false;
        outbox_.clear();
    }
    out_cv_.notify_all();
    if (process_) {
        process_->terminate();
        // An adapter that ignores the polite request (or hangs in ptrace) is killed.
        for (int i = 0; i < 200 && !exited_ && reader_.joinable(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!exited_ && reader_.joinable()) platform::kill_process(process_->pid());
    }
    if (writer_.joinable()) writer_.join();
    if (reader_.joinable()) reader_.join();
    if (process_) process_->wait();
    process_.reset();
    pending_.clear();
    std::lock_guard<std::mutex> lock(in_mutex_);
    inbox_.clear();
}

void DapClient::send(const Json& message) {
    std::string body = message.dump();
    if (trace_) trace_(true, body);
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        if (!running_) return;
        outbox_.push_back(dap_frame(body));
    }
    out_cv_.notify_one();
}

int DapClient::request(const std::string& command, Json arguments, ResponseFn on_response) {
    int seq = next_seq_++;
    Json msg = Json::object({{"seq", seq}, {"type", "request"}, {"command", command}});
    msg.set("arguments", std::move(arguments));
    if (on_response) pending_[seq] = std::move(on_response);
    send(msg);
    return seq;
}

void DapClient::respond(const Json& req, bool success, Json body, const std::string& message) {
    Json msg = Json::object({{"seq", next_seq_++}, {"type", "response"},
                             {"request_seq", req["seq"].integer()}, {"command", req["command"].str()},
                             {"success", success}});
    if (!message.empty()) msg.set("message", message);
    if (!body.is_null()) msg.set("body", std::move(body));
    send(msg);
}

void DapClient::writer_loop() {
    for (;;) {
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(out_mutex_);
            out_cv_.wait(lock, [this] { return !running_ || !outbox_.empty(); });
            if (!running_) return;
            batch.swap(outbox_);
        }
        for (auto& m : batch)
            if (!process_->write(m.data(), m.size())) return;
    }
}

void DapClient::reader_loop() {
    DapFramer framer;
    char buf[16384];
    for (;;) {
        long n = process_->read(buf, sizeof buf);
        if (n <= 0) break;
        framer.feed(buf, static_cast<size_t>(n));
        std::string body;
        bool any = false;
        while (framer.next(body)) {
            Json msg;
            if (!Json::parse(body, msg)) continue;
            std::lock_guard<std::mutex> lock(in_mutex_);
            inbox_.push_back(std::move(msg));
            any = true;
        }
        if (any) request_redraw();
    }
    exited_ = true;
    request_redraw();
}

bool DapClient::poll() {
    std::deque<Json> batch;
    {
        std::lock_guard<std::mutex> lock(in_mutex_);
        batch.swap(inbox_);
    }
    bool ran = !batch.empty();
    for (auto& msg : batch) {
        if (trace_) trace_(false, msg.dump());
        const std::string& type = msg["type"].str();
        if (type == "response") {
            int seq = static_cast<int>(msg["request_seq"].integer(-1));
            auto it = pending_.find(seq);
            if (it == pending_.end()) continue;
            ResponseFn fn = std::move(it->second);
            pending_.erase(it);
            if (fn) fn(msg);
        } else if (type == "event") {
            if (on_event_) on_event_(msg);
        } else if (type == "request") {
            if (on_request_) on_request_(msg);
            else respond(msg, false, Json(), "unsupported request");
        }
        if (!running_) break;  // a callback stopped the client
    }
    if (exited_ && !exit_reported_) {
        bool drained;
        {
            std::lock_guard<std::mutex> lock(in_mutex_);
            drained = inbox_.empty();
        }
        if (drained) {
            exit_reported_ = true;
            if (on_exit_) on_exit_();
            ran = true;
        }
    }
    return ran;
}

} // namespace oh
