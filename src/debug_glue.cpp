// Debugger integration: commands (F5, F9, F10...), the Run and Debug view, the
// Debug Console, gutter breakpoints and inline values. The session model lives in
// src/debug/debugger.cpp; this file connects it to the editor and the palette.
#include "app.h"
#include <cctype>
#include <cstdio>
#include <fstream>

namespace oh {

namespace {
LineEdit line_edit(const TextBuffer::Change& c) {
    return {c.start_pt.line, c.start_pt.col, c.old_end_pt.line, c.old_end_pt.col, c.new_end_pt.line, c.new_end_pt.col};
}
std::string clip(std::string s, size_t n) {
    s = s.substr(0, s.find('\n'));
    if (s.size() > n) s = s.substr(0, n - 3) + "...";
    return s;
}
} // namespace

Panel& App::bottom_panel() {
    if (bottom_view_ == BottomView::DebugConsole) return debug_console_;
    return terminal_;
}

void App::show_debug_console(bool focus_it) {
    bottom_view_ = BottomView::DebugConsole;
    terminal_visible_ = true;
    debug_console_.scroll_to_end();
    if (focus_it) focus(&debug_console_);
}

void App::setup_debugger() {
    debug_panel_.set_debugger(&debugger_);
    debug_console_.set_debugger(&debugger_);
    Debugger::Host host;
    host.reveal = [this](const fs::path& path, long line) {
        open_at(path, static_cast<size_t>(std::max(0L, line)), 0, 0);
        if (Editor* ed = current_editor()) {  // centre the stopped line when it was off screen
            float lh = font_.line_height();
            float target = static_cast<float>(line) * lh;
            Layout l = compute_layout();
            if (target < ed->scroll_y() || target > ed->scroll_y() + l.editor.h - lh * 2.0f)
                ed->set_scroll(ed->scroll_x(), std::max(0.0f, target - l.editor.h * 0.4f));
        }
    };
    host.status = [this](const std::string& m) { set_status_message(m, 5.0f); };
    host.prompt = [this](const std::string& placeholder, const std::string&, std::function<void(const std::string&)> done) {
        CommandPalette::PickOptions opts;
        opts.placeholder = placeholder;
        opts.allow_free_text = true;
        opts.on_pick = [done](int, const std::string& q) { done(q); };
        opts.on_cancel = [done] { done(""); };
        command_palette_.pick(std::move(opts));
    };
    host.current_file = [this] {
        Editor* ed = current_editor();
        return ed ? ed->buffer().file_path() : fs::path();
    };
    host.setting = [this](const std::string& key) { return themes_.setting(key); };
    host.save_all = [this] {
        for (TabBar* tb : {&tab_bar_, &tab_bar2_})
            for (auto& tab : tb->tabs()) {
                if (!tab.editor || !tab.editor->is_modified() || tab.editor->buffer().file_path().empty()) continue;
                const fs::path& p = tab.editor->buffer().file_path();
                if (!vfs_->write(p, tab.editor->buffer().text())) continue;
                tab.editor->buffer().set_unmodified();
                std::error_code ec;
                if (auto t = fs::last_write_time(p, ec); !ec) disk_mtimes_[p.string()] = t;
            }
    };
    host.session_started = [this] {
        if (themes_.setting("debug.open_console_on_start") != "false") show_debug_console(false);
        if (themes_.setting("debug.open_view_on_start") != "false") {
            sidebar_view_ = SidebarView::Debug;
            sidebar_visible_ = true;
        }
    };
    host.session_ended = [this] { debug_toolbar_.hide(); };
    debugger_.set_host(std::move(host));

    DebugPanel::Callbacks cb;
    cb.pick_config = [this] { debug_command("debug_select_config"); };
    cb.open_launch_file = [this] { debug_command("debug_open_launch"); };
    cb.start = [this] { debug_command("debug_start"); };
    cb.add_watch = [this] { debug_command("debug_add_watch"); };
    cb.edit_watch = [this](size_t i) {
        if (i >= debugger_.watches().size()) return;
        CommandPalette::PickOptions opts;
        opts.placeholder = "Edit watch expression (empty removes it): " + debugger_.watches()[i].expr;
        opts.allow_free_text = true;
        opts.on_pick = [this, i](int, const std::string& q) { debugger_.edit_watch(i, q); };
        command_palette_.pick(std::move(opts));
    };
    cb.open_location = [this](const fs::path& p, size_t line) { open_at(p, line, 0, 0); };
    cb.edit_breakpoint = [this](const fs::path& p, size_t line) { debug_breakpoint_menu(p, line); };
    cb.add_function_breakpoint = [this] { debug_command("debug_function_breakpoint"); };
    debug_panel_.set_callbacks(std::move(cb));

    auto gutter = [this](size_t line, int button) {
        Editor* ed = current_editor();
        if (!ed || ed->buffer().file_path().empty()) return;
        fs::path p = normalize_path(ed->buffer().file_path());
        if (button == 3) debug_breakpoint_menu(p, line);
        else debugger_.toggle_breakpoint(p, line);
    };
    editor_view_.on_gutter_click = gutter;
    editor_view2_.on_gutter_click = gutter;
}

void App::debug_breakpoint_menu(const fs::path& path, size_t line) {
    SourceBreakpoint* b = debugger_.breakpoints().find(path, line);
    std::vector<std::string> items;
    std::vector<std::string> actions;
    if (b) {
        items = {"Edit Condition...", "Edit Hit Count...", "Edit Log Message...",
                 b->enabled ? "Disable Breakpoint" : "Enable Breakpoint", "Remove Breakpoint"};
        actions = {"condition", "hits", "log", "enable", "remove"};
    } else {
        items = {"Add Breakpoint", "Add Conditional Breakpoint...", "Add Logpoint..."};
        actions = {"add", "condition", "log"};
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = path.filename().string() + ":" + std::to_string(line + 1);
    opts.items = items;
    opts.on_pick = [this, path, line, actions](int i, const std::string&) {
        if (i < 0) return;
        const std::string& a = actions[static_cast<size_t>(i)];
        if (a == "add") {
            debugger_.breakpoints().ensure(path, line);
            debugger_.breakpoints_changed(path);
        } else if (a == "remove") {
            debugger_.breakpoints().remove(path, line);
            debugger_.breakpoints_changed(path);
        } else if (a == "enable") {
            if (SourceBreakpoint* b = debugger_.breakpoints().find(path, line)) b->enabled = !b->enabled;
            debugger_.breakpoints_changed(path);
        } else {
            debug_edit_breakpoint(path, line, a);
        }
    };
    command_palette_.pick(std::move(opts));
}

void App::debug_edit_breakpoint(const fs::path& path, size_t line, const std::string& what) {
    SourceBreakpoint* b = debugger_.breakpoints().find(path, line);
    std::string current = !b ? "" : what == "condition" ? b->condition : what == "hits" ? b->hit_condition : b->log_message;
    CommandPalette::PickOptions opts;
    opts.placeholder = what == "condition" ? "Break when this expression is true (e.g. i == 3)"
                     : what == "hits" ? "Break after this many hits (e.g. 5, >= 5)"
                                      : "Log message, {expression} interpolates (e.g. i = {i})";
    if (!current.empty()) opts.placeholder += "   [now: " + current + "]";
    opts.allow_free_text = true;
    opts.on_pick = [this, path, line, what](int, const std::string& q) {
        SourceBreakpoint& b = debugger_.breakpoints().ensure(path, line);
        if (what == "condition") b.condition = q;
        else if (what == "hits") b.hit_condition = q;
        else b.log_message = q;
        b.enabled = true;
        debugger_.breakpoints_changed(path);
        set_status_message(q.empty() ? "Breakpoint updated" : "Breakpoint " + what + ": " + q);
    };
    command_palette_.pick(std::move(opts));
}

bool App::debug_command(const std::string& cmd) {
    if (cmd.rfind("debug_", 0) != 0) return false;
    auto cursor_bp = [this](const std::string& what) {
        Editor* ed = current_editor();
        if (!ed || ed->buffer().file_path().empty()) return;
        debug_edit_breakpoint(normalize_path(ed->buffer().file_path()), ed->cursors().primary_pos().line, what);
    };
    if (cmd == "debug_start") {
        if (remote_active_) {
            set_status_message("Debugging runs on this machine only (not over Remote-SSH yet)");
            return true;
        }
        debugger_.start_or_continue();
    } else if (cmd == "debug_continue") {
        debugger_.continue_();
    } else if (cmd == "debug_step_over") {
        debugger_.step_over();
    } else if (cmd == "debug_step_into") {
        if (debugger_.active()) debugger_.step_into();
        else handle_command("toggle_fullscreen");  // F11 outside a session, like VS Code
    } else if (cmd == "debug_step_out") {
        debugger_.step_out();
    } else if (cmd == "debug_pause") {
        debugger_.pause();
    } else if (cmd == "debug_stop") {
        debugger_.stop();
    } else if (cmd == "debug_restart") {
        debugger_.restart();
    } else if (cmd == "debug_toggle_breakpoint") {
        if (Editor* ed = current_editor(); ed && !ed->buffer().file_path().empty())
            debugger_.toggle_breakpoint(normalize_path(ed->buffer().file_path()), ed->cursors().primary_pos().line);
    } else if (cmd == "debug_conditional_breakpoint") {
        cursor_bp("condition");
    } else if (cmd == "debug_logpoint") {
        cursor_bp("log");
    } else if (cmd == "debug_hit_count") {
        cursor_bp("hits");
    } else if (cmd == "debug_breakpoint_menu") {
        if (Editor* ed = current_editor(); ed && !ed->buffer().file_path().empty())
            debug_breakpoint_menu(normalize_path(ed->buffer().file_path()), ed->cursors().primary_pos().line);
    } else if (cmd == "debug_function_breakpoint") {
        CommandPalette::PickOptions opts;
        opts.placeholder = "Function to break on (e.g. main, my_crate::run, Inventory::add)";
        opts.allow_free_text = true;
        opts.on_pick = [this](int, const std::string& q) {
            if (q.empty()) return;
            debugger_.breakpoints().functions().push_back(FunctionBreakpoint{q, {}, true, -1, false});
            debugger_.function_breakpoints_changed();
        };
        command_palette_.pick(std::move(opts));
    } else if (cmd == "debug_remove_all_breakpoints") {
        auto files = debugger_.breakpoints().files();
        debugger_.breakpoints().remove_all();
        for (auto& f : files) debugger_.breakpoints_changed(f);
        debugger_.function_breakpoints_changed();
    } else if (cmd == "debug_add_watch") {
        CommandPalette::PickOptions opts;
        opts.placeholder = "Expression to watch";
        opts.allow_free_text = true;
        opts.on_pick = [this](int, const std::string& q) { debugger_.add_watch(q); };
        command_palette_.pick(std::move(opts));
    } else if (cmd == "debug_watch_selection") {
        if (Editor* ed = current_editor()) {
            bool line_mode = false;
            std::string text = ed->cursors().selections().front().empty() ? "" : ed->copy_text(line_mode);
            if (text.empty()) {  // the word under the cursor
                Position p = ed->cursors().primary_pos();
                std::string l = ed->buffer().line_text(p.line);
                size_t a = p.col, b = p.col;
                auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                while (a > 0 && word(l[a - 1])) a--;
                while (b < l.size() && word(l[b])) b++;
                text = l.substr(a, b - a);
            }
            debugger_.add_watch(text);
            show_sidebar_view(SidebarView::Debug);
        }
    } else if (cmd == "debug_select_config") {
        debugger_.refresh_configs();
        auto& cfgs = debugger_.configs();
        std::vector<std::string> items, details;
        for (auto& c : cfgs) {
            items.push_back(c.name);
            details.push_back(c.origin + " · " + adapter_name(c.adapter));
        }
        if (items.empty()) {
            set_status_message("No launch configurations; opening .overheat/launch.toml");
            debug_command("debug_open_launch");
            return true;
        }
        CommandPalette::PickOptions opts;
        opts.placeholder = "Select a launch configuration (Enter starts it)";
        opts.items = items;
        opts.details = details;
        opts.initial = std::max(0, debugger_.selected_config());
        opts.on_pick = [this](int i, const std::string&) {
            if (i < 0) return;
            debugger_.select_config(i);
            if (!debugger_.active()) debugger_.start(i);
        };
        command_palette_.pick(std::move(opts));
    } else if (cmd == "debug_open_launch") {
        fs::path p = project_root_ / ".overheat" / "launch.toml";
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            fs::create_directories(p.parent_path(), ec);
            std::ofstream(p) << launch_toml_template(debugger_.configs());
            debugger_.refresh_configs();
        }
        open_file(p);
    } else if (cmd == "debug_view") {
        show_sidebar_view(SidebarView::Debug);
    } else if (cmd == "debug_console") {
        if (bottom_view_ == BottomView::DebugConsole && terminal_visible_ && debug_console_.focused()) {
            terminal_visible_ = false;
            focus_editor();
        } else {
            show_debug_console(true);
        }
    } else if (cmd == "debug_clear_console") {
        debugger_.clear_console();
    } else if (cmd == "debug_toggle_console_on_start") {
        bool on = themes_.setting("debug.open_console_on_start") == "false";
        themes_.set_setting("debug.open_console_on_start", on ? "true" : "false");
    } else if (cmd == "debug_toggle_inline_values") {
        bool on = themes_.setting("debug.inline_values") == "false";
        themes_.set_setting("debug.inline_values", on ? "true" : "false");
        debug_marks_.clear();
        set_status_message(std::string("Inline values ") + (on ? "on" : "off"));
    } else {
        return false;
    }
    return true;
}

void App::debug_register_commands() {
    auto add = [this](const std::string& name, const std::string& label, const std::string& keys) {
        commands_.push_back({name, label, [this, name]() { handle_command(name); }, keys});
    };
    add("debug_start",                  "Debug: Start Debugging / Continue", "F5");
    add("debug_step_over",              "Debug: Step Over",                  "F10");
    add("debug_step_into",              "Debug: Step Into",                  "F11");
    add("debug_step_out",               "Debug: Step Out",                   "Shift+F11");
    add("debug_pause",                  "Debug: Pause",                      "F6");
    add("debug_stop",                   "Debug: Stop",                       "Shift+F5");
    add("debug_restart",                "Debug: Restart",                    "Ctrl+Shift+F5");
    add("debug_toggle_breakpoint",      "Debug: Toggle Breakpoint",          "F9");
    add("debug_conditional_breakpoint", "Debug: Add Conditional Breakpoint...", "");
    add("debug_hit_count",              "Debug: Add Hit Count Breakpoint...", "");
    add("debug_logpoint",               "Debug: Add Logpoint...",            "");
    add("debug_breakpoint_menu",        "Debug: Edit Breakpoint at Cursor...", "");
    add("debug_function_breakpoint",    "Debug: Add Function Breakpoint...", "");
    add("debug_remove_all_breakpoints", "Debug: Remove All Breakpoints",     "");
    add("debug_add_watch",              "Debug: Add Watch Expression...",    "");
    add("debug_watch_selection",        "Debug: Add Selection to Watch",     "");
    add("debug_select_config",          "Debug: Select and Start Debugging...", "");
    add("debug_open_launch",            "Debug: Open launch.toml",           "");
    add("debug_view",                   "View: Run and Debug",               "");
    add("debug_console",                "View: Debug Console",               "Ctrl+Shift+Y");
    add("debug_clear_console",          "Debug: Clear Console",              "");
    add("debug_toggle_inline_values",   "Debug: Toggle Inline Values",       "");
}

const DebugMarks* App::debug_marks_for(Editor* ed) {
    if (!ed || ed->buffer().file_path().empty()) return nullptr;
    fs::path path = normalize_path(ed->buffer().file_path());
    CachedMarks& cm = debug_marks_[path.string()];
    uint64_t bp_gen = debugger_.breakpoints().generation(), dbg_gen = debugger_.generation();
    uint64_t ver = ed->buffer().version();
    if (cm.valid && cm.bp_gen == bp_gen && cm.dbg_gen == dbg_gen && cm.buf_ver == ver) return &cm.marks;
    cm = CachedMarks{true, bp_gen, dbg_gen, ver, {}};
    DebugMarks& m = cm.marks;
    bool session = debugger_.active() && debugger_.state() != DebugState::Building;
    for (auto& b : debugger_.breakpoints().source()) {
        if (b.path != path) continue;
        DebugMarks::Breakpoint mb;
        mb.line = b.shown_line();
        mb.kind = b.is_logpoint() ? DebugMarks::Breakpoint::Log
                : b.is_conditional() ? DebugMarks::Breakpoint::Conditional : DebugMarks::Breakpoint::Plain;
        mb.enabled = b.enabled;
        mb.verified = !session || !b.enabled || b.verified || debugger_.state() == DebugState::Launching;
        m.breakpoints.push_back(mb);
    }
    const DebugFrame* f = debugger_.current_frame();
    if (!f || f->path != path || f->line < 0) return &m;
    m.exec_line = f->line;
    m.exec_top = debugger_.selected_frame() == 0;
    if (themes_.setting("debug.inline_values") == "false") return &m;

    // Inline values: identifiers of the current function, from its start down to
    // the stopped line, that name a local or argument of the selected frame.
    auto vars = debugger_.frame_variables();
    if (vars.empty()) return &m;
    std::unordered_map<std::string, std::string> value_of;
    for (auto& v : vars) value_of.emplace(v.name, debugger_.display_value(v));
    auto& buf = ed->buffer();
    size_t stop_line = std::min(static_cast<size_t>(f->line), buf.line_count() - 1);
    size_t stop_end = buf.line_start(stop_line) + buf.line_length(stop_line);
    size_t fn_start = 0, fn_end = 0;
    if (!ed->syntax().function_range_at(buf.line_start(stop_line) + buf.line_length(stop_line) / 2, fn_start, fn_end))
        fn_start = buf.line_start(stop_line > 40 ? stop_line - 40 : 0);
    size_t first_line = buf.position_of(fn_start).line;
    if (stop_line > first_line + 300) fn_start = buf.line_start(stop_line - 300);  // keep it cheap
    std::map<size_t, std::vector<std::string>> per_line;
    for (auto [off, len] : ed->syntax().identifiers_in(fn_start, stop_end)) {
        Position p = buf.position_of(off);
        std::string name = buf.line_text(p.line).substr(p.col, len);
        auto it = value_of.find(name);
        if (it == value_of.end()) continue;
        auto& names = per_line[p.line];
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    for (auto& [line, names] : per_line) {
        std::string text;
        for (auto& n : names) {
            std::string part = n + " = " + clip(value_of[n], 40);
            if (!text.empty()) text += ", ";
            text += part;
            if (text.size() > 120) break;
        }
        m.inline_values.emplace_back(line, clip(text, 140));
    }
    return &m;
}

void App::debug_update() {
    if (!remote_active_ && debugger_.root() != project_root_) {
        debugger_.set_root(project_root_, config_dir_);
        debug_marks_.clear();
    }
    debugger_.update();
    // Breakpoints follow edits in every open buffer.
    for (TabBar* tb : {&tab_bar_, &tab_bar2_})
        for (auto& tab : tb->tabs())
            if (tab.editor && !tab.editor->buffer().on_change)
                tab.editor->buffer().on_change = [this](const TextBuffer& b, const TextBuffer::Change& c) {
                    if (b.file_path().empty() || debugger_.breakpoints().source().empty()) return;
                    debugger_.on_edit(normalize_path(b.file_path()), line_edit(c));
                };
    if (debug_marks_.size() > 64) debug_marks_.clear();
    editor_view_.set_debug_marks(debug_marks_for(editor_view_.editor().get()));
    editor_view2_.set_debug_marks(split_ ? debug_marks_for(editor_view2_.editor().get()) : nullptr);
}

void App::debug_probe() {
    const DebugFrame* f = debugger_.current_frame();
    std::fprintf(stderr, "  debug: state=%s config=\"%s\" stop=\"%s\" frame=%s line=%ld threads=%zu bps=%zu watches=%zu\n",
                 debug_state_name(debugger_.state()), debugger_.selected_config_name().c_str(),
                 debugger_.stop_description().c_str(), f ? f->name.c_str() : "-", f ? f->line + 1 : -1L,
                 debugger_.threads().size(), debugger_.breakpoints().source().size(), debugger_.watches().size());
    if (sidebar_visible_ && sidebar_view_ == SidebarView::Debug)
        for (auto& row : debug_panel_.last_rows()) std::fprintf(stderr, "  debug-row: %s\n", row.c_str());
    if (Editor* ed = current_editor())
        if (const DebugMarks* m = debug_marks_for(ed)) {
            for (auto& b : m->breakpoints)
                std::fprintf(stderr, "  debug-bp: line=%zu kind=%d enabled=%d verified=%d\n", b.line + 1,
                             static_cast<int>(b.kind), b.enabled, b.verified);
            for (auto& [line, text] : m->inline_values)
                std::fprintf(stderr, "  debug-inline: %zu: %s\n", line + 1, text.c_str());
        }
    auto& con = debugger_.console();
    for (size_t i = con.size() > 12 ? con.size() - 12 : 0; i < con.size(); i++)
        std::fprintf(stderr, "  debug-console: %s\n", clip(con[i].text, 160).c_str());
}

} // namespace oh
