// Build tasks, the Problems panel, Ctrl+click file locations in terminals, quick fixes
// (Ctrl+.) and LSP formatting (Shift+Alt+F, format on save).
#include "app.h"
#include "editor/utf8.h"
#include "tasks/json.h"
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace oh {

namespace {

// Byte column of a character (code point) column.
size_t byte_of_char_col(const std::string& line, size_t chars) {
    size_t i = 0;
    for (size_t c = 0; c < chars && i < line.size(); c++) i = utf8::next_boundary(line, i);
    return std::min(i, line.size());
}

size_t to_byte_col(const std::string& line, size_t col, int unit) {
    if (unit == 1) return utf8::byte_col_from_utf16(line, col);  // App::EDIT_UTF16
    if (unit == 2) return byte_of_char_col(line, col);  // App::EDIT_CHARS
    return std::min(col, line.size());
}

// End of the identifier (or at least one character) at `col`: squiggles for build
// problems that only give a point.
size_t token_end(const std::string& line, size_t col) {
    size_t e = col;
    auto ident = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
    while (e < line.size() && ident(static_cast<unsigned char>(line[e]))) e++;
    if (e == col) e = utf8::next_boundary(line, col);
    return std::max(e, col + 1);
}

Editor* open_editor(TabBar& a, TabBar& b, const std::string& path) {
    for (TabBar* bar : {&a, &b})
        for (auto& t : bar->tabs())
            if (t.editor && t.editor->buffer().file_path() == path) return t.editor.get();
    return nullptr;
}

// Lines of a file: the open editor's buffer, else the file on disk (read once).
struct FileLines {
    Editor* ed = nullptr;
    std::vector<std::string> disk;
    bool loaded = false;
    std::string path;
    std::string line(size_t n) {
        if (ed) return n < ed->buffer().line_count() ? ed->buffer().line_text(n) : std::string();
        if (!loaded) {
            loaded = true;
            std::ifstream in(path, std::ios::binary);
            for (std::string l; std::getline(in, l);) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                disk.push_back(std::move(l));
            }
        }
        return n < disk.size() ? disk[n] : std::string();
    }
};

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string path_from_uri(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return uri;
    std::string p = percent_decode(uri.substr(7));
#ifdef _WIN32
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1);  // /C:/x -> C:/x
#endif
    return p;
}

void push_text_edit(const std::string& file, const json::Value& e, std::vector<ProblemEdit>& out) {
    const json::Value& r = e["range"];
    ProblemEdit pe;
    pe.file = file;
    pe.line = static_cast<size_t>(r["start"]["line"].integer());
    pe.col = static_cast<size_t>(r["start"]["character"].integer());
    pe.end_line = static_cast<size_t>(r["end"]["line"].integer());
    pe.end_col = static_cast<size_t>(r["end"]["character"].integer());
    pe.text = e["newText"].str();
    out.push_back(std::move(pe));
}

// WorkspaceEdit (changes or documentChanges) -> edits. File create/rename/delete
// operations are not supported; *skipped counts them.
void collect_workspace_edit(const json::Value& we, std::vector<ProblemEdit>& out, int* skipped) {
    for (auto& [uri, edits] : we["changes"].members())
        for (auto& e : edits.items()) push_text_edit(path_from_uri(uri), e, out);
    for (auto& dc : we["documentChanges"].items()) {
        if (!dc["edits"].is_array()) {
            if (skipped) (*skipped)++;
            continue;
        }
        std::string file = path_from_uri(dc["textDocument"]["uri"].str());
        for (auto& e : dc["edits"].items()) push_text_edit(file, e, out);
    }
}

const char* group_word(const std::string& group) {
    return group == "build" ? "build" : group == "test" ? "test" : group == "run" ? "run" : "";
}

} // namespace

// ---------------------------------------------------------------------------
// Setup and commands
// ---------------------------------------------------------------------------

void App::setup_tasks() {
    format_on_save_ = themes_.setting("format_on_save") == "true";
    problems_.set_root(project_root_);
    problems_.set_on_open([this](const std::string& file, size_t line, size_t col) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            set_status_message("File not found: " + file, 6.0f);
            return;
        }
        open_at(file, line, col, 0);
    });
    terminal_.set_link_handler([this](const TextLink& l, bool open) { return open_link(l, terminal_.cwd(), open); });
    task_term_.set_link_handler([this](const TextLink& l, bool open) { return open_link(l, task_term_.cwd(), open); });
    // Code actions that run a server command come back as workspace/applyEdit requests.
    lsp_.set_on_server_request([this](const std::string& method, const std::string& msg) -> std::string {
        if (method != "workspace/applyEdit") return "";
        json::Value v;
        if (!json::parse(msg, v)) return "{\"applied\":false}";
        std::vector<ProblemEdit> edits;
        collect_workspace_edit(v["params"]["edit"], edits, nullptr);
        lsp_post([this, edits = std::move(edits)] {
            std::string summary;
            apply_edits(edits, lsp_edit_unit(), &summary);
            if (!summary.empty()) set_status_message(summary);
        });
        return "{\"applied\":true}";
    });

    auto add = [this](const std::string& name, const std::string& label, const std::string& keys) {
        commands_.push_back({name, label, [this, name]() { handle_command(name); }, keys});
    };
    add("run_build_task", "Tasks: Run Build Task", "Ctrl+Shift+B");
    add("run_test_task", "Tasks: Run Test Task", "");
    add("run_task", "Tasks: Run Task...", "");
    add("run_program", "Run Without Debugging (run task)", "Ctrl+F5");
    add("task_rerun", "Tasks: Rerun Last Task", "");
    add("task_terminate", "Tasks: Terminate Task", "Ctrl+Break");
    add("tasks_configure", "Tasks: Configure Tasks (.overheat/tasks.toml)", "");
    add("show_task_output", "Tasks: Show Task Output", "");
    add("prev_problem", "Go to Previous Problem (Error, Warning)", "Shift+F8");
    add("code_action", "Quick Fix... (Code Actions)", "Ctrl+.");
    add("format_document", "Format Document", "Shift+Alt+F");
    add("toggle_format_on_save", "Preferences: Toggle Format on Save", "");
}

bool App::tasks_command(const std::string& cmd) {
    if (cmd == "run_build_task") {
        run_default_task("build");
    } else if (cmd == "run_test_task") {
        run_default_task("test");
    } else if (cmd == "run_program") {
        run_default_task("run");
    } else if (cmd == "run_task") {
        pick_task("");
    } else if (cmd == "task_rerun") {
        if (last_task_) run_task(*last_task_);
        else pick_task("");
    } else if (cmd == "task_terminate") {
        if (tasks_.running()) {
            tasks_.cancel();
            set_status_message("Terminating " + tasks_.step_label() + "...");
        } else {
            set_status_message("No task is running");
        }
    } else if (cmd == "show_task_output") {
        show_bottom(BottomView::Task, true);
    } else if (cmd == "tasks_configure") {
        fs::path file = tasks_file(project_root_);
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            fs::create_directories(file.parent_path(), ec);
            std::ofstream out(file);
            out << "# Overheat build tasks: Tasks: Run Task... (Ctrl+Shift+B runs the default build task).\n"
                   "# Detected tasks (CMake, Make, Cargo) and .vscode/tasks.json are listed too; a task\n"
                   "# here with the same label replaces the detected one.\n"
                   "#\n"
                   "# [[task]]\n"
                   "# label = \"build\"\n"
                   "# command = \"make -j8\"            # a shell command line, or:\n"
                   "# # program = \"cargo\"              # a program with arguments\n"
                   "# # args = [\"build\", \"--release\"]\n"
                   "# cwd = \"${workspaceFolder}\"\n"
                   "# env = { CFLAGS = \"-O2\" }\n"
                   "# group = \"build\"                  # build | test | run\n"
                   "# default = true\n"
                   "# matcher = [\"gcc\"]                # gcc msvc cmake rustc cargo-json generic\n"
                   "# depends_on = [\"configure\"]\n";
            for (auto& t : detect_tasks(project_root_)) {
                out << "\n# [[task]]\n# label = \"" << t.label << "\"\n# program = \"" << t.command << "\"\n# args = [";
                for (size_t i = 0; i < t.args.size(); i++) out << (i ? ", " : "") << json::quote(t.args[i]);
                out << "]\n";
                if (!t.group.empty()) out << "# group = \"" << t.group << "\"\n";
            }
        }
        open_file(file);
    } else if (cmd == "prev_problem") {
        goto_problem(-1);
    } else if (cmd == "code_action") {
        code_actions();
    } else if (cmd == "format_document") {
        format_document({});
    } else if (cmd == "toggle_format_on_save") {
        format_on_save_ = !format_on_save_;
        themes_.set_setting("format_on_save", format_on_save_ ? "true" : "false");
        set_status_message(std::string("Format on save ") + (format_on_save_ ? "on" : "off"));
    } else if (cmd == "save" && format_on_save_ && current_editor() && !pending_format_ &&
               lsp_.is_running() && lsp_.is_open(current_editor()->buffer().file_path())) {
        format_document([this] { save_current(); });
    } else if (cmd == "toggle_terminal" && terminal_visible_ && bottom_view_ != BottomView::Terminal) {
        show_bottom(BottomView::Terminal, true);  // Ctrl+`: from Problems / task output to the shell
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Running tasks
// ---------------------------------------------------------------------------

TaskContext App::task_context() {
    TaskContext ctx;
    ctx.workspace = project_root_;
    if (Editor* ed = current_editor()) {
        ctx.file = ed->buffer().file_path();
        ctx.line = ed->cursors().primary_pos().line + 1;
    }
    return ctx;
}

void App::pick_task(const std::string& group) {
    std::vector<std::string> errors;
    auto tasks = load_tasks(project_root_, &errors);
    if (!errors.empty()) set_status_message(errors.front(), 10.0f);
    std::vector<TaskDef> shown;
    for (auto& t : tasks)
        if (group.empty() || t.group == group) shown.push_back(t);
    std::vector<std::string> items, details;
    for (auto& t : shown) {
        items.push_back(t.label);
        std::string d = t.source;
        if (!t.group.empty()) d += std::string(", ") + group_word(t.group) + (t.is_default ? " (default)" : "");
        if (!t.detail.empty()) d += "  " + t.detail;
        details.push_back(d);
    }
    items.push_back("Configure Tasks...");
    details.push_back(".overheat/tasks.toml");
    CommandPalette::PickOptions opts;
    opts.placeholder = shown.empty() ? "No tasks detected: configure one" : "Select the task to run";
    opts.items = std::move(items);
    opts.details = std::move(details);
    opts.on_pick = [this, shown](int i, const std::string&) {
        if (i < 0) return;
        if (static_cast<size_t>(i) == shown.size()) handle_command("tasks_configure");
        else run_task(shown[static_cast<size_t>(i)]);
    };
    command_palette_.pick(std::move(opts));
}

void App::run_default_task(const std::string& group) {
    if (vfs_->is_remote()) {
        set_status_message("Tasks run on this computer only; Remote-SSH builds are not supported yet", 8.0f);
        return;
    }
    std::vector<std::string> errors;
    auto tasks = load_tasks(project_root_, &errors);
    if (!errors.empty()) set_status_message(errors.front(), 10.0f);
    if (const TaskDef* t = default_task(tasks, group)) {
        run_task(*t);
        return;
    }
    set_status_message(std::string("No ") + group_word(group) + " task found: pick one, or add one with group = \"" +
                           group + "\" in .overheat/tasks.toml",
                       8.0f);
    pick_task("");
}

void App::run_task(const TaskDef& t) {
    if (vfs_->is_remote()) {
        set_status_message("Tasks run on this computer only; Remote-SSH builds are not supported yet", 8.0f);
        return;
    }
    // Dependencies first (depth first, each once), then the task itself.
    auto all = load_tasks(project_root_);
    std::vector<TaskDef> chain;
    std::set<std::string> seen;
    std::function<void(const TaskDef&, int)> visit = [&](const TaskDef& d, int depth) {
        if (depth > 16 || !seen.insert(d.label).second) return;
        for (auto& dep : d.depends_on) {
            if (const TaskDef* dt = find_task(all, dep)) visit(*dt, depth + 1);
            else set_status_message("Task \"" + d.label + "\" depends on unknown task \"" + dep + "\"", 8.0f);
        }
        chain.push_back(d);
    };
    visit(t, 0);
    // Like VS Code's task.saveBeforeRun: build what is on screen.
    for (TabBar* bar : {&tab_bar_, &tab_bar2_})
        for (auto& tab : bar->tabs()) {
            Editor* ed = tab.editor.get();
            if (!ed || !ed->is_modified() || ed->buffer().file_path().empty() || ed->read_only()) continue;
            if (vfs_->write(ed->buffer().file_path(), ed->buffer().text())) {
                ed->buffer().set_unmodified();
                std::error_code ec;
                if (auto mt = fs::last_write_time(ed->buffer().file_path(), ec); !ec)
                    disk_mtimes_[ed->buffer().file_path().string()] = mt;
                if (lsp_.is_open(ed->buffer().file_path())) lsp_.did_save(ed->buffer().file_path());
            }
        }
    last_task_ = t;
    task_problems_.clear();
    task_problems_version_ = UINT64_MAX;
    problems_dirty_++;
    status_bar_.set_task_result("", true);
    task_term_used_ = true;
    show_bottom(BottomView::Task, false);
    tasks_.start(std::move(chain), task_context(), task_term_);
}

// ---------------------------------------------------------------------------
// Bottom panel: Problems / Terminal / Task tabs
// ---------------------------------------------------------------------------

Panel& App::bottom_panel() {
    switch (bottom_view_) {
        case BottomView::Problems: return problems_;
        case BottomView::Task: return task_term_;
        case BottomView::DebugConsole: return debug_console_;
        default: return terminal_;
    }
}

Terminal* App::bottom_terminal() {
    if (bottom_view_ == BottomView::Problems || bottom_view_ == BottomView::DebugConsole) return nullptr;
    return bottom_view_ == BottomView::Task ? &task_term_ : &terminal_;
}

void App::show_bottom(BottomView v, bool take_focus) {
    bool had_focus = terminal_.focused() || task_term_.focused() || problems_.focused() || debug_console_.focused();
    bottom_view_ = v;
    if (v == BottomView::Task) task_term_used_ = true;
    terminal_visible_ = true;
    if (take_focus || had_focus) focus(&bottom_panel());
}

void App::show_problems() { show_bottom(BottomView::Problems, true); }

void App::goto_next_problem() { goto_problem(1); }

void App::render_panel_header(RenderContext& ctx, Rect r) {
    ctx.r->add_rect(r, theme_.terminal_bg);
    ctx.r->add_rect({r.x, r.y, r.w, 1.0f}, theme_.border);
    float cw = font_.char_width();
    float base = r.y + (r.h - font_.line_height()) * 0.5f + font_.ascender();
    panel_tabs_.clear();
    float x = r.x + 14.0f;
    auto tab = [&](BottomView v, const std::string& title, const std::string& extra, uint32_t icon, Color icon_color) {
        bool active = bottom_view_ == v;
        bool focused = active && bottom_panel().focused();
        float x0 = x;
        x = ctx.r->add_text(x, base, title, active || focused ? theme_.fg : theme_.fg_muted, font_);
        if (icon) {
            ctx.r->add_char(x + cw, base, icon, icon_color, font_);
            x += 2.0f * cw;
        }
        if (!extra.empty()) x = ctx.r->add_text(x + cw, base, extra, theme_.fg_muted, font_);
        Rect hit{x0 - 6.0f, r.y, x - x0 + 12.0f, r.h};
        if (active) ctx.r->add_rect({x0 - 4.0f, r.y + r.h - 2.0f, x - x0 + 8.0f, 2.0f}, theme_.accent);
        else if (hit.contains(ctx.mouse_x, ctx.mouse_y)) ctx.r->add_rect({x0 - 4.0f, r.y + r.h - 2.0f, x - x0 + 8.0f, 2.0f}, theme_.border);
        panel_tabs_.push_back({hit, v});
        x += 3.0f * cw;
    };
    // Tabs stop short of the buttons on the right (narrow windows fade them out).
    float tabs_end = r.x + r.w - (bottom_view_ == BottomView::Problems ? 40.0f : 68.0f);
    ctx.r->push_clip({r.x, r.y, tabs_end - r.x, r.h});
    ctx.r->set_fade(tabs_end - 24.0f, tabs_end);
    int n_problems = problems_.count(1) + problems_.count(2);
    tab(BottomView::Problems, "PROBLEMS", n_problems ? std::to_string(n_problems) : "", 0, theme_.fg);
    std::string shell = remote_active_ ? "ssh: " + remote_target_.alias : fs::path(local_shell().front()).stem().string();
    tab(BottomView::Terminal, "TERMINAL", shell, 0, theme_.fg);
    if (task_term_used_) {
        uint32_t icon = 0;
        Color c = theme_.fg_muted;
        if (tasks_.running()) icon = 0xEB19, c = theme_.accent;  // loading
        else if (tasks_.state() == TaskRunner::State::Succeeded) icon = 0xEAB2, c = theme_.git_added;
        else if (tasks_.state() == TaskRunner::State::Failed) icon = 0xEA87, c = theme_.error;
        tab(BottomView::Task, "TASK", tasks_.label(), icon, c);
    }
    tab(BottomView::DebugConsole, "DEBUG CONSOLE", "", 0, theme_.fg);
    ctx.r->set_fade(0, 0);
    ctx.r->pop_clip();
    // Right: restart (shell) / stop or rerun (task), and close.
    term_close_btn_ = {r.x + r.w - 32.0f, r.y + 2.0f, 24.0f, r.h - 4.0f};
    term_restart_btn_ = {r.x + r.w - 60.0f, r.y + 2.0f, 24.0f, r.h - 4.0f};
    uint32_t second = bottom_view_ == BottomView::Task           ? (tasks_.running() ? 0xEAD7u : 0xEB37u)
                      : bottom_view_ == BottomView::DebugConsole ? 0xEA81u  // clear console
                                                                 : 0xEB37u;
    std::vector<std::pair<Rect, uint32_t>> buttons{{term_close_btn_, 0xEA76u}};
    if (bottom_view_ != BottomView::Problems) buttons.push_back({term_restart_btn_, second});
    for (auto [btn, icon] : buttons) {
        bool hover = btn.contains(ctx.mouse_x, ctx.mouse_y);
        if (hover) ctx.r->add_rect(btn, theme_.surface_hover, 4.0f);
        ctx.r->add_char(btn.x + (btn.w - cw) * 0.5f, base, icon, hover ? theme_.fg : theme_.fg_muted, font_);
    }
}

bool App::panel_header_click(float mx, float my) {
    if (term_close_btn_.contains(mx, my)) {
        terminal_visible_ = false;
        if (bottom_panel().focused()) focus_editor();
        return true;
    }
    if (bottom_view_ != BottomView::Problems && term_restart_btn_.contains(mx, my)) {
        if (bottom_view_ == BottomView::Terminal) restart_terminal();
        else if (bottom_view_ == BottomView::DebugConsole) debugger_.clear_console();
        else if (tasks_.running()) tasks_.cancel();
        else if (last_task_) run_task(*last_task_);
        return true;
    }
    for (auto& [rect, v] : panel_tabs_)
        if (rect.contains(mx, my)) {
            show_bottom(v, true);
            return true;
        }
    focus(&bottom_panel());
    return true;
}

// ---------------------------------------------------------------------------
// Links and paths
// ---------------------------------------------------------------------------

fs::path App::resolve_path(const std::string& p, const fs::path& cwd) const {
    if (p.empty()) return {};
    std::error_code ec;
    fs::path path = p;
    if (p.size() > 1 && p[0] == '~' && (p[1] == '/' || p[1] == '\\')) path = platform::home_dir() / p.substr(2);
    if (path.is_absolute()) return fs::is_regular_file(path, ec) ? path.lexically_normal() : fs::path();
    std::vector<fs::path> bases;
    if (!cwd.empty()) bases.push_back(cwd);
    bases.push_back(project_root_);
    for (auto& r : extra_roots_) bases.push_back(r);
    for (auto& b : bases)
        if (fs::is_regular_file(b / path, ec)) return (b / path).lexically_normal();
    return {};
}

bool App::open_link(const TextLink& link, const fs::path& cwd, bool open) {
    if (vfs_->is_remote()) return false;  // remote paths: not resolvable on this machine
    fs::path p = resolve_path(link.path, cwd);
    if (p.empty()) return false;
    if (!open) return true;
    open_file(p);
    Editor* ed = current_editor();
    if (!ed || ed->buffer().file_path() != p) return true;
    size_t line = link.line > 0 ? std::min(link.line - 1, ed->buffer().line_count() - 1) : 0;
    size_t col = link.col > 0 ? byte_of_char_col(ed->buffer().line_text(line), link.col - 1) : 0;
    if (link.line > 0) open_at(p, line, col, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Problems: build problems + LSP diagnostics
// ---------------------------------------------------------------------------

void App::tasks_update() {
    task_term_.update(0.0f);
    bool changed = tasks_.poll();
    if (tasks_.problems_version() != task_problems_version_) {
        task_problems_version_ = tasks_.problems_version();
        // Absolute paths, byte columns (compilers count characters), real files only.
        std::map<std::string, FileLines> lines;
        task_problems_.clear();
        std::error_code ec;
        for (Problem p : tasks_.problems()) {
            if (!p.file.empty()) {
                if (!fs::is_regular_file(p.file, ec)) {
                    fs::path alt = resolve_path(fs::path(p.file).lexically_relative(project_root_).string(), project_root_);
                    if (alt.empty() && p.source == "generic") continue;  // the fallback matcher guessed wrong
                    if (!alt.empty()) p.file = alt.string();
                }
                auto& fl = lines[p.file];
                if (fl.path.empty()) fl.path = p.file, fl.ed = open_editor(tab_bar_, tab_bar2_, p.file);
                std::string l0 = fl.line(p.line);
                p.col = byte_of_char_col(l0, p.col);
                std::string l1 = p.end_line == p.line ? l0 : fl.line(p.end_line);
                p.end_col = byte_of_char_col(l1, p.end_col);
            }
            for (auto& fx : p.fixes)
                for (auto& e : fx.edits) {
                    auto& fl = lines[e.file];
                    if (fl.path.empty()) fl.path = e.file, fl.ed = open_editor(tab_bar_, tab_bar2_, e.file);
                    e.col = byte_of_char_col(fl.line(e.line), e.col);
                    e.end_col = byte_of_char_col(fl.line(e.end_line), e.end_col);
                }
            task_problems_.push_back(std::move(p));
        }
        problems_dirty_++;
    }
    if (changed && !tasks_.running()) {
        int errors = 0, warnings = 0;
        for (auto& p : task_problems_) (p.severity == 1 ? errors : warnings) += p.severity <= 2 ? 1 : 0;
        bool ok = tasks_.state() == TaskRunner::State::Succeeded;
        std::string what = tasks_.label();
        std::string counts = std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
                             (warnings == 1 ? " warning" : " warnings");
        if (tasks_.state() == TaskRunner::State::Cancelled) {
            set_status_message(what + ": cancelled");
            status_bar_.set_task_result(what + ": cancelled", false);
        } else {
            set_status_message(what + (ok ? " succeeded" : " failed (exit code " + std::to_string(tasks_.exit_code()) + ")") +
                                   ": " + counts,
                               ok ? 4.0f : 8.0f);
            status_bar_.set_task_result(what + ": " + (errors || warnings ? counts : ok ? "ok" : "failed"), ok);
        }
    }
    if (pending_format_ && Clock::now() > pending_format_->deadline) {  // the server never answered
        auto then = std::move(pending_format_->then);
        pending_format_.reset();
        set_status_message("Formatting timed out; saved unformatted");
        if (then) then();
    }
    refresh_problems();

    // Panel status line.
    if (tasks_.running()) problems_.set_status(tasks_.step_label() + ": running...", 0);
    else if (tasks_.state() == TaskRunner::State::Failed) problems_.set_status(tasks_.label() + ": failed", 1);
    else if (tasks_.state() == TaskRunner::State::Succeeded) problems_.set_status(tasks_.label() + ": succeeded", 2);
}

void App::refresh_problems() {
    // A cheap fingerprint of the language-server diagnostics: rebuild when it changes.
    static uint64_t last_sig = 0, last_dirty = 0;
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    for (auto& [file, list] : diagnostics_) {
        mix(std::hash<std::string>{}(file));
        for (auto& d : list) {
            mix(d.start.line);
            mix(d.start.col);
            mix(static_cast<uint64_t>(d.severity));
            mix(d.message.size());
        }
    }
    if (sig != last_sig || problems_dirty_ != last_dirty) {
        last_sig = sig;
        last_dirty = problems_dirty_;
        std::vector<ProblemsPanel::Item> items;
        for (auto& [file, list] : diagnostics_)
            for (auto& d : list) {
                ProblemsPanel::Item it;
                it.file = file;
                it.line = d.start.line;
                it.col = d.start.col;
                it.severity = d.severity;
                it.message = d.message;
                it.code = d.code;
                it.source = d.source.empty() ? lsp_server_ : d.source;
                items.push_back(std::move(it));
            }
        merged_diags_.clear();
        std::map<std::string, FileLines> lines;
        for (auto& p : task_problems_) {
            ProblemsPanel::Item it;
            it.file = p.file;
            it.line = p.line;
            it.col = p.col;
            it.severity = p.severity;
            it.message = p.message;
            it.code = p.code;
            it.source = p.source;
            it.notes = p.notes;
            items.push_back(std::move(it));
            if (p.file.empty()) continue;
            auto [mit, fresh] = merged_diags_.try_emplace(p.file);
            if (fresh)
                if (auto lit = diagnostics_.find(p.file); lit != diagnostics_.end()) mit->second = lit->second;
            LSPDiagnostic d;
            d.start = {p.line, p.col};
            d.end = {p.end_line, p.end_col};
            if (!(d.start < d.end)) {
                auto& fl = lines[p.file];
                if (fl.path.empty()) fl.path = p.file, fl.ed = open_editor(tab_bar_, tab_bar2_, p.file);
                d.end = {p.line, token_end(fl.line(p.line), p.col)};
            }
            d.message = p.message + (p.code.empty() ? "" : " [" + p.code + "]");
            d.severity = p.severity;
            d.source = p.source;
            mit->second.push_back(std::move(d));
        }
        problems_.set_items(std::move(items));
    }
    // Squiggles in the editor: LSP diagnostics plus the last build's problems.
    if (Editor* ed = current_editor()) {
        auto it = merged_diags_.find(ed->buffer().file_path().string());
        if (it != merged_diags_.end()) editor_view_.set_diagnostics(&it->second);
    }
    status_bar_.set_diagnostic_counts(problems_.count(1), problems_.count(2));
}

void App::goto_problem(int dir) {
    std::vector<const ProblemsPanel::Item*> list;
    for (auto& it : problems_.items())
        if (it.severity <= 3) list.push_back(&it);
    if (list.empty()) {
        set_status_message("No problems");
        return;
    }
    Editor* ed = current_editor();
    std::string file = ed ? ed->buffer().file_path().string() : std::string();
    Position cur = ed ? ed->cursors().primary_pos() : Position{0, 0};
    auto key = [](const std::string& f, size_t l, size_t c) { return std::tuple(f, l, c); };
    auto here = key(file, cur.line, cur.col);
    const ProblemsPanel::Item* pick = nullptr;
    if (dir > 0) {
        for (auto* it : list)
            if (key(it->file, it->line, it->col) > here) { pick = it; break; }
        if (!pick) pick = list.front();  // wrap around
    } else {
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (key((*it)->file, (*it)->line, (*it)->col) < here) { pick = *it; break; }
        if (!pick) pick = list.back();
    }
    ProblemsPanel::Item p = *pick;
    std::error_code ec;
    if (!fs::is_regular_file(p.file, ec)) {
        set_status_message("File not found: " + p.file, 6.0f);
        return;
    }
    open_at(p.file, p.line, p.col, 0);
    problems_.reveal(p.file, p.line, p.col);
    set_status_message(std::string(p.severity == 1 ? "Error: " : p.severity == 2 ? "Warning: " : "") +
                           p.message.substr(0, p.message.find('\n')),
                       6.0f);
}

// ---------------------------------------------------------------------------
// Text edits (code actions, formatting, rustc suggestions)
// ---------------------------------------------------------------------------

bool App::apply_edits(const std::vector<ProblemEdit>& edits, int unit, std::string* summary) {
    std::map<std::string, std::vector<const ProblemEdit*>> by_file;
    for (auto& e : edits) by_file[e.file].push_back(&e);
    int files = 0, failed = 0;
    for (auto& [file, list] : by_file) {
        Editor* ed = open_editor(tab_bar_, tab_bar2_, file);
        if (ed && ed->read_only()) {
            failed++;
            continue;
        }
        std::string disk;
        std::vector<size_t> starts;  // disk text line starts
        if (!ed) {
            if (!vfs_->read(file, disk)) {
                failed++;
                continue;
            }
            starts.push_back(0);
            for (size_t i = 0; i < disk.size(); i++)
                if (disk[i] == '\n') starts.push_back(i + 1);
        }
        auto line_text = [&](size_t l) -> std::string {
            if (ed) return l < ed->buffer().line_count() ? ed->buffer().line_text(l) : std::string();
            if (l >= starts.size()) return {};
            size_t end = l + 1 < starts.size() ? starts[l + 1] - 1 : disk.size();
            std::string t = disk.substr(starts[l], end - starts[l]);
            if (!t.empty() && t.back() == '\r') t.pop_back();
            return t;
        };
        auto offset = [&](size_t l, size_t c) -> size_t {
            size_t lines = ed ? ed->buffer().line_count() : starts.size();
            size_t len = ed ? ed->buffer().length() : disk.size();
            if (l >= lines) return len;
            size_t col = to_byte_col(line_text(l), c, unit);
            return ed ? ed->buffer().offset_of(l, col) : starts[l] + col;
        };
        struct Off { size_t a, b, idx; const std::string* text; };
        std::vector<Off> offs;
        for (size_t i = 0; i < list.size(); i++) {
            size_t a = offset(list[i]->line, list[i]->col), b = offset(list[i]->end_line, list[i]->end_col);
            if (b < a) std::swap(a, b);
            offs.push_back({a, b, i, &list[i]->text});
        }
        // Back to front keeps earlier offsets valid; same start: later edits first, so
        // inserts at one position keep their order.
        std::sort(offs.begin(), offs.end(), [](const Off& x, const Off& y) { return x.a != y.a ? x.a > y.a : x.idx > y.idx; });
        if (ed) {
            Position keep = ed->cursors().primary_pos();
            auto& buf = ed->buffer();
            buf.begin_undo_group();
            for (auto& o : offs) buf.replace(o.a, o.b - o.a, *o.text);
            buf.end_undo_group();
            ed->sync_syntax();
            keep.line = std::min(keep.line, buf.line_count() - 1);
            keep.col = std::min(keep.col, buf.line_length(keep.line));
            ed->cursors().set_primary(keep);
        } else {
            for (auto& o : offs) disk.replace(o.a, o.b - o.a, *o.text);
            if (!vfs_->write(file, disk)) {
                failed++;
                continue;
            }
        }
        files++;
    }
    if (summary) {
        if (failed) *summary = "Could not apply edits to " + std::to_string(failed) + " file(s)";
        else if (files > 1) *summary = "Applied edits to " + std::to_string(files) + " files";
        else summary->clear();
    }
    request_redraw();
    return failed == 0;
}

void App::code_actions() {
    Editor* ed = current_editor();
    if (!ed || ed->buffer().file_path().empty()) return;
    fs::path path = ed->buffer().file_path();
    Position cur = ed->cursors().primary_pos();

    // rustc / clippy suggestions from the last build, on the cursor's line.
    struct Choice { std::string title, detail; std::function<void()> apply; bool preferred = false; };
    auto build_fixes = std::make_shared<std::vector<Choice>>();
    for (auto& p : task_problems_) {
        if (p.file != path.string() || cur.line < p.line || cur.line > p.end_line) continue;
        for (auto& fx : p.fixes) {
            std::vector<ProblemEdit> edits = fx.edits;
            build_fixes->push_back({fx.title, p.source + (p.code.empty() ? "" : " " + p.code),
                                    [this, edits] {
                                        std::string summary;
                                        apply_edits(edits, EDIT_BYTES, &summary);
                                        if (!summary.empty()) set_status_message(summary);
                                    },
                                    true});
        }
    }
    auto show = [this](std::vector<Choice> choices) {
        if (choices.empty()) {
            set_status_message("No code actions available");
            return;
        }
        std::stable_sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) { return a.preferred > b.preferred; });
        CommandPalette::PickOptions opts;
        opts.placeholder = "Quick Fix / Code Actions";
        for (auto& c : choices) {
            opts.items.push_back(c.title);
            opts.details.push_back(c.detail);
        }
        opts.on_pick = [this, choices](int i, const std::string&) {
            if (i < 0) return;
            choices[static_cast<size_t>(i)].apply();
            focus_editor();
        };
        command_palette_.pick(std::move(opts));
    };

    if (!lsp_.is_running() || !lsp_.is_open(path)) {
        show(std::move(*build_fixes));
        return;
    }
    // Diagnostics on the cursor's line go in the context, as the server sent them.
    std::string diags = "[";
    if (auto it = diagnostics_.find(path.string()); it != diagnostics_.end())
        for (auto& d : it->second)
            if (d.start.line <= cur.line && cur.line <= d.end.line && !d.raw.empty())
                diags += (diags.size() > 1 ? "," : "") + d.raw;
    diags += "]";
    auto& sel = ed->cursors().selections().front();
    auto to_server = [&](Position p) {
        if (!lsp_.utf8_positions()) p.col = utf8::utf16_col(ed->buffer().line_text(p.line), p.col);
        return p;
    };
    Position a = sel.empty() ? lsp_position(*ed) : to_server(sel.min());
    Position b = sel.empty() ? a : to_server(sel.max());
    lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);  // flush edits first
    lsp_update();
    set_status_message("Looking for code actions...", 2.0f);
    lsp_.code_action(path, a, b, diags, [this, build_fixes, show](const std::string& response) {
        lsp_post([this, build_fixes, show, response] {
            json::Value v;
            json::parse(response, v);
            std::vector<Choice> choices = *build_fixes;
            for (auto& item : v["result"].items()) {
                if (item["disabled"].is_object()) continue;
                std::string title = item["title"].str();
                std::string kind = item["kind"].str();
                Choice c;
                c.title = title;
                c.detail = (lsp_server_.empty() ? std::string("server") : lsp_server_) + (kind.empty() ? "" : "  " + kind);
                c.preferred = item["isPreferred"].boolean();
                if (item["command"].is_string()) {  // a bare Command
                    std::string cmd = json::dump(item);
                    c.apply = [this, cmd] { lsp_.execute_command(cmd, {}); };
                } else {
                    std::vector<ProblemEdit> edits;
                    int skipped = 0;
                    collect_workspace_edit(item["edit"], edits, &skipped);
                    std::string cmd = item["command"].is_object() ? json::dump(item["command"]) : std::string();
                    c.apply = [this, edits, cmd, skipped] {
                        std::string summary;
                        if (!edits.empty()) apply_edits(edits, lsp_edit_unit(), &summary);
                        if (!cmd.empty()) lsp_.execute_command(cmd, {});
                        if (skipped) summary += (summary.empty() ? "" : "; ") + std::string("file operations skipped");
                        if (!summary.empty()) set_status_message(summary, 6.0f);
                    };
                }
                choices.push_back(std::move(c));
            }
            set_status_message("", 0.0f);
            show(std::move(choices));
        });
    });
}

void App::format_document(std::function<void()> then) {
    Editor* ed = current_editor();
    fs::path path = ed ? ed->buffer().file_path() : fs::path();
    if (!ed || path.empty() || !lsp_.is_running() || !lsp_.is_open(path) || ed->read_only()) {
        if (!then) set_status_message("No formatter for this file (formatting uses the language server)");
        if (then) then();
        return;
    }
    lsp_docs_[path.string()].changed = Clock::now() - std::chrono::seconds(1);  // the server must see the latest text
    lsp_update();
    uint64_t version = ed->buffer().version();
    uint64_t id = ++format_request_;
    if (then) pending_format_ = PendingFormat{id, Clock::now() + std::chrono::milliseconds(3000), then};
    auto ind = ed->indent_style();
    lsp_.formatting(path, ind.size, !ind.tabs, [this, path, version, id](const std::string& response) {
        lsp_post([this, path, version, id, response] {
            std::function<void()> then;
            if (pending_format_ && pending_format_->id == id) {
                then = std::move(pending_format_->then);
                pending_format_.reset();
            } else if (format_request_ != id) {
                return;  // superseded, or timed out and already saved
            }
            json::Value v;
            json::parse(response, v);
            Editor* ed = open_editor(tab_bar_, tab_bar2_, path.string());
            if (v["error"].is_object()) {
                set_status_message("Format failed: " + v["error"]["message"].str(), 6.0f);
            } else if (ed && ed->buffer().version() == version) {
                std::vector<ProblemEdit> edits;
                for (auto& e : v["result"].items()) push_text_edit(path.string(), e, edits);
                if (edits.empty()) {
                    if (!then) set_status_message("Already formatted");
                } else {
                    apply_edits(edits, lsp_edit_unit(), nullptr);
                    if (!then) set_status_message("Formatted " + path.filename().string());
                }
            } else if (ed) {
                set_status_message("Not formatted: the document changed meanwhile");
            }
            if (then) then();
        });
    });
}

} // namespace oh
