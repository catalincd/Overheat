// Language-server integration: document sync, diagnostics, completion, go to definition.
// LSP callbacks arrive on the client's reader thread and are marshalled to the UI thread.
#include "app.h"
#include "editor/utf8.h"
#include <cctype>

namespace oh {

namespace {
std::string server_for(const std::string& lang, std::vector<std::string>& args) {
    args.clear();
    if (lang == "cpp" || lang == "c") return "clangd";
    if (lang == "python") return "pylsp";
    if (lang == "rust") return "rust-analyzer";
    if (lang == "go") return "gopls";
    if (lang == "typescript" || lang == "javascript") {
        args = {"--stdio"};
        return "typescript-language-server";
    }
    return "";
}
bool is_ident(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// rust-analyzer's root is the Cargo package or workspace: the project folder when it
// has a Cargo.toml, else the nearest folder above the file that has one.
fs::path server_root(const std::string& server, const fs::path& file, const fs::path& project) {
    std::error_code ec;
    if (server != "rust-analyzer" || fs::exists(project / "Cargo.toml", ec)) return project;
    for (fs::path d = file.parent_path(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "Cargo.toml", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return project;
}

// clangd looks for compile_commands.json next to the sources, in their parent folders
// and in each one's build/ folder. CMake build trees named otherwise (build-release/,
// cmake-build-debug/, out/) are passed explicitly.
void add_clangd_args(const fs::path& project, std::vector<std::string>& args) {
    std::error_code ec;
    if (fs::exists(project / "compile_commands.json", ec) || fs::exists(project / "build" / "compile_commands.json", ec))
        return;
    for (auto& entry : fs::directory_iterator(project, ec)) {
        if (entry.is_directory(ec) && fs::exists(entry.path() / "compile_commands.json", ec)) {
            args.push_back("--compile-commands-dir=" + entry.path().string());
            return;
        }
    }
}
} // namespace

void App::lsp_post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(lsp_mutex_);
        lsp_tasks_.push_back(std::move(fn));
    }
    request_redraw();
}

size_t App::lsp_col_to_byte(const fs::path& path, size_t line, size_t col) {
    if (lsp_.utf8_positions()) return col;
    for (auto& tab : tab_bar_.tabs())
        if (tab.editor && tab.editor->buffer().file_path() == path)
            return utf8::byte_col_from_utf16(tab.editor->buffer().line_text(line), col);
    return col;
}

Position App::lsp_position(Editor& ed) {
    Position p = ed.cursors().primary_pos();
    if (!lsp_.utf8_positions()) p.col = utf8::utf16_col(ed.buffer().line_text(p.line), p.col);
    return p;
}

void App::setup_lsp_for_file(const fs::path& path) {
    if (vfs_->is_remote()) return;  // language servers run locally only (for now)
    if (Editor* ed = current_editor(); ed && (ed->large_file() || ed->read_only())) return;
    std::string lang = SyntaxHighlighter::detect_language(path);
    std::vector<std::string> args;
    std::string server = server_for(lang, args);
    if (server.empty()) return;
    if (!lsp_.is_running()) {
        if (server == "clangd") add_clangd_args(project_root_, args);
        if (!lsp_.start(server, args)) return;
        lsp_server_ = server;
        lsp_.set_on_diagnostics([this](const fs::path& file, std::vector<LSPDiagnostic> diags) {
            lsp_post([this, file, diags = std::move(diags)]() mutable {
                for (auto& d : diags) {
                    d.start.col = lsp_col_to_byte(file, d.start.line, d.start.col);
                    d.end.col = lsp_col_to_byte(file, d.end.line, d.end.col);
                }
                diagnostics_[file.string()] = std::move(diags);
            });
        });
        lsp_.initialize(server_root(server, path, project_root_));
    }
    if (server != lsp_server_ || lsp_.is_open(path)) return;  // one server, one language family
    Editor* ed = current_editor();
    if (!ed) return;
    lsp_.did_open(path, lang, ed->buffer().text());
    lsp_.mark_open(path);
    lsp_docs_[path.string()] = {ed->buffer().version(), Clock::now(), ed->buffer().version()};
}

void App::lsp_update() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(lsp_mutex_);
        tasks.swap(lsp_tasks_);
    }
    for (auto& t : tasks) t();

    if (lsp_.is_running()) {
        // Full-text sync after a short pause in typing.
        auto now = Clock::now();
        for (auto& tab : tab_bar_.tabs()) {
            if (!tab.editor) continue;
            const fs::path& p = tab.editor->buffer().file_path();
            auto it = lsp_docs_.find(p.string());
            if (it == lsp_docs_.end()) continue;
            uint64_t v = tab.editor->buffer().version();
            if (v != it->second.version) {
                it->second.version = v;
                it->second.changed = now;
            }
            if (it->second.sent != v && now - it->second.changed > std::chrono::milliseconds(300)) {
                lsp_.did_change(p, tab.editor->buffer().text());
                it->second.sent = v;
            }
        }
    }

    Editor* ed = current_editor();
    auto it = ed ? diagnostics_.find(ed->buffer().file_path().string()) : diagnostics_.end();
    editor_view_.set_diagnostics(it == diagnostics_.end() ? nullptr : &it->second);
    int errors = 0, warnings = 0;
    for (auto& [file, list] : diagnostics_)
        for (auto& d : list) {
            if (d.severity == 1) errors++;
            else if (d.severity == 2) warnings++;
        }
    status_bar_.set_diagnostic_counts(errors, warnings);
}

void App::lsp_maybe_complete(const std::string& typed, bool manual) {
    Editor* ed = current_editor();
    if (!ed || !lsp_.is_running() || !lsp_.is_open(ed->buffer().file_path())) return;
    auto& popup = editor_view_.completion();
    Position cur = ed->cursors().primary_pos();
    std::string line = ed->buffer().line_text(cur.line);
    bool member = false, ident = false;
    if (!typed.empty()) {
        unsigned char c = static_cast<unsigned char>(typed.back());
        member = c == '.' || (c == '>' && cur.col >= 2 && line[cur.col - 2] == '-') ||
                 (c == ':' && cur.col >= 2 && line[cur.col - 2] == ':');
        ident = is_ident(c);
    }
    // While a list is open, more identifier characters only narrow it; member
    // access (., ->, ::) always asks the server again.
    if (popup.visible() && !manual && !member) {
        popup.refilter(*ed);
        return;
    }
    if (!manual && !member && !ident) {
        popup.hide();
        return;
    }
    size_t start = cur.col;
    while (start > 0 && is_ident(static_cast<unsigned char>(line[start - 1]))) start--;
    // Don't pop up for a lone digit or mid-number.
    if (!manual && start < cur.col && std::isdigit(static_cast<unsigned char>(line[start]))) return;
    Position anchor{cur.line, start};
    fs::path path = ed->buffer().file_path();
    popup.hide();
    uint64_t request = ++completion_request_;
    lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);  // flush edits first
    lsp_update();
    lsp_.completion(path, lsp_position(*ed), [this, path, anchor, request](std::vector<LSPCompletion> items) {
        lsp_post([this, path, anchor, request, items = std::move(items)]() mutable {
            if (request != completion_request_) return;  // a newer request superseded this one
            Editor* ed = current_editor();
            if (!ed || ed->buffer().file_path() != path) return;
            if (ed->cursors().primary_pos().line != anchor.line) return;
            auto& popup = editor_view_.completion();
            popup.show(std::move(items), anchor);
            popup.refilter(*ed);
        });
    });
}

void App::lsp_goto_definition() {
    Editor* ed = current_editor();
    if (!ed || !lsp_.is_running()) return;
    fs::path path = ed->buffer().file_path();
    lsp_.definition(path, lsp_position(*ed), [this](std::vector<LSPLocation> locs) {
        lsp_post([this, locs = std::move(locs)] {
            if (locs.empty()) {
                set_status_message("No definition found");
                return;
            }
            const LSPLocation& l = locs.front();
            open_file(l.path);
            size_t col = lsp_col_to_byte(l.path, l.pos.line, l.pos.col);
            open_at(l.path, l.pos.line, col, 0);
        });
    });
}

} // namespace oh
