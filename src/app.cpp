#include "app.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace oh {

void request_redraw() {
    if (!SDL_HasEvent(SDL_EVENT_USER)) {
        SDL_Event wake{};
        wake.type = SDL_EVENT_USER;
        SDL_PushEvent(&wake);
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

App::App() = default;

App::~App() {
    if (vk_.device()) vkDeviceWaitIdle(vk_.device());
    plugins_.shutdown();

    if (lsp_.is_running()) lsp_.stop();
    if (ssh_.is_connected()) ssh_.disconnect();
    terminal_.kill();

    font_.cleanup(vk_);
    renderer_.cleanup(vk_);
    vk_.cleanup();

    if (window_) {
        SDL_StopTextInput(window_);
        SDL_DestroyWindow(window_);
    }
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool App::init(int argc, char** argv) {
    // ---- Parse arguments ----
    std::vector<fs::path> file_args;
    fs::path script_path;
    project_root_ = fs::current_path();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--project=", 0) == 0) {
            project_root_ = fs::absolute(arg.substr(10));
        } else if (arg == "--project" && i + 1 < argc) {
            project_root_ = fs::absolute(argv[++i]);
        } else if (arg == "--script" && i + 1 < argc) {
            script_path = argv[++i];
        } else if (arg == "--shot-dir" && i + 1 < argc) {
            shot_dir_ = argv[++i];
        } else if (arg == "--debug-hit") {
            debug_hit_ = true;
        } else if (arg[0] != '-') {
            file_args.push_back(fs::absolute(arg));
        }
    }

    // ---- SDL ----
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow("Overheat",
                               1600, 900,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // ---- Vulkan ----
    if (!vk_.init(window_)) {
        std::fprintf(stderr, "VulkanContext::init failed\n");
        return false;
    }

    // ---- Font ----
    // Primary is the first that exists; the rest form the glyph fallback chain.
    const char* font_candidates[] = {
        "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
    };
    std::vector<std::string> font_chain;
    for (const char* fp : font_candidates)
        if (fs::exists(fp)) font_chain.push_back(fp);
    if (!font_.init(vk_, font_chain, 15.0f)) {
        std::fprintf(stderr, "Could not load any font\n");
        return false;
    }

    // ---- Renderers ----
    if (!renderer_.init(vk_, font_)) {
        std::fprintf(stderr, "Renderer2D::init failed\n");
        return false;
    }

    // ---- Theme ----
    {
        const char* home = std::getenv("HOME");
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        fs::path config = xdg && *xdg ? fs::path(xdg) / "overheat" : fs::path(home ? home : ".") / ".config" / "overheat";
        themes_.init(config);
        std::string choice = themes_.saved_choice();
        if (choice.empty() || !themes_.apply(choice, theme_)) themes_.apply("Overheat Dark", theme_);
    }

    // ---- Key bindings ----
    keymap_.init_defaults();

    // Try to load user key bindings (override defaults)
    fs::path user_keys = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "~")
                         / ".config" / "overheat" / "keybindings.json";
    if (fs::exists(user_keys)) {
        keymap_.load(user_keys);
    }

    // ---- Commands ----
    register_commands();

    // ---- Git ----
    git_.open(project_root_);
    if (git_.is_open()) {
        status_bar_.set_git_branch(git_.branch_name());
        request_git_refresh();
    }

    // ---- File tree ----
    file_tree_.set_root(project_root_, vfs_.get());
    file_tree_.set_on_open([this](const fs::path& p) { open_file(p); });
    setup_scm();
    setup_search();

    // ---- Terminal ----
    terminal_.spawn({}, project_root_);

    // ---- Open files from command line, or create empty editor ----
    if (!file_args.empty()) {
        for (auto& fp : file_args) {
            open_file(fp);
        }
    } else {
        new_file();
    }

    // ---- Plugins ----
    plugins_.init(this);
    fs::path plugin_dir = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "~")
                          / ".config" / "overheat" / "plugins";
    plugins_.scan_directory(plugin_dir);

    // ---- Scan project files for command palette ----
    scan_project_files();
    command_palette_.set_commands(commands_);
    command_palette_.set_root(project_root_);
    command_palette_.set_file_list(all_files_);
    command_palette_.set_on_file_open([this](const fs::path& p) { open_file(p); });

    // ---- Text input ----
    SDL_StartTextInput(window_);

    if (!script_path.empty() && !script_.load(script_path, window_))
        return false;

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::run() {
    auto last_time = Clock::now();
    constexpr float FRAME_BUDGET_MS = 1000.0f / 60.0f;

    while (running_) {
        // Event-driven: sleep until input, terminal output (SDL_EVENT_USER), the next
        // cursor blink, or the next scripted step.
        int timeout = 500;
        if (script_.active()) timeout = std::min(timeout, script_.ms_until_next(Clock::now()));
        SDL_Event e;
        if (pending_probes_.empty() && SDL_WaitEventTimeout(&e, timeout))
            handle_event(e);
        while (SDL_PollEvent(&e))
            handle_event(e);
        if (!running_) break;

        auto frame_start = Clock::now();
        float dt = std::chrono::duration<float>(frame_start - last_time).count();
        last_time = frame_start;

        update(dt);
        auto probe_start = Clock::now();
        for (auto& label : pending_probes_) {
            emit_probe(label);
            // Screenshot probes block until the driver acks on stdin, so the capture
            // shows exactly this state (the frame for it was presented last iteration).
            if (label.rfind("shot:", 0) == 0) {
                if (!shot_dir_.empty()) {
                    // Capture this iteration's frame ourselves; no compositor involved.
                    std::string name = label.substr(5);
                    vk_.request_capture((fs::path(shot_dir_) / (name + ".ppm")).string());
                } else {
                    char ack[16];
                    if (!std::fgets(ack, sizeof(ack), stdin)) {}
                }
            }
        }
        pending_probes_.clear();
        frame_start += Clock::now() - probe_start;  // test-harness waits are not frame time
        for (auto& label : script_.step(Clock::now())) pending_probes_.push_back(label);

        render();
        frame_count_++;

        float frame_ms = std::chrono::duration<float, std::milli>(Clock::now() - frame_start).count();
        frame_times_ms_.push_back(frame_ms);
        if (!first_paint_done_) {
            first_paint_done_ = true;
            std::fprintf(stderr, "first paint time: %.1f ms\n",
                         std::chrono::duration<float, std::milli>(Clock::now() - start_time_).count());
        } else if (frame_ms > FRAME_BUDGET_MS) {
            std::fprintf(stderr, "frame time exceeded 60hz: %.2f ms (%zu quads)\n",
                         frame_ms, renderer_.quad_count());
        }
    }
    print_frame_stats();
}

void App::print_frame_stats() const {
    if (frame_times_ms_.size() < 2) return;
    std::vector<float> t(frame_times_ms_.begin() + 1, frame_times_ms_.end());  // skip first paint
    std::sort(t.begin(), t.end());
    double sum = 0;
    for (float v : t) sum += v;
    std::fprintf(stderr, "frame stats: %zu frames, avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f ms\n",
                 t.size(), sum / t.size(), t[t.size() / 2], t[t.size() * 95 / 100], t.back());
}

void App::emit_probe(const std::string& label) {
    Layout l = compute_layout();
    int w, h, pw, ph;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    VkExtent2D ext = vk_.extent();
    std::fprintf(stderr, "PROBE %s: window=%dx%d pixels=%dx%d swapchain=%ux%u\n",
                 label.c_str(), w, h, pw, ph, ext.width, ext.height);
    std::fprintf(stderr, "  editor=(%.0f,%.0f %.0fx%.0f) terminal=(%.0f,%.0f %.0fx%.0f) gutter=%.0f "
                 "char=%.1fx%.1f last_click=(%.0f,%.0f)\n",
                 l.editor.x, l.editor.y, l.editor.w, l.editor.h,
                 l.terminal.x, l.terminal.y, l.terminal.w, l.terminal.h,
                 editor_view_.last_gutter_width(), font_.char_width(), font_.line_height(),
                 last_click_x_, last_click_y_);
    const char* focus = terminal_.focused() ? "terminal"
                      : file_tree_.focused() ? "tree"
                      : editor_view_.focused() ? "editor" : "none";
    if (Editor* ed = current_editor()) {
        auto& sel = ed->cursors().selections();
        Position p = ed->cursors().primary_pos();
        std::fprintf(stderr, "  file=%s cursor=%zu:%zu (0-based) cursors=%zu scroll=(%.0f,%.0f) "
                     "focus=%s palette=%d modified=%d\n",
                     ed->buffer().file_path().filename().c_str(), p.line, p.col, sel.size(),
                     ed->scroll_x(), ed->scroll_y(), focus, command_palette_.is_visible(),
                     ed->is_modified());
        auto& s0 = sel.front();
        std::fprintf(stderr, "  selection=%zu:%zu-%zu:%zu line=\"%s\"\n", s0.anchor.line,
                     s0.anchor.col, s0.head.line, s0.head.col,
                     ed->buffer().line_text(p.line).substr(0, 80).c_str());
    }
}

void App::quit() {
    running_ = false;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

KeyMod App::sdl_mods(SDL_Keymod m) const {
    KeyMod r = KeyMod::None;
    if (m & SDL_KMOD_CTRL)  r = r | KeyMod::Ctrl;
    if (m & SDL_KMOD_SHIFT) r = r | KeyMod::Shift;
    if (m & SDL_KMOD_ALT)   r = r | KeyMod::Alt;
    if (m & SDL_KMOD_GUI)   r = r | KeyMod::Super;
    return r;
}

void App::handle_event(const SDL_Event& e) {
    switch (e.type) {

    case SDL_EVENT_QUIT:
        quit();
        break;

    case SDL_EVENT_CLIPBOARD_UPDATE:
        if (!e.clipboard.owner) own_clipboard_.clear();
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        vk_.recreate_swapchain();
        break;

    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode kc = e.key.key;
        KeyMod mods = sdl_mods(e.key.mod);

        // Command palette intercepts keys when visible
        if (command_palette_.is_visible()) {
            KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
            if (command_palette_.handle_key(ke)) break;
        }

        // Sidebar text inputs (commit message, search) take typing keys before global bindings.
        if (sidebar_visible_ && !command_palette_.is_visible()) {
            KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
            if (sidebar_view_ == SidebarView::SourceControl && scm_.input_focused() && scm_.handle_key(ke)) break;
            if (sidebar_view_ == SidebarView::Search && search_.input_focused() && search_.handle_key(ke)) break;
        }

        // Check keymap. While the terminal has focus, the shell owns the keyboard
        // except for a few app-level shortcuts (and Ctrl+Shift+C/V for the clipboard).
        std::string cmd = keymap_.lookup(kc, mods);
        bool in_terminal = terminal_.focused() && terminal_visible_;
        if (!cmd.empty() && in_terminal) {
            static const char* global_cmds[] = {
                "toggle_terminal", "command_palette_files", "command_palette_commands",
                "next_tab", "prev_tab", "quit", "toggle_fullscreen"};
            bool global = std::any_of(std::begin(global_cmds), std::end(global_cmds),
                                      [&](const char* g) { return cmd == g; });
            bool clipboard = (cmd == "copy" || cmd == "paste") && (mods & KeyMod::Shift);
            if (!global && !clipboard) cmd.clear();
        }
        if (!cmd.empty()) {
            handle_command(cmd);
            break;
        }

        // Forward to focused panel
        KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
        if (terminal_.focused() && terminal_visible_) {
            terminal_.handle_key(ke);
        } else {
            editor_view_.handle_key(ke);
        }
        break;
    }

    case SDL_EVENT_KEY_UP: {
        KeyEvent ke{static_cast<uint32_t>(e.key.key), sdl_mods(e.key.mod), false};
        if (terminal_.focused() && terminal_visible_) {
            terminal_.handle_key(ke);
        } else {
            editor_view_.handle_key(ke);
        }
        break;
    }

    case SDL_EVENT_TEXT_INPUT: {
        const char* text = e.text.text;
        if (command_palette_.is_visible()) {
            command_palette_.handle_text(text);
        } else if (scm_.input_focused()) {
            scm_.handle_text(text);
        } else if (search_.input_focused()) {
            search_.handle_text(text);
        } else if (terminal_.focused() && terminal_visible_) {
            terminal_.handle_text(text);
        } else {
            editor_view_.handle_text(text);
        }
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float mx = e.button.x;
        float my = e.button.y;
        last_click_x_ = mx;
        last_click_y_ = my;
        Layout layout = compute_layout();

        MouseEvent me{mx, my, static_cast<int>(e.button.button), true,
                      (e.button.clicks >= 2), 0, 0};

        // Determine which panel was clicked and set focus
        if (command_palette_.is_visible()) {
            command_palette_.handle_mouse(me);
        } else if (layout.activity_bar.contains(mx, my)) {
            int slot = static_cast<int>((my - layout.activity_bar.y) / ACTIVITY_BAR_W);
            SidebarView v = static_cast<SidebarView>(std::clamp(slot, 0, 2));
            if (slot <= 2) {
                if (sidebar_visible_ && v == sidebar_view_) sidebar_visible_ = false;
                else show_sidebar_view(v);
            }
        } else if (sidebar_visible_ && layout.sidebar.contains(mx, my)) {
            focus(&sidebar_panel());
            sidebar_panel().handle_mouse(me);
        } else if (terminal_visible_ && layout.terminal.contains(mx, my)) {
            focus(&terminal_);
            terminal_.handle_mouse(me);
        } else if (layout.tab_bar.contains(mx, my)) {
            tab_bar_.handle_mouse(me);
        } else if (layout.editor.contains(mx, my)) {
            focus(&editor_view_);
            editor_view_.handle_mouse(me);
        }
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        MouseEvent me{e.button.x, e.button.y, static_cast<int>(e.button.button),
                      false, false, 0, 0};
        editor_view_.handle_mouse(me);
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        float mx = e.wheel.mouse_x;
        float my = e.wheel.mouse_y;
        Layout layout = compute_layout();

        MouseEvent me{mx, my, 0, false, false, e.wheel.x, e.wheel.y};

        if (sidebar_visible_ && layout.sidebar.contains(mx, my)) {
            sidebar_panel().handle_mouse(me);
        } else if (terminal_visible_ && layout.terminal.contains(mx, my)) {
            terminal_.handle_mouse(me);
        } else if (layout.editor.contains(mx, my)) {
            editor_view_.handle_mouse(me);
        }
        break;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        if (e.motion.state & SDL_BUTTON_LMASK) {
            MouseEvent me{e.motion.x, e.motion.y, 1, true, false, 0, 0, true};
            editor_view_.handle_mouse(me);
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void App::handle_command(const std::string& cmd) {
    if (cmd == "save") {
        save_current();
    } else if (cmd == "save_as") {
        save_as();
    } else if (cmd == "new_file") {
        new_file();
    } else if (cmd == "open_file") {
        // Open via command palette in files mode
        command_palette_.show(CommandPalette::Mode::Files);
    } else if (cmd == "close_tab") {
        close_current_tab();
    } else if (cmd == "next_tab") {
        tab_bar_.next_tab();
    } else if (cmd == "prev_tab") {
        tab_bar_.prev_tab();
    } else if (cmd == "command_palette_files") {
        command_palette_.show(CommandPalette::Mode::Files);
    } else if (cmd == "command_palette_commands") {
        command_palette_.show(CommandPalette::Mode::Commands);
    } else if (cmd == "toggle_terminal") {
        terminal_visible_ = !terminal_visible_;
    } else if (cmd == "toggle_sidebar") {
        sidebar_visible_ = !sidebar_visible_;
    } else if (cmd == "copy" || cmd == "cut") {
        if (terminal_.focused() && terminal_visible_) return;  // terminal selection: TODO
        if (Editor* ed = current_editor()) {
            bool line_mode = false;
            std::string text = cmd == "cut" ? ed->cut_text(line_mode) : ed->copy_text(line_mode);
            SDL_SetClipboardText(text.c_str());
            own_clipboard_ = text;
            clipboard_line_text_ = line_mode ? text : std::string();
        }
    } else if (cmd == "paste") {
        // SDL on Wayland can't read back a selection this client owns, so our own
        // copies are served from memory until another client takes the clipboard.
        std::string text = own_clipboard_;
        if (text.empty()) {
            char* raw = SDL_GetClipboardText();
            text = raw ? raw : "";
            SDL_free(raw);
        }
        if (text.empty()) return;
        if (terminal_.focused() && terminal_visible_) {
            terminal_.paste(text);
        } else if (Editor* ed = current_editor()) {
            ed->paste_text(text, !clipboard_line_text_.empty() && text == clipboard_line_text_);
            editor_view_.reveal_cursor();
        }
    } else if (cmd == "undo") {
        if (Editor* ed = current_editor()) ed->undo();
    } else if (cmd == "redo") {
        if (Editor* ed = current_editor()) ed->redo();
    } else if (cmd == "select_word") {
        if (Editor* ed = current_editor()) ed->cursors().select_word(ed->buffer());
    } else if (cmd == "select_line") {
        if (Editor* ed = current_editor()) ed->cursors().select_line(ed->buffer());
    } else if (cmd == "select_all") {
        if (Editor* ed = current_editor()) ed->cursors().select_all(ed->buffer());
    } else if (cmd == "toggle_comment") {
        if (Editor* ed = current_editor()) ed->toggle_comment();
    } else if (cmd == "delete_line") {
        if (Editor* ed = current_editor()) ed->delete_line();
    } else if (cmd == "move_line_up") {
        if (Editor* ed = current_editor()) ed->move_line_up();
    } else if (cmd == "move_line_down") {
        if (Editor* ed = current_editor()) ed->move_line_down();
    } else if (cmd == "duplicate_line") {
        if (Editor* ed = current_editor()) ed->duplicate_line();
    } else if (cmd == "goto_line") {
        if (Editor* ed = current_editor()) {
            CommandPalette::PickOptions opts;
            opts.placeholder = "Go to line (1 - " + std::to_string(ed->buffer().line_count()) + ")";
            opts.allow_free_text = true;
            opts.on_pick = [this](int, const std::string& q) {
                if (Editor* ed = current_editor(); ed && !q.empty()) {
                    long n = std::strtol(q.c_str(), nullptr, 10);
                    ed->goto_line(static_cast<size_t>(std::max(1L, n) - 1));
                    focus_editor();
                    editor_view_.reveal_cursor();
                }
            };
            command_palette_.pick(std::move(opts));
        }
    } else if (cmd == "find" || cmd == "replace") {
        focus_editor();
        editor_view_.open_find(cmd == "replace");
    } else if (cmd == "toggle_fullscreen") {
        uint32_t flags = SDL_GetWindowFlags(window_);
        if (flags & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window_, false);
        } else {
            SDL_SetWindowFullscreen(window_, true);
        }
    } else if (cmd == "zoom_in") {
        // TODO: adjust font size and reinit font atlas
    } else if (cmd == "zoom_out") {
        // TODO: adjust font size and reinit font atlas
    } else if (cmd == "remote_connect") {
        remote_connect_prompt();
    } else if (cmd == "remote_disconnect") {
        remote_disconnect();
    } else if (cmd == "view_explorer") {
        show_sidebar_view(SidebarView::Explorer);
    } else if (cmd == "view_search") {
        show_sidebar_view(SidebarView::Search);
        search_.focus_input();
    } else if (cmd == "view_scm") {
        show_sidebar_view(SidebarView::SourceControl);
    } else if (cmd == "git_checkout") {
        git_checkout();
    } else if (cmd == "toggle_blame") {
        editor_view_.toggle_blame();
    } else if (cmd == "select_theme") {
        select_theme();
    } else if (cmd == "quit") {
        quit();
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

App::Layout App::compute_layout() const {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    Layout l{};

    // Status bar: full width, 24px at bottom
    float status_h = 26.0f;
    l.status_bar = {0, fh - status_h, fw, status_h};

    float content_h = fh - status_h;

    // Activity bar (always visible), then the sidebar view.
    float ab_w = ACTIVITY_BAR_W;
    l.activity_bar = {0, 0, ab_w, content_h};
    float sb_w = sidebar_visible_ ? std::min(sidebar_width_, std::floor((fw - ab_w) * 0.28f)) : 0.0f;
    l.sidebar = {ab_w, 0, sb_w, content_h};

    float right_x = ab_w + sb_w;
    float right_w = fw - right_x;

    // Tab bar: 32px tall at top of right region
    float tab_h = 36.0f;
    l.tab_bar = {right_x, 0, right_w, tab_h};

    // Terminal: at bottom of right region (above status bar)
    float term_h = terminal_visible_
        ? std::min(terminal_height_, std::floor((content_h - tab_h) * 0.4f)) : 0.0f;
    l.terminal = {right_x, content_h - term_h, right_w, term_h};

    // Editor: remaining space
    float editor_y = tab_h;
    float editor_h = content_h - tab_h - term_h;
    if (editor_h < 0) editor_h = 0;
    l.editor = {right_x, editor_y, right_w, editor_h};

    return l;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void App::request_git_refresh() {
    if (!git_.is_open()) return;
    if (git_job_.valid()) {  // one at a time; run again when this one lands
        git_refresh_pending_ = true;
        return;
    }
    git_job_ = std::async(std::launch::async, [dir = git_.workdir()] {
        auto st = GitRepository::status_in(dir);
        request_redraw();
        return st;
    });
}

void App::poll_git_refresh() {
    if (!git_job_.valid() || git_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    git_statuses_ = git_job_.get();
    std::unordered_map<std::string, FileStatus> by_path;
    fs::path wd = git_.workdir();
    for (auto& s : git_statuses_) by_path[(wd / s.path).lexically_normal().string()] = s.status;
    file_tree_.set_git_status(by_path);
    scm_.set_state(wd, git_.branch_name(), git_statuses_);
    status_bar_.set_git_branch(git_.branch_name());
    int ahead = 0, behind = 0;
    git_.ahead_behind(ahead, behind);
    status_bar_.set_git_sync(ahead, behind);
    if (git_refresh_pending_) {
        git_refresh_pending_ = false;
        request_git_refresh();
    }
}

void App::attach_git(Editor& ed) {
    if (!git_.is_open() || ed.buffer().file_path().empty()) return;
    const fs::path path = ed.buffer().file_path();
    ed.set_git_base(git_.head_content(path));
    if (!ed.has_git_base()) return;
    ed.set_blame_job(std::async(std::launch::async, [wd = git_.workdir(), path] {
        auto b = GitRepository::blame_in(wd, path);
        request_redraw();
        return b;
    }));
}

void App::after_head_change() {
    for (auto& tab : tab_bar_.tabs()) {
        if (!tab.editor || tab.editor->buffer().file_path().empty()) continue;
        if (!tab.editor->is_modified()) tab.editor->reload();
        attach_git(*tab.editor);
    }
    file_tree_.refresh();
    status_bar_.set_git_branch(git_.branch_name());
    request_git_refresh();
}

void App::git_checkout() {
    if (!git_.is_open()) {
        set_status_message("Not a git repository");
        return;
    }
    std::string current = git_.branch_name();
    std::vector<std::string> branches = git_.branches();
    std::vector<std::string> items;
    int initial = 0;
    for (auto& b : branches) {
        if (b == current) initial = static_cast<int>(items.size());
        items.push_back(b == current ? b + "  (current)" : b);
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select a branch, or type a new name to create it";
    opts.items = items;
    opts.initial = initial;
    opts.allow_free_text = true;
    opts.on_pick = [this, branches](int i, const std::string& query) {
        bool ok;
        std::string name;
        if (i >= 0) {
            name = branches[static_cast<size_t>(i)];
            ok = git_.checkout(name);
        } else {
            name = query;
            ok = !name.empty() && git_.create_branch(name);
        }
        if (!ok) {
            set_status_message(git_.last_error().empty() ? "Checkout failed" : git_.last_error(), 6.0f);
            return;
        }
        set_status_message((i >= 0 ? "Switched to " : "Created and switched to ") + name);
        after_head_change();
    };
    command_palette_.pick(std::move(opts));
}

void App::select_theme() {
    auto names = themes_.names();
    std::string original = themes_.current();
    int initial = 0;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == original) initial = static_cast<int>(i);
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select color theme (live preview)";
    opts.items = names;
    opts.initial = initial;
    opts.on_preview = [this, names](int i) { themes_.apply(names[static_cast<size_t>(i)], theme_); };
    opts.on_cancel = [this, original] { themes_.apply(original, theme_); };
    opts.on_pick = [this, names](int i, const std::string&) {
        if (i < 0) return;
        themes_.apply(names[static_cast<size_t>(i)], theme_);
        themes_.save_choice(names[static_cast<size_t>(i)]);
        set_status_message("Theme: " + names[static_cast<size_t>(i)]);
    };
    command_palette_.pick(std::move(opts));
}

void App::update(float dt) {
    poll_remote_connect();
    search_.tick(dt);
    if (search_job_.valid() && search_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto hits = search_job_.get();
        if (!search_cancel_ || !search_cancel_->load()) {
            bool truncated = hits.size() >= 20000;
            search_.set_results(std::move(hits), truncated);
        }
    }
    poll_git_refresh();
    git_poll_timer_ += dt;
    if (git_poll_timer_ > 3.0f) {
        git_poll_timer_ = 0;
        request_git_refresh();
    }
    theme_poll_timer_ += dt;
    if (theme_poll_timer_ > 1.0f) {
        theme_poll_timer_ = 0;
        if (themes_.poll_reload(theme_)) set_status_message("Theme reloaded");
    }

    terminal_.update(dt);
    editor_view_.update(dt);
    status_bar_.update(dt);

    // Keep status bar in sync with current editor
    auto ed = tab_bar_.active_editor();
    status_bar_.set_editor(ed);
    editor_view_.set_editor(ed);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void App::render() {
    VkExtent2D extent = vk_.extent();

    Layout layout = compute_layout();

    RenderContext ctx;
    ctx.vk    = &vk_;
    ctx.r     = &renderer_;
    ctx.font  = &font_;
    ctx.theme = &theme_;
    ctx.dt    = 0; // already handled in update

    // One ordered quad stream for the whole frame
    renderer_.begin(extent);

    // Flat layout: panels separate by tone; only a hairline above the terminal.
    renderer_.add_rect({0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)}, theme_.bg);
    if (terminal_visible_)
        renderer_.add_rect({layout.terminal.x, layout.terminal.y, layout.terminal.w, 1}, theme_.border);

    // All panels add their UI rects (selections, highlights, gutter bg)
    auto clipped = [&](Panel& panel, Rect r) {
        renderer_.push_clip(r);
        panel.render(ctx, r);
        renderer_.pop_clip();
    };
    render_activity_bar(ctx, layout.activity_bar);
    if (sidebar_visible_) clipped(sidebar_panel(), layout.sidebar);
    clipped(tab_bar_, layout.tab_bar);
    clipped(editor_view_, layout.editor);
    if (terminal_visible_) clipped(terminal_, layout.terminal);
    clipped(status_bar_, layout.status_bar);
    if (command_palette_.is_visible())
        clipped(command_palette_, {0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)});

    if (debug_hit_ && last_click_x_ >= 0) {
        Color hit = Color::hex(0xff00ff);
        renderer_.add_rect({last_click_x_, 0, 1, static_cast<float>(extent.height)}, hit);
        renderer_.add_rect({0, last_click_y_, static_cast<float>(extent.width), 1}, hit);
    }

    // Glyphs first seen while building this frame must reach the GPU before it draws.
    font_.upload_pending(vk_);

    uint32_t image_index;
    if (!vk_.begin_frame(image_index)) {
        vk_.recreate_swapchain();
        return;
    }
    VkCommandBuffer cmd = vk_.current_frame_data().cmd;
    renderer_.flush(vk_, cmd);

    vk_.end_frame(image_index);
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void App::open_file(const fs::path& path) {
    // Check if already open
    for (int i = 0; i < static_cast<int>(tab_bar_.tabs().size()); ++i) {
        auto& tab = tab_bar_.tabs()[i];
        if (tab.editor && tab.editor->buffer().file_path() == path) {
            tab_bar_.set_active(i);
            focus_editor();
            return;
        }
    }

    std::string content;
    if (!vfs_->read(path, content)) {
        set_status_message("Cannot open " + path.string(), 6.0f);
        return;
    }
    auto editor = std::make_shared<Editor>();
    editor->load_content(path, content);
    if (!vfs_->is_remote()) attach_git(*editor);

    // Replace a pristine "Untitled" tab instead of piling up empty tabs.
    int replace = -1;
    if (auto cur = tab_bar_.active_editor(); cur && cur->buffer().file_path().empty() &&
                                              !cur->is_modified() && cur->buffer().length() == 0)
        replace = tab_bar_.active_index();
    tab_bar_.add_tab(editor);
    if (replace >= 0) tab_bar_.close_tab(replace);
    tab_bar_.set_active(static_cast<int>(tab_bar_.tabs().size()) - 1);
    editor_view_.set_editor(editor);
    status_bar_.set_editor(editor);
    focus_editor();

    setup_lsp_for_file(path);

}

void App::save_current() {
    if (Editor* ed = current_editor()) {
        if (ed->buffer().file_path().empty()) {
            save_as();
            return;
        }
        if (!vfs_->write(ed->buffer().file_path(), ed->buffer().text())) {
            set_status_message("Save FAILED: " + ed->buffer().file_path().string(), 8.0f);
            return;
        }
        ed->buffer().set_unmodified();
        set_status_message("Saved " + ed->buffer().file_path().filename().string());
        request_git_refresh();

        // Notify LSP
        if (lsp_.is_running()) {
            lsp_.did_save(ed->buffer().file_path());
        }
    }
}

void App::save_as() {
    Editor* ed = current_editor();
    if (!ed) return;
    CommandPalette::PickOptions opts;
    opts.placeholder = "Save as (path relative to " + project_root_.string() + ")";
    opts.allow_free_text = true;
    opts.on_pick = [this](int, const std::string& query) {
        Editor* ed = current_editor();
        if (!ed || query.empty()) return;
        fs::path p = fs::path(query).is_absolute() ? fs::path(query) : project_root_ / query;
        ed->buffer().set_file_path(p.lexically_normal());
        save_current();
        file_tree_.refresh();
    };
    command_palette_.pick(std::move(opts));
}

void App::new_file() {
    auto editor = std::make_shared<Editor>();
    editor->new_file();
    tab_bar_.add_tab(editor);
    tab_bar_.set_active(static_cast<int>(tab_bar_.tabs().size()) - 1);
    editor_view_.set_editor(editor);
    status_bar_.set_editor(editor);
    focus_editor();
}

void App::focus_editor() { focus(&editor_view_); }

void App::focus(Panel* p) {
    for (Panel* q : std::initializer_list<Panel*>{&editor_view_, &terminal_, &file_tree_, &scm_, &search_})
        q->set_focused(q == p);
}

void App::show_sidebar_view(SidebarView v) {
    sidebar_view_ = v;
    sidebar_visible_ = true;
    focus(&sidebar_panel());
}

void App::render_activity_bar(RenderContext& ctx, Rect r) {
    ctx.r->add_rect(r, theme_.tab_bg);
    const uint32_t icons[] = {0xEAF0, 0xEA6D, 0xEA68};
    float cw = font_.char_width();
    for (int i = 0; i < 3; i++) {
        Rect slot{r.x, r.y + i * ACTIVITY_BAR_W, ACTIVITY_BAR_W, ACTIVITY_BAR_W};
        bool active = sidebar_visible_ && static_cast<int>(sidebar_view_) == i;
        if (active) ctx.r->add_rect({slot.x, slot.y + 10.0f, 2.0f, slot.h - 20.0f}, theme_.accent, 1.0f);
        float bx = slot.x + (slot.w - cw) * 0.5f;
        float by = slot.y + (slot.h - font_.line_height()) * 0.5f + font_.ascender();
        ctx.r->add_char(bx, by, icons[i], active ? theme_.fg : theme_.fg_muted, font_);
        if (i == 2 && scm_.change_count() > 0) {
            std::string n = std::to_string(std::min<size_t>(scm_.change_count(), 99));
            float bw = std::max(16.0f, static_cast<float>(n.size()) * cw + 8.0f);
            Rect badge{slot.x + slot.w - bw - 6.0f, slot.y + slot.h - 22.0f, bw, 16.0f};
            ctx.r->add_rect(badge, theme_.accent, 8.0f);
            ctx.r->add_text(badge.x + (bw - static_cast<float>(n.size()) * cw) * 0.5f,
                            badge.y + (16.0f - font_.line_height()) * 0.5f + font_.ascender(), n,
                            theme_.tab_active_bg, font_);
        }
    }
}

void App::setup_search() {
    SearchPanel::Callbacks cb;
    cb.run = [this](const SearchQuery& q) { run_search(q); };
    cb.open = [this](const SearchHit& h) { open_at(h.file, h.line, h.col, h.len); };
    search_.set_callbacks(std::move(cb));
    search_.set_root(project_root_);
}

void App::run_search(const SearchQuery& q) {
    if (search_cancel_) search_cancel_->store(true);
    search_cancel_ = std::make_shared<std::atomic<bool>>(false);
    if (q.text.empty()) {
        search_.set_results({}, false);
        return;
    }
    if (vfs_->is_remote()) {
        auto hits = search_remote(q);  // runs on the SSH session; brief UI wait
        bool truncated = hits.size() >= 20000;
        search_.set_results(std::move(hits), truncated);
        return;
    }
    search_job_ = std::async(std::launch::async, [files = all_files_, q, cancel = search_cancel_] {
        auto hits = search_files(files, q, *cancel);
        request_redraw();
        return hits;
    });
}

void App::open_at(const fs::path& path, size_t line, size_t col, size_t len) {
    open_file(path);
    if (Editor* ed = current_editor()) {
        line = std::min(line, ed->buffer().line_count() - 1);
        ed->cursors().set_primary({line, col});
        ed->cursors().extend_primary({line, col + len});
        focus_editor();
        editor_view_.reveal_cursor();
    }
}

void App::setup_scm() {
    ScmPanel::Callbacks cb;
    cb.open = [this](const fs::path& p) { open_file(p); };
    cb.stage = [this](const fs::path& p) {
        if (!git_.stage(p)) set_status_message(git_.last_error(), 6.0f);
        request_git_refresh();
    };
    cb.unstage = [this](const fs::path& p) {
        if (!git_.unstage(p)) set_status_message(git_.last_error(), 6.0f);
        request_git_refresh();
    };
    cb.stage_all = [this] {
        fs::path wd = git_.workdir();
        for (auto& s : git_statuses_)
            if (s.wt_status != FileStatus::Unmodified) git_.stage(wd / s.path);
        request_git_refresh();
    };
    cb.commit = [this](std::string msg) {  // by value: the panel's buffer is cleared below
        bool staged = std::any_of(git_statuses_.begin(), git_statuses_.end(),
                                  [](auto& s) { return s.index_status != FileStatus::Unmodified; });
        if (msg.empty()) { set_status_message("Enter a commit message first"); return; }
        if (!staged) { set_status_message("Nothing staged: stage changes with + first"); return; }
        if (!git_.commit(msg)) {
            set_status_message(git_.last_error().empty() ? "Commit failed" : git_.last_error(), 6.0f);
            return;
        }
        scm_.clear_message();
        set_status_message("Committed: " + msg);
        after_head_change();
    };
    scm_.set_callbacks(std::move(cb));
}

void App::close_current_tab() {
    int idx = tab_bar_.active_index();
    if (idx < 0) return;

    // TODO: prompt to save if modified
    tab_bar_.close_tab(idx);

    if (tab_bar_.empty()) {
        new_file();
    } else {
        int new_active = tab_bar_.active_index();
        auto ed = tab_bar_.active_editor();
        editor_view_.set_editor(ed);
        status_bar_.set_editor(ed);
    }
}

void App::set_status_message(const std::string& msg, float seconds) {
    status_bar_.set_message(msg, seconds);
}

Editor* App::current_editor() {
    auto ed = tab_bar_.active_editor();
    return ed ? ed.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void App::register_commands() {
    auto add = [this](const std::string& name, const std::string& label, const std::string& keys) {
        commands_.push_back({name, label, [this, name]() { handle_command(name); }, keys});
    };

    add("save",                    "Save File",                "Ctrl+S");
    add("save_as",                 "Save As...",               "Ctrl+Shift+S");
    add("new_file",                "New File",                 "Ctrl+N");
    add("open_file",               "Open File",                "Ctrl+O");
    add("close_tab",               "Close Tab",                "Ctrl+W");
    add("next_tab",                "Next Tab",                 "Ctrl+Tab");
    add("prev_tab",                "Previous Tab",             "Ctrl+Shift+Tab");
    add("command_palette_files",   "Quick Open",               "Ctrl+P");
    add("command_palette_commands","Command Palette",           "Ctrl+Shift+P");
    add("toggle_terminal",         "Toggle Terminal",           "Ctrl+`");
    add("toggle_sidebar",          "Toggle Sidebar",            "Ctrl+B");
    add("copy",                    "Copy",                      "Ctrl+C");
    add("cut",                     "Cut",                       "Ctrl+X");
    add("paste",                   "Paste",                     "Ctrl+V");
    add("undo",                    "Undo",                      "Ctrl+Z");
    add("redo",                    "Redo",                      "Ctrl+Shift+Z");
    add("select_word",             "Select Word",               "Ctrl+D");
    add("select_line",             "Select Line",               "Ctrl+L");
    add("select_all",              "Select All",                "Ctrl+A");
    add("toggle_comment",          "Toggle Comment",            "Ctrl+/");
    add("delete_line",             "Delete Line",               "Ctrl+Shift+K");
    add("move_line_up",            "Move Line Up",              "Alt+Up");
    add("move_line_down",          "Move Line Down",            "Alt+Down");
    add("duplicate_line",          "Duplicate Line",            "Ctrl+Shift+D");
    add("goto_line",               "Go to Line",                "Ctrl+G");
    add("find",                    "Find",                      "Ctrl+F");
    add("replace",                 "Replace",                   "Ctrl+H");
    add("toggle_fullscreen",       "Toggle Fullscreen",         "F11");
    add("zoom_in",                 "Zoom In",                   "Ctrl++");
    add("zoom_out",                "Zoom Out",                  "Ctrl+-");
    add("remote_connect",          "Remote-SSH: Connect to Host...", "");
    add("remote_disconnect",       "Remote-SSH: Close Remote Connection", "");
    add("view_explorer",           "View: Explorer",            "Ctrl+Shift+E");
    add("view_search",             "Search: Find in Files",     "Ctrl+Shift+F");
    add("view_scm",                "View: Source Control",      "Ctrl+Shift+G");
    add("git_checkout",            "Git: Checkout to...",       "");
    add("toggle_blame",            "Git: Toggle Inline Blame",  "");
    add("select_theme",            "Preferences: Color Theme",  "Ctrl+K Ctrl+T");
    add("quit",                    "Quit",                      "Ctrl+Q");
}

// ---------------------------------------------------------------------------
// Project scanning
// ---------------------------------------------------------------------------

void App::scan_project_files() {
    all_files_.clear();
    static const std::vector<std::string> skip_dirs = {
        "node_modules", "__pycache__", "build", "dist", "target", "vendor", "third_party"};
    // Bounded so a huge (or remote) tree cannot stall startup.
    const size_t max_files = vfs_->is_remote() ? 5000 : 50000;
    auto deadline = Clock::now() + std::chrono::milliseconds(vfs_->is_remote() ? 2000 : 4000);
    std::function<void(const fs::path&, int)> scan = [&](const fs::path& dir, int depth) {
        if (depth > 16 || all_files_.size() >= max_files || Clock::now() > deadline) return;
        std::vector<Vfs::DirEntry> items;
        if (!vfs_->list(dir, items)) return;
        for (auto& it : items) {
            if (it.name.empty() || it.name[0] == '.') continue;
            fs::path p = dir / it.name;
            if (git_.is_open() && git_.is_ignored(p)) continue;
            if (it.is_dir) {
                if (std::find(skip_dirs.begin(), skip_dirs.end(), it.name) == skip_dirs.end()) scan(p, depth + 1);
            } else {
                all_files_.push_back(p);
                if (all_files_.size() >= max_files) return;
            }
        }
    };
    scan(project_root_, 0);
    std::sort(all_files_.begin(), all_files_.end());
}

// ---------------------------------------------------------------------------
// LSP helper
// ---------------------------------------------------------------------------

void App::setup_lsp_for_file(const fs::path& path) {
    if (vfs_->is_remote()) return;  // language servers run locally only (for now)
    if (lsp_.is_running()) {
        // Notify of new file
        auto ed = tab_bar_.active_editor();
        if (ed) {
            std::string lang = SyntaxHighlighter::detect_language(path);
            lsp_.did_open(path, lang, ed->buffer().text());
        }
        return;
    }

    // Try to start an LSP server based on file type
    std::string lang = SyntaxHighlighter::detect_language(path);
    std::string lsp_cmd;
    std::vector<std::string> lsp_args;

    if (lang == "cpp" || lang == "c") {
        lsp_cmd = "clangd";
    } else if (lang == "python") {
        lsp_cmd = "pylsp";
    } else if (lang == "rust") {
        lsp_cmd = "rust-analyzer";
    } else if (lang == "typescript" || lang == "javascript") {
        lsp_cmd = "typescript-language-server";
        lsp_args = {"--stdio"};
    } else if (lang == "go") {
        lsp_cmd = "gopls";
    }

    if (!lsp_cmd.empty()) {
        if (lsp_.start(lsp_cmd, lsp_args)) {
            lsp_.initialize(project_root_);

            auto ed = tab_bar_.active_editor();
            if (ed) {
                lsp_.did_open(path, lang, ed->buffer().text());
            }
        }
    }
}

} // namespace oh
