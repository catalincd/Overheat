#include "lsp/client.h"
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>

namespace oh {

// --- JSON helpers (crude but functional for MVP) ---

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 32);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static std::string find_string_value(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '"') { result += '"'; pos += 2; continue; }
            if (next == '\\') { result += '\\'; pos += 2; continue; }
            if (next == 'n') { result += '\n'; pos += 2; continue; }
            if (next == 'r') { result += '\r'; pos += 2; continue; }
            if (next == 't') { result += '\t'; pos += 2; continue; }
            if (next == 'u' && pos + 5 < json.size()) {
                auto hex4 = [&](size_t at) {
                    return static_cast<uint32_t>(std::strtoul(json.substr(at, 4).c_str(), nullptr, 16));
                };
                uint32_t cp = hex4(pos + 2);
                pos += 6;
                if (cp >= 0xD800 && cp < 0xDC00 && pos + 5 < json.size() && json[pos] == '\\' &&
                    json[pos + 1] == 'u') {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4(pos + 2) - 0xDC00);  // surrogate pair
                    pos += 6;
                }
                if (cp < 0x80) {
                    result += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    result += static_cast<char>(0xC0 | (cp >> 6));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    result += static_cast<char>(0xE0 | (cp >> 12));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                }
                continue;
            }
            result += next; pos += 2; continue;
        }
        if (c == '"') break;
        result += c;
        pos++;
    }
    return result;
}

static int find_int_value(const std::string& json, const std::string& key, int default_val = 0) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;
    pos += needle.size();
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return default_val;
    // handle negative
    bool neg = false;
    if (json[pos] == '-') { neg = true; pos++; }
    int val = 0;
    bool found = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
        found = true;
    }
    if (!found) return default_val;
    return neg ? -val : val;
}

// Extract balanced content starting from the character at start_pos (which should be { or [)
static std::string extract_balanced(const std::string& json, size_t start_pos) {
    if (start_pos >= json.size()) return {};
    char open = json[start_pos];
    char close_char = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    size_t i = start_pos;
    while (i < json.size()) {
        char c = json[i];
        if (in_string) {
            if (c == '\\') { i += 2; continue; }
            if (c == '"') in_string = false;
        } else {
            if (c == '"') in_string = true;
            else if (c == open) depth++;
            else if (c == close_char) {
                depth--;
                if (depth == 0) return json.substr(start_pos, i - start_pos + 1);
            }
        }
        i++;
    }
    return {};
}

static std::string find_object_value(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '{') return {};
    return extract_balanced(json, pos);
}

static std::string find_array(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '[') return {};
    return extract_balanced(json, pos);
}

// Find "result": value in a JSON-RPC response (could be object, array, string, number, null)
static std::string find_result_value(const std::string& json) {
    std::string needle = "\"result\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size()) return {};
    char c = json[pos];
    if (c == '{' || c == '[') return extract_balanced(json, pos);
    if (c == '"') {
        // string value
        size_t end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            end++;
        }
        return json.substr(pos, end - pos + 1);
    }
    // number, true, false, null
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']'
           && json[end] != ' ' && json[end] != '\n' && json[end] != '\r') end++;
    return json.substr(pos, end - pos);
}

static std::vector<std::string> split_array(const std::string& arr) {
    std::vector<std::string> items;
    if (arr.size() < 2) return items;
    // Strip outer brackets
    size_t i = 1; // skip '['
    size_t end = arr.size() - 1; // skip ']'
    while (i < end) {
        // skip whitespace
        while (i < end && (arr[i] == ' ' || arr[i] == '\t' || arr[i] == '\n' || arr[i] == '\r' || arr[i] == ',')) i++;
        if (i >= end) break;
        char c = arr[i];
        if (c == '{' || c == '[') {
            std::string obj = extract_balanced(arr, i);
            if (obj.empty()) break;
            items.push_back(obj);
            i += obj.size();
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < end) {
                if (arr[j] == '\\') { j += 2; continue; }
                if (arr[j] == '"') break;
                j++;
            }
            items.push_back(arr.substr(i, j - i + 1));
            i = j + 1;
        } else {
            // number, bool, null
            size_t j = i;
            while (j < end && arr[j] != ',' && arr[j] != '}' && arr[j] != ']') j++;
            std::string token = arr.substr(i, j - i);
            // trim
            while (!token.empty() && (token.back() == ' ' || token.back() == '\n' || token.back() == '\r' || token.back() == '\t'))
                token.pop_back();
            if (!token.empty()) items.push_back(token);
            i = j;
        }
    }
    return items;
}

static fs::path path_from_uri(const std::string& uri) {
    if (uri.compare(0, 7, "file://") == 0) {
        return fs::path(uri.substr(7));
    }
    return fs::path(uri);
}

static std::vector<LSPLocation> parse_locations(const std::string& result_json) {
    std::vector<LSPLocation> locs;
    if (result_json.empty() || result_json == "null") return locs;

    // Could be a single Location object or an array of Locations
    if (result_json[0] == '[') {
        auto items = split_array(result_json);
        for (auto& item : items) {
            std::string uri = find_string_value(item, "uri");
            std::string range = find_object_value(item, "range");
            std::string start_obj = find_object_value(range, "start");
            int line = find_int_value(start_obj, "line");
            int character = find_int_value(start_obj, "character");
            LSPLocation loc;
            loc.path = path_from_uri(uri);
            loc.pos = {static_cast<size_t>(line), static_cast<size_t>(character)};
            locs.push_back(std::move(loc));
        }
    } else if (result_json[0] == '{') {
        std::string uri = find_string_value(result_json, "uri");
        std::string range = find_object_value(result_json, "range");
        std::string start_obj = find_object_value(range, "start");
        int line = find_int_value(start_obj, "line");
        int character = find_int_value(start_obj, "character");
        LSPLocation loc;
        loc.path = path_from_uri(uri);
        loc.pos = {static_cast<size_t>(line), static_cast<size_t>(character)};
        locs.push_back(std::move(loc));
    }
    return locs;
}

// --- LSPClient implementation ---

LSPClient::LSPClient() {}

LSPClient::~LSPClient() {
    stop();
}

std::string LSPClient::uri_from_path(const fs::path& path) const {
    return "file://" + fs::absolute(path).string();
}

bool LSPClient::start(const std::string& command, const std::vector<std::string>& args) {
    if (running_) return false;

    std::vector<std::string> argv{command};
    argv.insert(argv.end(), args.begin(), args.end());
    process_ = std::make_unique<platform::Process>();
    if (!process_->start(argv)) {
        process_.reset();
        return false;
    }
    rpos_ = rlen_ = 0;
    running_ = true;

    reader_thread_ = std::thread(&LSPClient::reader_loop, this);
    writer_thread_ = std::thread(&LSPClient::writer_loop, this);
    return true;
}

void LSPClient::stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        running_ = false;
        outbox_.clear();
    }
    outbox_cv_.notify_all();

    // Stopping the server first makes any blocked write/read return (EPIPE/EOF),
    // so both threads can be joined before the pipes are closed.
    if (process_) process_->terminate();
    if (writer_thread_.joinable()) writer_thread_.join();
    if (reader_thread_.joinable()) reader_thread_.join();
    if (process_) process_->wait();
    process_.reset();

    // Clear pending requests
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_requests_.clear();
}

void LSPClient::send_message(const std::string& json) {
    if (!process_) return;
    std::string message = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        if (!running_) return;
        outbox_.push_back(std::move(message));
    }
    outbox_cv_.notify_one();
}

void LSPClient::writer_loop() {
    for (;;) {
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(outbox_mutex_);
            outbox_cv_.wait(lock, [this] { return !running_ || !outbox_.empty(); });
            if (!running_) return;
            batch.swap(outbox_);
        }
        for (auto& message : batch) {
            if (!process_->write(message.data(), message.size())) return;  // server gone
        }
    }
}

void LSPClient::send_request(const std::string& method, const std::string& params,
                             std::function<void(const std::string&)> callback) {
    int id;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        id = next_id_++;
        if (callback) {
            pending_requests_[id] = std::move(callback);
        }
    }

    std::string json = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                       ",\"method\":\"" + method + "\",\"params\":" + params + "}";
    send_message(json);
}

void LSPClient::send_notification(const std::string& method, const std::string& params) {
    std::string json = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method +
                       "\",\"params\":" + params + "}";
    send_message(json);
}

bool LSPClient::read_byte(char& c) {
    if (rpos_ == rlen_) {
        long n = process_->read(rbuf_, sizeof(rbuf_));
        if (n <= 0) return false;
        rpos_ = 0;
        rlen_ = static_cast<size_t>(n);
    }
    c = rbuf_[rpos_++];
    return true;
}

long LSPClient::read_bytes(char* out, size_t n) {
    if (rpos_ < rlen_) {  // drain what the header reader buffered first
        size_t take = std::min(n, rlen_ - rpos_);
        std::memcpy(out, rbuf_ + rpos_, take);
        rpos_ += take;
        return static_cast<long>(take);
    }
    return process_->read(out, n);
}

void LSPClient::reader_loop() {
    while (running_) {
        // Read headers byte by byte until \r\n\r\n
        std::string headers;
        int consecutive_newlines = 0;
        bool got_headers = false;

        while (running_) {
            char c;
            if (!read_byte(c)) {
                running_ = false;
                return;
            }
            headers += c;

            // Detect \r\n\r\n
            if (headers.size() >= 4) {
                size_t len = headers.size();
                if (headers[len-4] == '\r' && headers[len-3] == '\n' &&
                    headers[len-2] == '\r' && headers[len-1] == '\n') {
                    got_headers = true;
                    break;
                }
            }
        }

        if (!got_headers) break;

        // Parse Content-Length
        int content_length = 0;
        {
            std::string cl_key = "Content-Length: ";
            auto pos = headers.find(cl_key);
            if (pos != std::string::npos) {
                pos += cl_key.size();
                content_length = 0;
                while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
                    content_length = content_length * 10 + (headers[pos] - '0');
                    pos++;
                }
            }
        }

        if (content_length <= 0) continue;

        // Read body
        std::string body(static_cast<size_t>(content_length), '\0');
        size_t total_read = 0;
        while (total_read < static_cast<size_t>(content_length) && running_) {
            long n = read_bytes(&body[total_read], static_cast<size_t>(content_length) - total_read);
            if (n <= 0) {
                running_ = false;
                return;
            }
            total_read += static_cast<size_t>(n);
        }

        if (total_read == static_cast<size_t>(content_length)) {
            handle_message(body);
        }
    }
}

void LSPClient::handle_message(const std::string& json) {
    // Check if it's a response (has "id" and "result" or "error")
    // We need to distinguish responses from requests (server->client requests also have "id" + "method")
    bool has_method = json.find("\"method\"") != std::string::npos;
    bool has_result = json.find("\"result\"") != std::string::npos;
    bool has_error = json.find("\"error\"") != std::string::npos;

    if ((has_result || has_error) && !has_method) {
        // It's a response
        int id = find_int_value(json, "id", -1);
        std::function<void(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                callback = std::move(it->second);
                pending_requests_.erase(it);
            }
        }
        if (callback) {
            callback(json);
        }
    } else if (has_method) {
        // It's a notification or server request
        std::string method = find_string_value(json, "method");
        if (method == "textDocument/publishDiagnostics") {
            if (!on_diagnostics_) return;

            std::string params_str = find_object_value(json, "params");
            if (params_str.empty()) return;

            std::string uri = find_string_value(params_str, "uri");
            fs::path file_path = path_from_uri(uri);

            std::string diag_array = find_array(params_str, "diagnostics");
            std::vector<LSPDiagnostic> diagnostics;

            if (!diag_array.empty()) {
                auto items = split_array(diag_array);
                for (auto& item : items) {
                    LSPDiagnostic diag;

                    std::string range = find_object_value(item, "range");
                    std::string start_obj = find_object_value(range, "start");
                    std::string end_obj = find_object_value(range, "end");

                    diag.start.line = static_cast<size_t>(find_int_value(start_obj, "line"));
                    diag.start.col = static_cast<size_t>(find_int_value(start_obj, "character"));
                    diag.end.line = static_cast<size_t>(find_int_value(end_obj, "line"));
                    diag.end.col = static_cast<size_t>(find_int_value(end_obj, "character"));
                    diag.message = find_string_value(item, "message");
                    diag.severity = find_int_value(item, "severity", 1);

                    diagnostics.push_back(std::move(diag));
                }
            }

            on_diagnostics_(file_path, std::move(diagnostics));
        }
    }
}

// --- LSP protocol methods ---

void LSPClient::initialize(const fs::path& root_uri) {
    std::string uri = uri_from_path(root_uri);
    std::string params =
        "{\"processId\":" + std::to_string(platform::process_id()) +
        ",\"rootUri\":\"" + escape_json(uri) + "\""
        ",\"capabilities\":{"
            "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
            "\"textDocument\":{"
                "\"synchronization\":{\"didSave\":true,\"didOpen\":true,\"didClose\":true},"
                "\"publishDiagnostics\":{},"
                "\"completion\":{\"completionItem\":{\"snippetSupport\":false}},"
                "\"hover\":{},"
                "\"definition\":{},"
                "\"references\":{}"
            "}"
        "}}";

    send_request("initialize", params, [this](const std::string& response) {
        // LSP 3.17 position encoding negotiation; servers that ignore it use UTF-16.
        utf8_positions_ = find_string_value(response, "positionEncoding") == "utf-8";
        send_notification("initialized", "{}");
    });
}

void LSPClient::did_open(const fs::path& path, const std::string& language, const std::string& text) {
    std::string uri = uri_from_path(path);
    std::string escaped_text = escape_json(text);
    std::string params =
        "{\"textDocument\":{"
            "\"uri\":\"" + escape_json(uri) + "\","
            "\"languageId\":\"" + escape_json(language) + "\","
            "\"version\":" + std::to_string(versions_[uri] = 1) + ","
            "\"text\":\"" + escaped_text + "\""
        "}}";
    send_notification("textDocument/didOpen", params);
}

void LSPClient::did_change(const fs::path& path, const std::string& text) {
    std::string uri = uri_from_path(path);
    std::string escaped_text = escape_json(text);
    std::string params =
        "{\"textDocument\":{"
            "\"uri\":\"" + escape_json(uri) + "\","
            "\"version\":" + std::to_string(++versions_[uri]) +
        "},\"contentChanges\":[{\"text\":\"" + escaped_text + "\"}]}";
    send_notification("textDocument/didChange", params);
}

void LSPClient::did_save(const fs::path& path) {
    std::string uri = uri_from_path(path);
    std::string params = "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"}}";
    send_notification("textDocument/didSave", params);
}

void LSPClient::did_close(const fs::path& path) {
    std::string uri = uri_from_path(path);
    std::string params = "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"}}";
    send_notification("textDocument/didClose", params);
}

void LSPClient::completion(const fs::path& path, Position pos, CompletionCallback cb) {
    std::string uri = uri_from_path(path);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"},"
        "\"position\":{\"line\":" + std::to_string(pos.line) +
        ",\"character\":" + std::to_string(pos.col) + "}}";

    send_request("textDocument/completion", params,
        [cb = std::move(cb)](const std::string& json) {
            std::vector<LSPCompletion> completions;
            std::string result = find_result_value(json);
            if (result.empty() || result == "null") {
                if (cb) cb(completions);
                return;
            }

            // Result could be CompletionList {items:[...]} or CompletionItem[]
            std::string items_str;
            if (result[0] == '{') {
                items_str = find_array(result, "items");
            } else if (result[0] == '[') {
                items_str = result;
            }

            if (!items_str.empty()) {
                auto items = split_array(items_str);
                for (auto& item : items) {
                    LSPCompletion comp;
                    comp.label = find_string_value(item, "label");
                    comp.detail = find_string_value(item, "detail");
                    comp.insert_text = find_string_value(item, "insertText");
                    if (comp.insert_text.empty()) comp.insert_text = comp.label;
                    comp.kind = find_int_value(item, "kind", 1);
                    completions.push_back(std::move(comp));
                }
            }

            if (cb) cb(std::move(completions));
        });
}

void LSPClient::hover(const fs::path& path, Position pos, HoverCallback cb) {
    std::string uri = uri_from_path(path);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"},"
        "\"position\":{\"line\":" + std::to_string(pos.line) +
        ",\"character\":" + std::to_string(pos.col) + "}}";

    send_request("textDocument/hover", params,
        [cb = std::move(cb)](const std::string& json) {
            std::string result = find_result_value(json);
            if (result.empty() || result == "null") {
                if (cb) cb(std::nullopt);
                return;
            }

            LSPHover hover;

            // contents can be MarkedString (string), MarkupContent ({kind,value}), or array
            std::string contents_str = find_object_value(result, "contents");
            if (!contents_str.empty()) {
                // MarkupContent: {kind, value}
                hover.contents = find_string_value(contents_str, "value");
                if (hover.contents.empty()) {
                    // might be a simple string? Not an object.
                    hover.contents = contents_str;
                }
            } else {
                // Try as a plain string
                hover.contents = find_string_value(result, "contents");
            }

            // Parse range if present
            std::string range = find_object_value(result, "range");
            if (!range.empty()) {
                std::string start_obj = find_object_value(range, "start");
                std::string end_obj = find_object_value(range, "end");
                hover.start.line = static_cast<size_t>(find_int_value(start_obj, "line"));
                hover.start.col = static_cast<size_t>(find_int_value(start_obj, "character"));
                hover.end.line = static_cast<size_t>(find_int_value(end_obj, "line"));
                hover.end.col = static_cast<size_t>(find_int_value(end_obj, "character"));
            }

            if (cb) cb(std::move(hover));
        });
}

void LSPClient::definition(const fs::path& path, Position pos, DefinitionCallback cb) {
    std::string uri = uri_from_path(path);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"},"
        "\"position\":{\"line\":" + std::to_string(pos.line) +
        ",\"character\":" + std::to_string(pos.col) + "}}";

    send_request("textDocument/definition", params,
        [cb = std::move(cb)](const std::string& json) {
            std::string result = find_result_value(json);
            auto locations = parse_locations(result);
            if (cb) cb(std::move(locations));
        });
}

void LSPClient::references(const fs::path& path, Position pos, ReferencesCallback cb) {
    std::string uri = uri_from_path(path);
    std::string params =
        "{\"textDocument\":{\"uri\":\"" + escape_json(uri) + "\"},"
        "\"position\":{\"line\":" + std::to_string(pos.line) +
        ",\"character\":" + std::to_string(pos.col) + "},"
        "\"context\":{\"includeDeclaration\":true}}";

    send_request("textDocument/references", params,
        [cb = std::move(cb)](const std::string& json) {
            std::string result = find_result_value(json);
            auto locations = parse_locations(result);
            if (cb) cb(std::move(locations));
        });
}

} // namespace oh
