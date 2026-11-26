// Navigation requests of the LSP client: hover (markdown), definition and friends,
// references, rename, document / workspace symbols, clangd's switch header/source
// and inlay hints, plus cancellation, work-done progress and the server->client
// requests that servers block on. Responses are parsed with the JSON DOM (json.h).
#include "lsp/client.h"
#include "lsp/json.h"
#include "lsp/workspace_edit.h"

namespace oh {

namespace {

std::string pos_json(Position p) {
    return "{\"line\":" + std::to_string(p.line) + ",\"character\":" + std::to_string(p.col) + "}";
}

std::string error_message(const lsp_json::Value& msg) {
    if (msg.is_null()) return "language server not running";
    const lsp_json::Value& err = msg["error"];
    if (err.is_null()) return {};
    std::string m = err["message"].str();
    return m.empty() ? "request failed (" + std::to_string(err["code"].as_int()) + ")" : m;
}

// MarkedString | MarkedString[] | MarkupContent -> markdown.
std::string hover_text(const lsp_json::Value& c) {
    if (c.is_string()) return c.str();
    if (c.is_array()) {
        std::string out;
        for (auto& part : c.arr) {
            std::string t = hover_text(part);
            if (t.empty()) continue;
            if (!out.empty()) out += "\n\n";
            out += t;
        }
        return out;
    }
    if (c.is_object()) {
        if (c.has("language")) return "```" + c["language"].str() + "\n" + c["value"].str() + "\n```";
        return c["value"].str();  // MarkupContent (markdown or plaintext)
    }
    return {};
}

void add_location(const lsp_json::Value& l, std::vector<LSPRangeLoc>& out) {
    LSPRangeLoc loc;
    if (l.has("targetUri")) {  // LocationLink
        loc.path = lsp_uri_to_path(l["targetUri"].str());
        lsp_parse_range(l.has("targetSelectionRange") ? l["targetSelectionRange"] : l["targetRange"], loc.start, loc.end);
    } else if (l.has("uri")) {
        loc.path = lsp_uri_to_path(l["uri"].str());
        lsp_parse_range(l["range"], loc.start, loc.end);
    } else {
        return;
    }
    out.push_back(std::move(loc));
}

void add_document_symbols(const lsp_json::Value& arr, const fs::path& path, int depth, const std::string& container,
                          std::vector<LSPSymbol>& out) {
    for (auto& s : arr.arr) {
        LSPSymbol sym;
        sym.name = s["name"].str();
        sym.detail = s["detail"].str();
        sym.kind = s["kind"].as_int();
        sym.depth = depth;
        if (s.has("location")) {  // SymbolInformation (flat)
            sym.path = lsp_uri_to_path(s["location"]["uri"].str());
            lsp_parse_range(s["location"]["range"], sym.start, sym.end);
            sym.sel = sym.start;
            sym.container = s["containerName"].str();
        } else {
            sym.path = path;
            lsp_parse_range(s["range"], sym.start, sym.end);
            Position e;
            lsp_parse_range(s["selectionRange"], sym.sel, e);
            sym.container = container;
        }
        std::string name = sym.name;
        out.push_back(std::move(sym));
        if (s["children"].is_array()) add_document_symbols(s["children"], path, depth + 1, name, out);
    }
}

} // namespace

std::string LSPClient::text_document_position(const fs::path& path, Position pos) const {
    return "{\"textDocument\":{\"uri\":" + lsp_json::quote(uri_from_path(path)) + "},\"position\":" + pos_json(pos) + "}";
}

int LSPClient::request(const std::string& method, const std::string& params,
                       std::function<void(const lsp_json::Value& response)> cb) {
    if (!running_) {
        if (cb) cb(lsp_json::Value{});
        return 0;
    }
    return send_request(method, params, [cb = std::move(cb)](const std::string& raw) {
        if (!cb) return;
        cb(raw.empty() ? lsp_json::Value{} : lsp_json::parse(raw));
    });
}

void LSPClient::cancel(int id) {
    if (id <= 0) return;
    bool was_pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        was_pending = pending_requests_.erase(id) > 0;
    }
    if (was_pending && running_) send_notification("$/cancelRequest", "{\"id\":" + std::to_string(id) + "}");
}

void LSPClient::fail_pending() {
    if (stopping_) return;
    std::unordered_map<int, std::function<void(const std::string&)>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.swap(pending_requests_);
    }
    {
        std::lock_guard<std::mutex> lock(nav_mutex_);
        progress_.clear();
    }
    for (auto& [id, cb] : pending)
        if (cb) cb(std::string());  // empty = no response (old parsers see an empty result)
    request_redraw();
}

bool LSPClient::has_provider(const std::string& name) const {
    std::lock_guard<std::mutex> lock(nav_mutex_);
    if (server_caps_.empty()) return false;
    auto it = provider_cache_.find(name);
    if (it != provider_cache_.end()) return it->second;
    lsp_json::Value caps = lsp_json::parse(server_caps_);
    const lsp_json::Value& v = caps["result"]["capabilities"][name];
    return provider_cache_[name] = v.is_object() || v.as_bool(false);
}

std::string LSPClient::progress() const {
    std::lock_guard<std::mutex> lock(nav_mutex_);
    std::string out;
    for (auto& [token, text] : progress_) {
        if (!out.empty()) out += "; ";
        out += text;
    }
    return out;
}

bool LSPClient::handle_server_message(const std::string& method, const std::string& raw) {
    lsp_json::Value msg = lsp_json::parse(raw);
    const lsp_json::Value& params = msg["params"];
    bool is_request = msg.has("id");
    auto reply = [&](const std::string& result) {
        send_message("{\"jsonrpc\":\"2.0\",\"id\":" + lsp_json::dump(msg["id"]) + ",\"result\":" + result + "}");
    };
    if (method == "$/progress") {
        std::string token = lsp_json::dump(params["token"]);
        const lsp_json::Value& v = params["value"];
        std::string kind = v["kind"].str();
        std::lock_guard<std::mutex> lock(nav_mutex_);
        if (kind == "end") {
            progress_.erase(token);
            refresh_epoch_++;
        } else {
            std::string& text = progress_[token];
            if (kind == "begin") text = v["title"].str();
            size_t colon = text.find(": ");
            std::string title = colon == std::string::npos ? text : text.substr(0, colon);
            std::string m = v["message"].str();
            text = m.empty() ? title : title + ": " + m;
        }
        request_redraw();
        return true;
    }
    if (method == "workspace/inlayHint/refresh") {
        refresh_epoch_++;
        request_redraw();
    }
    if (!is_request) return method.rfind("window/", 0) == 0 || method.rfind("$/", 0) == 0;
    // Edits the server wants applied (quick fixes that run a command) go to the app's
    // handler (tasks_glue.cpp) in LSPClient::handle_message.
    if (method == "workspace/applyEdit" && on_server_request_) return false;
    // Servers wait for these answers (rust-analyzer: progress tokens, capability
    // registration, configuration); unknown requests get a null result too.
    if (method == "workspace/configuration") {
        std::string arr = "[";
        for (size_t i = 0; i < params["items"].size(); i++) arr += i ? ",null" : "null";
        reply(arr + "]");
    } else {
        reply("null");
    }
    return true;
}

int LSPClient::hover_markdown(const fs::path& path, Position pos, HoverCallback cb) {
    return request("textDocument/hover", text_document_position(path, pos), [cb](const lsp_json::Value& msg) {
        const lsp_json::Value& r = msg["result"];
        if (r.is_null()) return cb(std::nullopt);
        LSPHover h;
        h.contents = hover_text(r["contents"]);
        h.start = h.end = {0, 0};
        if (r.has("range")) lsp_parse_range(r["range"], h.start, h.end);
        if (h.contents.find_first_not_of(" \n\t") == std::string::npos) return cb(std::nullopt);
        cb(std::move(h));
    });
}

int LSPClient::locations(const std::string& method, const fs::path& path, Position pos, LocationsCallback cb) {
    std::string params = text_document_position(path, pos);
    if (method == "textDocument/references") {
        params.pop_back();
        params += ",\"context\":{\"includeDeclaration\":true}}";
    }
    return request(method, params, [cb](const lsp_json::Value& msg) {
        std::vector<LSPRangeLoc> out;
        const lsp_json::Value& r = msg["result"];
        if (r.is_array())
            for (auto& l : r.arr) add_location(l, out);
        else if (r.is_object())
            add_location(r, out);
        cb(std::move(out));
    });
}

int LSPClient::prepare_rename(const fs::path& path, Position pos, std::function<void(LSPPrepareRename)> cb) {
    lsp_json::Value caps;
    {
        std::lock_guard<std::mutex> lock(nav_mutex_);
        if (!server_caps_.empty()) caps = lsp_json::parse(server_caps_);
    }
    if (running_ && !caps.is_null()) {
        const lsp_json::Value& rp = caps["result"]["capabilities"]["renameProvider"];
        if (!rp["prepareProvider"].as_bool(false)) {  // no prepare support: rename the word
            LSPPrepareRename p;
            p.ok = p.use_word = rp.is_object() || rp.as_bool(false);
            if (!p.ok) p.error = "This language server cannot rename symbols";
            cb(p);
            return 0;
        }
    }
    return request("textDocument/prepareRename", text_document_position(path, pos), [cb](const lsp_json::Value& msg) {
        LSPPrepareRename p;
        const lsp_json::Value& r = msg["result"];
        p.error = error_message(msg);
        if (r.is_object()) {
            if (r.has("range")) {
                lsp_parse_range(r["range"], p.start, p.end);
                p.placeholder = r["placeholder"].str();
                p.ok = true;
            } else if (r.has("start")) {
                lsp_parse_range(r, p.start, p.end);
                p.ok = true;
            } else if (r["defaultBehavior"].as_bool(false)) {
                p.ok = p.use_word = true;
            }
        }
        if (!p.ok && p.error.empty()) p.error = "The element can't be renamed";
        cb(std::move(p));
    });
}

int LSPClient::rename(const fs::path& path, Position pos, const std::string& new_name, RenameCallback cb) {
    std::string params = text_document_position(path, pos);
    params.pop_back();
    params += ",\"newName\":" + lsp_json::quote(new_name) + "}";
    return request("textDocument/rename", params, [cb](const lsp_json::Value& msg) {
        std::string err = error_message(msg);
        const lsp_json::Value& r = msg["result"];
        if (!err.empty() || !r.is_object()) return cb(std::nullopt, err.empty() ? "Nothing to rename" : err);
        cb(parse_workspace_edit(r), {});
    });
}

int LSPClient::document_symbols(const fs::path& path, SymbolsCallback cb) {
    std::string params = "{\"textDocument\":{\"uri\":" + lsp_json::quote(uri_from_path(path)) + "}}";
    return request("textDocument/documentSymbol", params, [cb, path](const lsp_json::Value& msg) {
        std::vector<LSPSymbol> out;
        add_document_symbols(msg["result"], path, 0, {}, out);
        cb(std::move(out));
    });
}

int LSPClient::workspace_symbols(const std::string& query, SymbolsCallback cb) {
    return request("workspace/symbol", "{\"query\":" + lsp_json::quote(query) + "}", [cb](const lsp_json::Value& msg) {
        std::vector<LSPSymbol> out;
        add_document_symbols(msg["result"], {}, 0, {}, out);
        cb(std::move(out));
    });
}

int LSPClient::switch_source_header(const fs::path& path, std::function<void(std::optional<fs::path>)> cb) {
    std::string params = "{\"uri\":" + lsp_json::quote(uri_from_path(path)) + "}";
    return request("textDocument/switchSourceHeader", params, [cb](const lsp_json::Value& msg) {
        const lsp_json::Value& r = msg["result"];
        if (r.is_string() && !r.str().empty()) return cb(lsp_uri_to_path(r.str()));
        cb(std::nullopt);
    });
}

int LSPClient::inlay_hints(const fs::path& path, Position start, Position end, InlayCallback cb) {
    std::string params = "{\"textDocument\":{\"uri\":" + lsp_json::quote(uri_from_path(path)) +
                         "},\"range\":{\"start\":" + pos_json(start) + ",\"end\":" + pos_json(end) + "}}";
    return request("textDocument/inlayHint", params, [cb](const lsp_json::Value& msg) {
        std::vector<LSPInlayHint> out;
        const lsp_json::Value& r = msg["result"];
        for (auto& h : r.arr) {
            LSPInlayHint hint;
            hint.pos.line = static_cast<size_t>(std::max(0, h["position"]["line"].as_int()));
            hint.pos.col = static_cast<size_t>(std::max(0, h["position"]["character"].as_int()));
            const lsp_json::Value& label = h["label"];
            if (label.is_string()) hint.label = label.str();
            else
                for (auto& part : label.arr) hint.label += part["value"].str();
            hint.kind = h["kind"].as_int();
            hint.pad_left = h["paddingLeft"].as_bool(false);
            hint.pad_right = h["paddingRight"].as_bool(false);
            if (!hint.label.empty()) out.push_back(std::move(hint));
        }
        cb(std::move(out), !msg.is_null() && !msg.has("error"));
    });
}

void LSPClient::did_change_watched_files(const std::vector<fs::path>& paths) {
    if (!running_ || paths.empty()) return;
    std::string changes;
    for (auto& p : paths) {
        if (!changes.empty()) changes += ",";
        changes += "{\"uri\":" + lsp_json::quote(uri_from_path(p)) + ",\"type\":2}";
    }
    send_notification("workspace/didChangeWatchedFiles", "{\"changes\":[" + changes + "]}");
}

} // namespace oh
