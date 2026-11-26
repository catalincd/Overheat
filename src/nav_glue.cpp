// LSP navigation: hover tooltips (mouse dwell, Ctrl+K Ctrl+I), Ctrl+hover links and
// Ctrl+click, go to definition / declaration / type definition / implementation,
// peek definition and references (peek_view.cpp), rename across files (F2),
// document and workspace symbols, clangd's switch header/source, inlay hints and
// back/forward history. Server callbacks arrive on the LSP reader thread and are
// marshalled with lsp_post(); every result is checked against a generation counter
// and the document version, so stale answers after edits are dropped.
#include "app.h"
#include "editor/utf8.h"
#include "lsp/json.h"
#include "lsp/workspace_edit.h"
#include "nav_history.h"
#include "ui/hover_popup.h"
#include "ui/peek_view.h"
#include "ui/rename_box.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace oh {

namespace {

bool ident(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// [start, end) of the identifier containing byte `col`.
std::optional<std::pair<size_t, size_t>> word_at(const std::string& line, size_t col) {
    if (col >= line.size() || !ident(static_cast<unsigned char>(line[col]))) return std::nullopt;
    size_t a = col, b = col;
    while (a > 0 && ident(static_cast<unsigned char>(line[a - 1]))) a--;
    while (b < line.size() && ident(static_cast<unsigned char>(line[b]))) b++;
    return std::pair{a, b};
}

uint32_t symbol_icon(int kind) {
    switch (kind) {  // LSP SymbolKind -> codicon
        case 1: return 0xEB60;                    // file
        case 2: case 3: case 4: return 0xEA8B;    // module / namespace / package
        case 5: return 0xEB5B;                    // class
        case 6: case 9: case 12: return 0xEA8C;   // method / constructor / function
        case 7: return 0xEB65;                    // property
        case 8: return 0xEB5F;                    // field
        case 10: return 0xEA95;                   // enum
        case 11: return 0xEB61;                   // interface (Rust traits)
        case 13: return 0xEA88;                   // variable
        case 14: return 0xEB5D;                   // constant
        case 15: return 0xEB8D;                   // string
        case 16: return 0xEA90;                   // number
        case 17: return 0xEA8F;                   // boolean
        case 18: return 0xEA8A;                   // array
        case 20: return 0xEA93;                   // key
        case 22: return 0xEB5E;                   // enum member
        case 23: return 0xEA91;                   // struct
        case 24: return 0xEA86;                   // event
        case 25: return 0xEB64;                   // operator
        case 26: return 0xEA92;                   // type parameter
        default: return 0xEA8B;
    }
}

const char* symbol_kind_name(int kind) {
    static const char* names[] = {"", "file", "module", "namespace", "package", "class", "method", "property",
                                  "field", "constructor", "enum", "interface", "function", "variable", "constant",
                                  "string", "number", "boolean", "array", "object", "key", "null", "enum member",
                                  "struct", "event", "operator", "type parameter"};
    return kind > 0 && kind < 27 ? names[kind] : "";
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::string content;
    if (!LocalVfs().read(p, content)) return lines;
    size_t start = 0;
    for (;;) {
        size_t nl = content.find('\n', start);
        std::string l = content.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

Uint32 SDLCALL wake_cb(void*, SDL_TimerID, Uint32) {
    request_redraw();
    return 0;
}
void wake_in(int ms) { SDL_AddTimer(static_cast<Uint32>(std::max(1, ms)), wake_cb, nullptr); }

constexpr auto HOVER_DELAY = std::chrono::milliseconds(500);
constexpr auto INLAY_DEBOUNCE = std::chrono::milliseconds(400);

} // namespace

struct App::NavState {
    HoverPopup hover;
    PeekView peek;
    RenameBox rename;
    NavHistory history;

    // Hover: the word under a resting pointer, or the cursor (keyboard).
    bool dwell_armed = false;
    TimePoint dwell_at{};
    fs::path dwell_path;
    Position dwell_pos{};
    std::pair<size_t, size_t> dwell_word{SIZE_MAX, SIZE_MAX};  // (line, word start)
    bool hover_keyboard = false;
    int hover_req = 0;
    uint64_t hover_gen = 0;

    // Ctrl+hover link.
    std::pair<size_t, size_t> link_word{SIZE_MAX, SIZE_MAX};
    fs::path link_path;
    bool link_shown = false;
    std::vector<LSPRangeLoc> link_targets;
    int link_req = 0;
    uint64_t link_gen = 0;
    SDL_Cursor* hand = nullptr;

    // Go to / peek / references / symbols / switch header (one at a time).
    int nav_req = 0;
    uint64_t nav_gen = 0;
    int ws_req = 0;
    std::vector<LSPSymbol> ws_syms;

    // History polling.
    bool last_valid = false;
    NavLocation last;
    const Editor* last_editor = nullptr;
    uint64_t last_version = 0;

    // Inlay hints of the active editor.
    fs::path inlay_path;
    uint64_t inlay_version = 0;       // buffer version the hints were computed for
    size_t inlay_lines = 0;           // line count then
    size_t inlay_first = 0, inlay_last = 0;
    std::vector<LSPInlayHint> inlay_hints, inlay_shown;
    uint64_t inlay_shown_version = UINT64_MAX;
    int inlay_req = 0;
    uint64_t inlay_req_version = 0;
    uint64_t inlay_epoch = 0;
    uint64_t inlay_seen_version = UINT64_MAX;
    TimePoint inlay_changed_at{}, inlay_retry_at{};
    bool inlay_valid = false;
    const Editor* inlay_editor = nullptr;

    // An explicit jump is about to leave the editor's cursor position: record it, and
    // make the per-frame poll start from it too so it doesn't add a stale entry.
    void jump_from(const Editor* ed) {
        NavLocation from{ed->buffer().file_path(), ed->cursors().primary_pos()};
        history.record(from);
        last = from;
        last_editor = ed;
        last_version = ed->buffer().version();
        last_valid = true;
    }
};

void App::NavStateDeleter::operator()(NavState* s) const {
    if (s && s->hand) SDL_DestroyCursor(s->hand);
    delete s;
}

// ---------------------------------------------------------------------------
// Setup and commands
// ---------------------------------------------------------------------------

void App::setup_nav() {
    nav_.reset(new NavState);
    auto add = [this](const std::string& name, const std::string& label, const std::string& keys) {
        commands_.push_back({name, label, [this, name]() { handle_command(name); }, keys});
    };
    add("peek_definition", "Peek Definition", "Alt+F12");
    add("goto_declaration", "Go to Declaration", "");
    add("goto_type_definition", "Go to Type Definition", "");
    add("goto_implementation", "Go to Implementations", "Ctrl+F12");
    add("find_references", "Find All References (peek)", "Shift+F12");
    add("rename_symbol", "Rename Symbol", "F2");
    add("document_symbols", "Go to Symbol in Editor...", "Ctrl+Shift+O");
    add("workspace_symbols", "Go to Symbol in Workspace...", "Ctrl+T");
    add("switch_header_source", "Switch Header/Source (clangd)", "Alt+O");
    add("show_hover", "Show Hover", "Ctrl+K Ctrl+I");
    add("toggle_inlay_hints", "View: Toggle Inlay Hints", "");
    add("navigate_back", "Go Back", "Ctrl+Alt+-");
    add("navigate_forward", "Go Forward", "Ctrl+Shift+-");
}

bool App::nav_command(const std::string& cmd) {
    if (!nav_) return false;
    auto goto_cmd = [&](const char* method, const char* what, bool peek) {
        Editor* ed = current_editor();
        if (!ed) return;
        fs::path path = ed->buffer().file_path();
        if (!lsp_.is_ready() || !lsp_.is_open(path)) {
            set_status_message(lsp_.is_running() ? "Language server is starting..." : "No language server for this file");
            return;
        }
        lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);  // flush edits first
        lsp_update();
        lsp_.cancel(nav_->nav_req);
        uint64_t gen = ++nav_->nav_gen;
        const Editor* from = ed;
        std::string label = what;
        nav_->nav_req = lsp_.locations(method, path, lsp_position(*ed), [this, gen, from, label, peek](std::vector<LSPRangeLoc> locs) {
            lsp_post([this, gen, from, label, peek, locs = std::move(locs)] {
                if (!nav_ || gen != nav_->nav_gen || current_editor() != from) return;  // superseded or switched away
                if (locs.empty()) {
                    set_status_message("No " + label + " found");
                    return;
                }
                if (locs.size() == 1 && !peek) {
                    nav_->jump_from(from);
                    const LSPRangeLoc& l = locs.front();
                    open_file(l.path);
                    open_at(l.path, l.start.line, lsp_col_to_byte(l.path, l.start.line, l.start.col), 0);
                    return;
                }
                // Several results (or a peek): the inline peek view.
                std::vector<PeekView::Item> items;
                std::unordered_map<std::string, std::vector<std::string>> files;
                fs::path here = from->buffer().file_path();
                for (auto& l : locs) {
                    PeekView::Item it{l.path, l.start, l.end, {}};
                    std::shared_ptr<Editor> open;
                    for (auto* tb : {&tab_bar_, &tab_bar2_})
                        for (auto& t : tb->tabs())
                            if (t.editor && t.editor->buffer().file_path() == l.path) open = t.editor;
                    if (open && l.start.line < open->buffer().line_count()) {
                        it.line_text = open->buffer().line_text(l.start.line);
                    } else {
                        auto& lines = files[l.path.string()];
                        if (lines.empty()) lines = read_lines(l.path);
                        if (l.start.line < lines.size()) it.line_text = lines[l.start.line];
                    }
                    if (!lsp_.utf8_positions()) {
                        it.start.col = utf8::byte_col_from_utf16(it.line_text, l.start.col);
                        it.end.col = l.end.line == l.start.line ? utf8::byte_col_from_utf16(it.line_text, l.end.col) : l.end.col;
                    }
                    items.push_back(std::move(it));
                }
                std::stable_sort(items.begin(), items.end(), [&](const PeekView::Item& a, const PeekView::Item& b) {
                    bool ah = a.path == here, bh = b.path == here;
                    if (ah != bh) return ah;
                    if (a.path != b.path) return a.path < b.path;
                    return a.start < b.start;
                });
                items.erase(std::unique(items.begin(), items.end(), [](const PeekView::Item& a, const PeekView::Item& b) {
                    return a.path == b.path && a.start == b.start;
                }), items.end());
                std::string title = label;
                if (!title.empty()) title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
                size_t anchor = from->cursors().primary_pos().line;
                nav_->hover.hide();
                nav_->peek.set_root(project_root_);
                nav_->peek.open(
                    title, std::move(items), anchor, from,
                    [this](const fs::path& p) -> std::shared_ptr<Editor> {
                        auto e = std::make_shared<Editor>();
                        std::shared_ptr<Editor> open;
                        for (auto* tb : {&tab_bar_, &tab_bar2_})
                            for (auto& t : tb->tabs())
                                if (t.editor && t.editor->buffer().file_path() == p) open = t.editor;
                        std::string content;
                        if (open) content = open->buffer().text();
                        else if (!LocalVfs().read(p, content)) return nullptr;
                        e->load_content(p, content);
                        e->set_read_only(true);
                        return e;
                    },
                    [this](const PeekView::Item& it) {
                        if (Editor* ed = current_editor())
                            nav_->jump_from(ed);
                        open_at(it.path, it.start.line, it.start.col, 0);
                    },
                    [this] { editor_view_.set_zone(std::nullopt, 0); });
                float lh = editor_view_.line_height();
                float zone_h = nav_peek_height();
                editor_view_.set_zone(anchor, zone_h);
                // Scroll so the anchor line and the whole peek are in view.
                if (Editor* ed = current_editor()) {
                    float view_h = compute_layout().editor.h;
                    float bottom = static_cast<float>(anchor + 1) * lh + zone_h;
                    float sy = ed->scroll_y();
                    if (bottom > sy + view_h) sy = bottom - view_h + lh;
                    sy = std::min(sy, static_cast<float>(anchor) * lh);
                    if (static_cast<float>(anchor) * lh < sy) sy = static_cast<float>(anchor) * lh;
                    ed->set_scroll(ed->scroll_x(), std::max(0.0f, sy));
                }
            });
        });
    };

    if (cmd == "goto_definition") goto_cmd("textDocument/definition", "definition", false);
    else if (cmd == "peek_definition") goto_cmd("textDocument/definition", "definition", true);
    else if (cmd == "goto_declaration") goto_cmd("textDocument/declaration", "declaration", false);
    else if (cmd == "goto_type_definition") goto_cmd("textDocument/typeDefinition", "type definition", false);
    else if (cmd == "goto_implementation") goto_cmd("textDocument/implementation", "implementations", false);
    else if (cmd == "find_references") goto_cmd("textDocument/references", "references", true);
    else if (cmd == "show_hover") {
        if (Editor* ed = current_editor()) {
            Position p = ed->cursors().primary_pos();
            std::string line = ed->buffer().line_text(p.line);
            if (p.col >= line.size() && p.col > 0) p.col--;  // cursor right after a word
            else if (p.col > 0 && !word_at(line, p.col) && word_at(line, p.col - 1)) p.col--;
            nav_->hover_keyboard = true;
            nav_->dwell_armed = false;
            // Reuse the dwell request path with the cursor's cell as the anchor.
            nav_->dwell_path = ed->buffer().file_path();
            nav_->dwell_pos = p;
            nav_->dwell_word = {p.line, SIZE_MAX};
            nav_->dwell_at = Clock::now() - HOVER_DELAY;
            nav_->dwell_armed = true;
            nav_->hover.hide();
        }
    } else if (cmd == "rename_symbol") {
        Editor* ed = current_editor();
        if (!ed) return true;
        fs::path path = ed->buffer().file_path();
        if (!lsp_.is_ready() || !lsp_.is_open(path)) {
            set_status_message(lsp_.is_running() ? "Language server is starting..." : "No language server for this file");
            return true;
        }
        if (ed->read_only()) {
            set_status_message("This editor is read-only");
            return true;
        }
        lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);
        lsp_update();
        Position at = ed->cursors().primary_pos();
        Position lsp_at = lsp_position(*ed);
        uint64_t version = ed->buffer().version();
        uint64_t gen = ++nav_->nav_gen;
        const Editor* from = ed;
        nav_->hover.hide();
        nav_->nav_req = lsp_.prepare_rename(path, lsp_at, [this, gen, from, path, at, lsp_at, version](LSPPrepareRename pr) {
            lsp_post([this, gen, from, path, at, lsp_at, version, pr = std::move(pr)] {
                Editor* ed = current_editor();
                if (!nav_ || gen != nav_->nav_gen || ed != from || ed->buffer().version() != version) return;
                if (!pr.ok) {
                    set_status_message(pr.error);
                    return;
                }
                std::string line = ed->buffer().line_text(at.line);
                Position a = at, b = at;
                if (pr.use_word || pr.start.line != pr.end.line) {
                    auto w = word_at(line, at.col < line.size() ? at.col : (at.col > 0 ? at.col - 1 : 0));
                    if (!w) {
                        set_status_message("The element can't be renamed");
                        return;
                    }
                    a.col = w->first;
                    b.col = w->second;
                } else {
                    a = {pr.start.line, lsp_col_to_byte(path, pr.start.line, pr.start.col)};
                    b = {pr.end.line, lsp_col_to_byte(path, pr.end.line, pr.end.col)};
                    line = ed->buffer().line_text(a.line);
                }
                std::string old = pr.placeholder.empty() ? line.substr(std::min(a.col, line.size()), b.col - std::min(a.col, b.col))
                                                         : pr.placeholder;
                ed->cursors().set_primary(a);
                ed->cursors().extend_primary(b);
                editor_view_.reveal_cursor();
                Rect anchor = editor_view_.char_rect(a);
                nav_->rename.open(anchor, old, [this, from, path, lsp_at, old](const std::string& name) {
                    if (name.empty() || name == old) {
                        nav_->rename.close();
                        return;
                    }
                    nav_->rename.set_busy(true);
                    uint64_t gen2 = ++nav_->nav_gen;
                    nav_->nav_req = lsp_.rename(path, lsp_at, name, [this, gen2, name, old](std::optional<LSPWorkspaceEdit> we, std::string err) {
                        lsp_post([this, gen2, name, old, we = std::move(we), err] {
                            if (!nav_ || gen2 != nav_->nav_gen) return;
                            nav_->rename.close();
                            if (!we) {
                                set_status_message("Rename failed: " + err, 6.0f);
                                return;
                            }
                            // Apply to open buffers (one undo step each) and to files on disk.
                            size_t edits = 0, files = 0, on_disk = 0, failed = 0;
                            std::vector<fs::path> disk_paths;
                            bool utf16 = !lsp_.utf8_positions();
                            for (auto& fe : we->files) {
                                if (fe.edits.empty()) continue;
                                std::vector<std::shared_ptr<Editor>> eds;
                                for (auto* tb : {&tab_bar_, &tab_bar2_})
                                    for (auto& t : tb->tabs())
                                        if (t.editor && t.editor->buffer().file_path() == fe.path &&
                                            std::find(eds.begin(), eds.end(), t.editor) == eds.end())
                                            eds.push_back(t.editor);
                                if (!eds.empty()) {
                                    for (auto& e : eds) {
                                        std::vector<ByteEdit> be;
                                        if (e->read_only() || !resolve_text_edits(e->buffer().text(), fe.edits, utf16, be)) {
                                            failed++;
                                            continue;
                                        }
                                        auto& buf = e->buffer();
                                        auto sel = e->cursors().selections().front();
                                        size_t anchor_off = map_offset_through(buf.offset_of(sel.anchor.line, sel.anchor.col), be);
                                        size_t head_off = map_offset_through(buf.offset_of(sel.head.line, sel.head.col), be);
                                        buf.begin_undo_group();
                                        for (auto it = be.rbegin(); it != be.rend(); ++it) buf.replace(it->offset, it->length, it->text);
                                        buf.end_undo_group();
                                        e->cursors().set_primary(buf.position_of(std::min(anchor_off, buf.length())));
                                        e->cursors().extend_primary(buf.position_of(std::min(head_off, buf.length())));
                                        e->sync_syntax();
                                    }
                                } else {
                                    // Not open: rewrite the file atomically (temp file + rename).
                                    std::string text, err2;
                                    if (!LocalVfs().read(fe.path, text) || !apply_text_edits(text, fe.edits, utf16, &err2)) {
                                        failed++;
                                        continue;
                                    }
                                    fs::path tmp = fe.path;
                                    tmp += ".overheat-rename.tmp";
                                    {
                                        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                                        out.write(text.data(), static_cast<std::streamsize>(text.size()));
                                        if (!out) {
                                            failed++;
                                            std::error_code ec;
                                            fs::remove(tmp, ec);
                                            continue;
                                        }
                                    }
                                    std::error_code ec;
                                    fs::permissions(tmp, fs::status(fe.path, ec).permissions(), ec);
                                    fs::rename(tmp, fe.path, ec);
                                    if (ec) {
                                        failed++;
                                        fs::remove(tmp, ec);
                                        continue;
                                    }
                                    disk_paths.push_back(fe.path);
                                    on_disk++;
                                    // clangd ignores watched-file events for headers: hand it the
                                    // new text once so dependent preambles are rebuilt.
                                    if (!lsp_.is_open(fe.path)) {
                                        lsp_.did_open(fe.path, SyntaxHighlighter::detect_language(fe.path), text);
                                        lsp_.did_close(fe.path);
                                    }
                                }
                                files++;
                                edits += fe.edits.size();
                            }
                            lsp_.did_change_watched_files(disk_paths);
                            std::string msg = "Renamed '" + old + "' to '" + name + "': " + std::to_string(edits) + " edit" +
                                              (edits == 1 ? "" : "s") + " in " + std::to_string(files) + " file" + (files == 1 ? "" : "s");
                            if (on_disk) msg += " (" + std::to_string(on_disk) + " saved on disk)";
                            if (failed) msg += ", " + std::to_string(failed) + " file(s) failed";
                            if (we->skipped_ops) msg += ", " + std::to_string(we->skipped_ops) + " file operation(s) skipped";
                            set_status_message(msg, 8.0f);
                        });
                    });
                }, [this] { set_status_message("Rename cancelled"); });
            });
        });
    } else if (cmd == "document_symbols") {
        Editor* ed = current_editor();
        if (!ed) return true;
        fs::path path = ed->buffer().file_path();
        if (!lsp_.is_ready() || !lsp_.is_open(path)) {
            set_status_message(lsp_.is_running() ? "Language server is starting..." : "No language server for this file");
            return true;
        }
        lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);
        lsp_update();
        uint64_t gen = ++nav_->nav_gen;
        const Editor* from = ed;
        nav_->nav_req = lsp_.document_symbols(path, [this, gen, from, path](std::vector<LSPSymbol> syms) {
            lsp_post([this, gen, from, path, syms = std::move(syms)]() mutable {
                Editor* ed = current_editor();
                if (!nav_ || gen != nav_->nav_gen || ed != from) return;
                if (syms.empty()) {
                    set_status_message("No symbols in this file");
                    return;
                }
                for (auto& s : syms) s.sel.col = lsp_col_to_byte(path, s.sel.line, s.sel.col);
                CommandPalette::PickOptions opts;
                opts.placeholder = "Go to symbol in " + path.filename().string();
                Position here = ed->cursors().primary_pos();
                int initial = 0;
                for (size_t i = 0; i < syms.size(); i++) {
                    auto& s = syms[i];
                    opts.items.push_back(std::string(static_cast<size_t>(s.depth) * 2, ' ') + s.name);
                    std::string d = s.detail.empty() ? symbol_kind_name(s.kind) : s.detail;
                    opts.details.push_back(d.substr(0, 60));
                    opts.icons.push_back(symbol_icon(s.kind));
                    if (s.start <= here) initial = static_cast<int>(i);  // the symbol around the cursor
                }
                opts.initial = initial;
                auto saved = ed->cursors();
                float sx = ed->scroll_x(), sy = ed->scroll_y();
                auto shared = std::make_shared<std::vector<LSPSymbol>>(std::move(syms));
                auto go = [this, from, shared](int i, bool preview) {
                    Editor* e = current_editor();
                    if (e != from || i < 0 || i >= static_cast<int>(shared->size())) return;
                    const LSPSymbol& s = (*shared)[static_cast<size_t>(i)];
                    Position p{std::min(s.sel.line, e->buffer().line_count() - 1), s.sel.col};
                    e->cursors().set_primary(p);
                    if (!preview) e->set_jump_line(p.line);
                    editor_view_.reveal_cursor();
                };
                opts.on_preview = [go](int i) { go(i, true); };
                opts.on_pick = [this, go](int i, const std::string&) {
                    go(i, false);
                    focus_editor();
                };
                opts.on_cancel = [this, from, saved, sx, sy] {
                    if (Editor* e = current_editor(); e == from) {
                        e->cursors() = saved;
                        e->set_scroll(sx, sy);
                    }
                };
                command_palette_.pick(std::move(opts));
            });
        });
    } else if (cmd == "workspace_symbols") {
        if (!lsp_.is_ready()) {
            set_status_message(lsp_.is_running() ? "Language server is starting..." : "No language server running");
            return true;
        }
        CommandPalette::PickOptions opts;
        opts.placeholder = "Go to symbol in workspace (type to search)";
        opts.server_filtered = true;
        nav_->ws_syms.clear();
        auto query = [this](const std::string& q) {
            lsp_.cancel(nav_->ws_req);
            nav_->ws_req = lsp_.workspace_symbols(q, [this, q](std::vector<LSPSymbol> syms) {
                lsp_post([this, q, syms = std::move(syms)]() mutable {
                    if (!nav_ || !command_palette_.picking() || command_palette_.query() != q) return;  // stale
                    if (syms.size() > 300) syms.resize(300);
                    std::vector<std::string> items, details;
                    std::vector<uint32_t> icons;
                    for (auto& s : syms) {
                        items.push_back(s.name);
                        fs::path rel = s.path.lexically_relative(project_root_);
                        std::string where = rel.empty() || rel.string().rfind("..", 0) == 0 ? s.path.string() : rel.string();
                        details.push_back((s.container.empty() ? "" : s.container + "  ") + where);
                        icons.push_back(symbol_icon(s.kind));
                    }
                    nav_->ws_syms = std::move(syms);
                    command_palette_.set_pick_items(std::move(items), std::move(details), std::move(icons));
                });
            });
        };
        opts.on_query = query;
        opts.on_pick = [this](int i, const std::string&) {
            if (i < 0 || i >= static_cast<int>(nav_->ws_syms.size())) return;
            LSPSymbol s = nav_->ws_syms[static_cast<size_t>(i)];
            if (Editor* ed = current_editor()) nav_->jump_from(ed);
            open_file(s.path);
            open_at(s.path, s.start.line, lsp_col_to_byte(s.path, s.start.line, s.start.col), 0);
        };
        opts.on_cancel = [this] { lsp_.cancel(nav_->ws_req); };
        command_palette_.pick(std::move(opts));
        query("");
    } else if (cmd == "switch_header_source") {
        Editor* ed = current_editor();
        if (!ed) return true;
        fs::path path = ed->buffer().file_path();
        auto fallback = [this, path] {
            static const std::vector<std::string> headers = {".h", ".hpp", ".hh", ".hxx", ".h++", ".inl"};
            static const std::vector<std::string> sources = {".c", ".cpp", ".cc", ".cxx", ".c++", ".m", ".mm"};
            std::string ext = path.extension().string();
            bool is_header = std::any_of(std::begin(headers), std::end(headers), [&](const std::string& e) { return ext == e; });
            for (const std::string& e : is_header ? sources : headers) {
                fs::path cand = path;
                cand.replace_extension(e);
                std::error_code ec;
                if (fs::exists(cand, ec)) {
                    open_file(cand);
                    return;
                }
            }
            set_status_message("No matching " + std::string(is_header ? "source" : "header") + " file found");
        };
        if (!lsp_.is_ready() || lsp_server_ != "clangd" || !lsp_.is_open(path)) {
            fallback();
            return true;
        }
        uint64_t gen = ++nav_->nav_gen;
        const Editor* from = ed;
        nav_->nav_req = lsp_.switch_source_header(path, [this, gen, from, fallback](std::optional<fs::path> other) {
            lsp_post([this, gen, from, fallback, other] {
                if (!nav_ || gen != nav_->nav_gen || current_editor() != from) return;
                if (other) open_file(*other);
                else fallback();
            });
        });
    } else if (cmd == "toggle_inlay_hints") {
        Editor* ed = current_editor();
        std::string lang = ed ? ed->syntax().language_id() : std::string();
        std::string v = themes_.setting("inlay_hints");
        bool on = v == "true" || v == "on" || ((v.empty() || v == "auto") && lang == "rust");
        themes_.set_setting("inlay_hints", on ? "false" : "true");
        nav_->inlay_valid = false;
        set_status_message(on ? "Inlay hints off" : "Inlay hints on");
    } else if (cmd == "navigate_back" || cmd == "navigate_forward") {
        Editor* ed = current_editor();
        NavLocation cur = ed ? NavLocation{ed->buffer().file_path(), ed->cursors().primary_pos()} : NavLocation{};
        bool back = cmd == "navigate_back";
        nav_->peek.close();
        for (;;) {
            auto to = back ? nav_->history.back(cur) : nav_->history.forward(cur);
            if (!to) {
                set_status_message(back ? "No previous location" : "No next location");
                break;
            }
            std::error_code ec;
            if (!to->path.empty() && !fs::exists(to->path, ec)) {
                nav_->history.forget(to->path);
                continue;
            }
            open_file(to->path);
            Editor* e = current_editor();
            if (!e || e->buffer().file_path() != to->path) break;
            Position p{std::min(to->pos.line, e->buffer().line_count() - 1), to->pos.col};
            p.col = std::min(p.col, e->buffer().line_text(p.line).size());
            e->cursors().set_primary(p);
            focus_editor();
            editor_view_.reveal_cursor();
            // This move is the history's own: don't record it again.
            nav_->last = {to->path, p};
            nav_->last_editor = e;
            nav_->last_version = e->buffer().version();
            nav_->last_valid = true;
            break;
        }
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool App::nav_key(SDL_Keycode key, KeyMod mods) {
    if (!nav_ || command_palette_.is_visible()) return false;
    KeyEvent ke{static_cast<uint32_t>(key), mods, true};
    if (nav_->rename.visible()) return nav_->rename.handle_key(ke);
    bool modifier = key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
                    key == SDLK_LALT || key == SDLK_RALT || key == SDLK_LGUI || key == SDLK_RGUI;
    if (modifier) {
        if (key == SDLK_LCTRL || key == SDLK_RCTRL) {  // pointer already resting on a symbol
            float mx, my;
            SDL_GetMouseState(&mx, &my);
            SDL_Event m{};
            m.type = SDL_EVENT_MOUSE_MOTION;
            m.motion.x = mx;
            m.motion.y = my;
            nav_mouse(m);
        }
        return false;
    }
    if (nav_->hover.visible()) {
        nav_->hover.hide();
        nav_->hover_keyboard = false;
        if (key == SDLK_ESCAPE) return true;
    }
    nav_->dwell_armed = false;
    bool editor_keys = editor_view_.focused() && !editor_view_.completion().visible() && !editor_view_.find_has_focus();
    if (nav_->peek.visible() && editor_keys && nav_->peek.handle_key(ke)) return true;
    return false;
}

void App::nav_key_up(SDL_Keycode key) {
    if (!nav_ || (key != SDLK_LCTRL && key != SDLK_RCTRL)) return;
    if (nav_->link_shown || nav_->link_word.first != SIZE_MAX) {
        nav_->link_word = {SIZE_MAX, SIZE_MAX};
        nav_->link_shown = false;
        lsp_.cancel(nav_->link_req);
        editor_view_.set_link(std::nullopt);
        SDL_SetCursor(SDL_GetDefaultCursor());
        request_redraw();
    }
}

bool App::nav_text(const std::string& text) {
    if (!nav_ || command_palette_.is_visible()) return false;
    if (nav_->rename.visible()) return nav_->rename.handle_text(text);
    return false;
}

bool App::nav_mouse(const SDL_Event& e) {
    if (!nav_ || command_palette_.is_visible() || settings_page_.visible()) return false;
    NavState& n = *nav_;
    Layout layout = compute_layout();
    auto clear_link = [&] {
        if (n.link_shown) SDL_SetCursor(SDL_GetDefaultCursor());
        n.link_shown = false;
        n.link_word = {SIZE_MAX, SIZE_MAX};
        lsp_.cancel(n.link_req);
        editor_view_.set_link(std::nullopt);
    };
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x = e.button.x, y = e.button.y;
        MouseEvent me{x, y, static_cast<int>(e.button.button), true, e.button.clicks >= 2, 0, 0};
        n.dwell_armed = false;
        if (n.rename.visible()) {
            if (n.rename.handle_mouse(me)) return true;
        }
        if (n.hover.visible()) {
            if (n.hover.rect().contains(x, y)) return true;
            n.hover.hide();
            n.hover_keyboard = false;
        }
        if (n.peek.visible() && editor_view_.zone_rect().contains(x, y) && layout.editor.contains(x, y)) {
            focus(&editor_view_);
            n.peek.handle_mouse(me);
            return true;
        }
        bool ctrl = SDL_GetModState() & SDL_KMOD_CTRL;
        if (ctrl && e.button.button == SDL_BUTTON_LEFT && layout.editor.contains(x, y)) {
            Editor* ed = current_editor();
            auto pos = editor_view_.char_at_point(x, y);
            if (!ed || !pos) return false;
            auto w = word_at(ed->buffer().line_text(pos->line), pos->col);
            if (!w) return false;
            focus(&editor_view_);
            bool have = n.link_shown && n.link_word == std::pair{pos->line, w->first} && !n.link_targets.empty();
            auto targets = n.link_targets;
            clear_link();
            ed->cursors().set_primary(*pos);
            if (have && targets.size() == 1) {
                n.jump_from(ed);
                const LSPRangeLoc& l = targets.front();
                open_file(l.path);
                open_at(l.path, l.start.line, lsp_col_to_byte(l.path, l.start.line, l.start.col), 0);
            } else {
                nav_command("goto_definition");
            }
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (n.peek.visible() && editor_view_.zone_rect().contains(e.button.x, e.button.y)) {
            MouseEvent me{e.button.x, e.button.y, static_cast<int>(e.button.button), false, false, 0, 0};
            n.peek.handle_mouse(me);
        }
        return false;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float x = e.wheel.mouse_x, y = e.wheel.mouse_y;
        if (n.hover.visible() && n.hover.rect().contains(x, y)) {
            n.hover.scroll(e.wheel.y * 3.0f);
            request_redraw();
            return true;
        }
        if (n.hover.visible()) n.hover.hide(), n.hover_keyboard = false;
        n.dwell_armed = false;
        if (n.peek.visible() && editor_view_.zone_rect().contains(x, y) && layout.editor.contains(x, y)) {
            MouseEvent me{x, y, 0, false, false, e.wheel.x, e.wheel.y};
            n.peek.handle_mouse(me);
            request_redraw();
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float x = e.motion.x, y = e.motion.y;
        if (e.motion.state & SDL_BUTTON_LMASK) return false;
        Editor* ed = current_editor();
        bool in_editor = ed && layout.editor.contains(x, y) && !welcome_visible_ && !n.rename.visible();
        if (n.hover.visible() && !n.hover_keyboard && !n.hover.keeps_open(x, y)) {
            n.hover.hide();
            request_redraw();
        }
        std::optional<Position> pos = in_editor ? editor_view_.char_at_point(x, y) : std::nullopt;
        std::optional<std::pair<size_t, size_t>> w;
        if (pos) w = word_at(ed->buffer().line_text(pos->line), pos->col);
        // Ctrl+hover: underline the symbol once the server confirms it has a definition.
        bool ctrl = SDL_GetModState() & SDL_KMOD_CTRL;
        if (ctrl && w && lsp_.is_ready() && lsp_.is_open(ed->buffer().file_path())) {
            std::pair<size_t, size_t> key{pos->line, w->first};
            if (key != n.link_word) {
                clear_link();
                n.link_word = key;
                n.link_path = ed->buffer().file_path();
                uint64_t gen = ++n.link_gen;
                uint64_t version = ed->buffer().version();
                Position at = *pos;
                std::pair<Position, Position> range{{pos->line, w->first}, {pos->line, w->second}};
                const Editor* from = ed;
                std::string line = ed->buffer().line_text(at.line);
                Position lsp_at = at;
                if (!lsp_.utf8_positions()) lsp_at.col = utf8::utf16_col(line, at.col);
                n.link_req = lsp_.locations("textDocument/definition", n.link_path, lsp_at,
                    [this, gen, version, range, from](std::vector<LSPRangeLoc> locs) {
                        lsp_post([this, gen, version, range, from, locs = std::move(locs)] {
                            Editor* ed = current_editor();
                            if (!nav_ || gen != nav_->link_gen || ed != from || ed->buffer().version() != version) return;
                            if (!(SDL_GetModState() & SDL_KMOD_CTRL) || locs.empty()) return;
                            nav_->link_targets = locs;
                            nav_->link_shown = true;
                            editor_view_.set_link(range);
                            if (!nav_->hand) nav_->hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
                            SDL_SetCursor(nav_->hand);
                        });
                    });
            }
        } else if (n.link_word.first != SIZE_MAX) {
            clear_link();
            request_redraw();
        }
        // Hover: arm the dwell timer when the pointer settles on a new word.
        if (!w || ctrl) {
            if (!n.hover_keyboard) n.dwell_armed = false;
            n.dwell_word = {SIZE_MAX, SIZE_MAX};
            return false;
        }
        std::pair<size_t, size_t> key{pos->line, w->first};
        if (n.hover.visible() && key == n.dwell_word) return false;  // still on the shown word
        if (key != n.dwell_word || !n.dwell_armed) {
            n.dwell_word = key;
            n.dwell_pos = *pos;
            n.dwell_path = ed->buffer().file_path();
            n.dwell_at = Clock::now();
            n.dwell_armed = true;
            n.hover_keyboard = false;
            wake_in(static_cast<int>(HOVER_DELAY.count()) + 20);
        }
        return false;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void App::nav_update() {
    if (!nav_) return;
    NavState& n = *nav_;
    Editor* ed = current_editor();

    // Popups belong to the editor they were opened on.
    if (n.peek.visible() && n.peek.host() != ed) n.peek.close();
    if (!n.peek.visible()) editor_view_.set_zone(std::nullopt, 0);
    else editor_view_.set_zone(n.peek.anchor_line(), nav_peek_height());  // follows window resizes
    if (n.rename.visible() && (!ed || ed->buffer().file_path().empty())) n.rename.close();
    editor_view2_.set_zone(std::nullopt, 0);
    editor_view2_.set_link(std::nullopt);
    editor_view2_.set_inlay_hints(nullptr);

    // Hover after the dwell delay (or right away for Ctrl+K Ctrl+I).
    if (n.dwell_armed && ed && Clock::now() - n.dwell_at >= HOVER_DELAY && ed->buffer().file_path() == n.dwell_path &&
        !command_palette_.is_visible() && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) {
        n.dwell_armed = false;
        fs::path path = n.dwell_path;
        bool keyboard = n.hover_keyboard;
        if (lsp_.is_ready() && lsp_.is_open(path)) {
            lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);
            lsp_update();
            Position at = n.dwell_pos;
            std::string line = ed->buffer().line_text(at.line);
            auto w = word_at(line, std::min(at.col, line.size()));
            Position lsp_at = at;
            if (!lsp_.utf8_positions()) lsp_at.col = utf8::utf16_col(line, at.col);
            lsp_.cancel(n.hover_req);
            uint64_t gen = ++n.hover_gen;
            uint64_t version = ed->buffer().version();
            const Editor* from = ed;
            size_t wa = w ? w->first : at.col, wb = w ? w->second : at.col + 1;
            n.hover_req = lsp_.hover_markdown(path, lsp_at, [this, gen, version, from, at, wa, wb, keyboard](std::optional<LSPHover> h) {
                lsp_post([this, gen, version, from, at, wa, wb, keyboard, h = std::move(h)] {
                    Editor* ed = current_editor();
                    if (!nav_ || gen != nav_->hover_gen || ed != from || ed->buffer().version() != version) return;
                    if (!h) {
                        if (keyboard) set_status_message("No hover information");
                        return;
                    }
                    Rect a = editor_view_.char_rect({at.line, wa});
                    Rect b = editor_view_.char_rect({at.line, wb});
                    a.w = std::max(a.w, b.x - a.x);
                    nav_->hover.show(h->contents, a, ed->syntax().language_id());
                    nav_->hover_keyboard = keyboard;
                });
            });
        } else if (keyboard) {
            set_status_message("No language server for this file");
        }
    }
    if (n.hover.visible() && !ed) n.hover.hide();

    // History: remember where big jumps leave from (definition, references, symbol
    // picks, search hits, tab switches, Ctrl+Home...), but not edits.
    if (ed && !command_palette_.is_visible() && !n.rename.visible() && !ed->buffer().file_path().empty()) {
        NavLocation cur{ed->buffer().file_path(), ed->cursors().primary_pos()};
        uint64_t v = ed->buffer().version();
        if (n.last_valid) {
            size_t d = cur.pos.line > n.last.pos.line ? cur.pos.line - n.last.pos.line : n.last.pos.line - cur.pos.line;
            bool jumped = cur.path != n.last.path || (ed == n.last_editor && v == n.last_version && d > 10);
            if (jumped) n.history.record(n.last);
        }
        n.last = cur;
        n.last_editor = ed;
        n.last_version = v;
        n.last_valid = true;
    }

    // Inlay hints for the visible range, refreshed after edits (debounced).
    auto inlays_off = [&] {
        editor_view_.set_inlay_hints(nullptr);
        n.inlay_valid = false;
    };
    if (!ed) return inlays_off();
    fs::path path = ed->buffer().file_path();
    std::string lang = ed->syntax().language_id();
    std::string setting = themes_.setting("inlay_hints");
    bool enabled = setting == "true" || setting == "on" || ((setting.empty() || setting == "auto") && lang == "rust");
    if (!enabled || path.empty() || !lsp_.is_ready() || !lsp_.is_open(path) || !lsp_.has_provider("inlayHintProvider"))
        return inlays_off();
    if (n.inlay_path != path || n.inlay_editor != ed) {
        n.inlay_path = path;
        n.inlay_editor = ed;
        n.inlay_hints.clear();
        n.inlay_shown.clear();
        n.inlay_valid = false;
        n.inlay_shown_version = UINT64_MAX;
        lsp_.cancel(n.inlay_req);
        n.inlay_req = 0;
    }
    uint64_t v = ed->buffer().version();
    auto now = Clock::now();
    if (v != n.inlay_seen_version) {
        n.inlay_seen_version = v;
        n.inlay_changed_at = now;
        wake_in(static_cast<int>(INLAY_DEBOUNCE.count()) + 20);
    }
    float lh = std::max(1.0f, editor_view_.line_height());
    size_t first = static_cast<size_t>(std::max(0.0f, ed->scroll_y()) / lh);
    size_t last = std::min(first + static_cast<size_t>(compute_layout().editor.h / lh) + 2, ed->buffer().line_count() - 1);
    first = std::min(first, last);
    bool covered = n.inlay_valid && first >= n.inlay_first && last <= n.inlay_last;
    bool fresh = n.inlay_valid && n.inlay_version == v && n.inlay_epoch == lsp_.refresh_epoch();
    bool settled = now - n.inlay_changed_at >= INLAY_DEBOUNCE && now >= n.inlay_retry_at;
    bool in_flight = n.inlay_req != 0 && n.inlay_req_version == v;
    if ((!fresh || !covered) && settled && !in_flight) {
        lsp_docs_[path.string()].changed = now - std::chrono::seconds(1);  // the server must see the text first
        lsp_update();
        lsp_.cancel(n.inlay_req);
        size_t a = first > 60 ? first - 60 : 0, b = std::min(last + 60, ed->buffer().line_count() - 1);
        Position range_end{b, ed->buffer().line_length(b)};  // servers reject a line past the end
        uint64_t epoch = lsp_.refresh_epoch();
        const Editor* from = ed;
        n.inlay_req_version = v;
        n.inlay_req = lsp_.inlay_hints(path, {a, 0}, range_end, [this, from, v, a, b, epoch](std::vector<LSPInlayHint> hints, bool ok) {
            lsp_post([this, from, v, a, b, epoch, ok, hints = std::move(hints)]() mutable {
                if (!nav_) return;
                NavState& n = *nav_;
                if (n.inlay_req_version == v) n.inlay_req = 0;
                Editor* ed = current_editor();
                if (ed != from || n.inlay_editor != from || ed->buffer().version() != v) return;  // stale: edited since
                if (!ok) {  // e.g. "content modified" while indexing: retry later
                    n.inlay_retry_at = Clock::now() + std::chrono::seconds(2);
                    wake_in(2100);
                    return;
                }
                bool utf16 = !lsp_.utf8_positions();
                for (auto& h : hints) {
                    if (h.pos.line >= ed->buffer().line_count()) continue;
                    std::string line = ed->buffer().line_text(h.pos.line);
                    if (utf16) h.pos.col = utf8::byte_col_from_utf16(line, h.pos.col);
                    h.pos.col = std::min(h.pos.col, line.size());
                    while (!h.label.empty() && h.label.back() == ' ') h.label.pop_back(), h.pad_right = true;
                    while (!h.label.empty() && h.label.front() == ' ') h.label.erase(0, 1), h.pad_left = true;
                    h.cells = static_cast<int>(utf8::display_col(h.label, h.label.size())) + (h.pad_left ? 1 : 0) +
                              (h.pad_right ? 1 : 0);
                }
                std::stable_sort(hints.begin(), hints.end(), [](const LSPInlayHint& x, const LSPInlayHint& y) { return x.pos < y.pos; });
                n.inlay_hints = std::move(hints);
                n.inlay_version = v;
                n.inlay_lines = ed->buffer().line_count();
                n.inlay_first = a;
                n.inlay_last = b;
                n.inlay_epoch = epoch;
                n.inlay_valid = true;
                n.inlay_shown_version = UINT64_MAX;
            });
        });
    }
    if (n.inlay_shown_version != v) {
        n.inlay_shown_version = v;
        n.inlay_shown.clear();
        if (n.inlay_version == v) {
            n.inlay_shown = n.inlay_hints;
        } else if (!n.inlay_hints.empty()) {
            // Edited since: keep hints above the first change, drop the changed line,
            // shift the rest by the change in line count until fresh hints arrive.
            size_t off = ed->buffer().earliest_change_since(n.inlay_version);
            size_t change_line = off == SIZE_MAX ? SIZE_MAX : ed->buffer().position_of(std::min(off, ed->buffer().length())).line;
            long long delta = static_cast<long long>(ed->buffer().line_count()) - static_cast<long long>(n.inlay_lines);
            for (auto h : n.inlay_hints) {
                if (h.pos.line < change_line) {
                    n.inlay_shown.push_back(h);
                } else if (h.pos.line > change_line) {
                    long long l = static_cast<long long>(h.pos.line) + delta;
                    if (l <= static_cast<long long>(change_line)) continue;
                    h.pos.line = static_cast<size_t>(l);
                    if (h.pos.line >= ed->buffer().line_count()) continue;
                    h.pos.col = std::min(h.pos.col, ed->buffer().line_length(h.pos.line));
                    n.inlay_shown.push_back(h);
                }
            }
        }
    }
    editor_view_.set_inlay_hints(&n.inlay_shown);
}

// ---------------------------------------------------------------------------
// Rendering and probes
// ---------------------------------------------------------------------------

float App::nav_peek_height() {
    // Twelve lines of preview, but leave a few editor lines visible in small windows.
    float lh = std::max(1.0f, editor_view_.line_height());
    float avail = compute_layout().editor.h - 3.0f * lh;
    return std::max(std::min(nav_->peek.height(lh), avail), std::min(nav_->peek.height(lh), 7.0f * lh));
}

void App::nav_render_editor(RenderContext& ctx) {
    if (!nav_ || !nav_->peek.visible()) return;
    Rect zone = editor_view_.zone_rect();
    if (zone.w <= 0 || zone.h <= 0) return;
    Rect ed = compute_layout().editor;
    float top = std::max(zone.y, ed.y), bottom = std::min(zone.y + zone.h, ed.y + ed.h);
    if (bottom <= top) return;
    renderer_.push_clip({zone.x, top, zone.w, bottom - top});
    nav_->peek.render(ctx, zone);
    renderer_.pop_clip();
}

void App::nav_render_popups(RenderContext& ctx, Rect window) {
    if (!nav_) return;
    Rect ed = compute_layout().editor;
    if (nav_->hover.visible()) nav_->hover.render(ctx, ed.w > 0 ? ed : window);
    if (nav_->rename.visible()) nav_->rename.render(ctx, ed.w > 0 ? ed : window);
}

void App::nav_probe() {
    if (!nav_) return;
    NavState& n = *nav_;
    std::string hover = n.hover.visible() ? n.hover.text().substr(0, 160) : std::string();
    for (auto& c : hover)
        if (c == '\n') c = '|';
    std::fprintf(stderr, "  nav: lsp=%s ready=%d progress=\"%s\" hover=%d blocks=%zu text=\"%s\"\n",
                 lsp_server_.c_str(), lsp_.is_ready(), lsp_.progress().c_str(), n.hover.visible(), n.hover.block_count(),
                 hover.c_str());
    std::string peek;
    if (n.peek.visible())
        if (const PeekView::Item* it = n.peek.selected_item())
            peek = it->path.filename().string() + ":" + std::to_string(it->start.line + 1) + ":" + std::to_string(it->start.col + 1);
    std::fprintf(stderr, "  nav: peek=%d items=%zu selected=%s rename=%d rename_text=\"%s\" link=%d inlays=%zu "
                 "history=%zu/%zu palette_items=%s\n",
                 n.peek.visible(), n.peek.count(), peek.c_str(), n.rename.visible(), n.rename.text().c_str(), n.link_shown,
                 n.inlay_shown.size(), n.history.back_size(), n.history.forward_size(),
                 command_palette_.picking() ? "open" : "-");
    if (!n.inlay_shown.empty()) {
        std::string s;
        for (size_t i = 0; i < n.inlay_shown.size() && i < 8; i++)
            s += " " + std::to_string(n.inlay_shown[i].pos.line + 1) + ":" + n.inlay_shown[i].label;
        std::fprintf(stderr, "  nav: inlay_sample=%s\n", s.c_str());
    }
}

} // namespace oh
