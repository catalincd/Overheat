#include "app.h"
#include "platform/platform.h"
#include "fonts.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <fstream>
#include <sstream>
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
    cancel_search();
    llm_.cancel();  // its worker posts to SDL, which is shut down below
    if (vk_.device()) vkDeviceWaitIdle(vk_.device());
    plugins_.shutdown();

    if (lsp_.is_running()) lsp_.stop();
    monitor_.shutdown();  // its worker posts redraw events to SDL, shut down below
    if (ssh_.is_connected()) ssh_.disconnect();
    terminal_.kill();

    font_.cleanup(vk_);
    renderer_.cleanup(vk_);
    vk_.cleanup();

    for (SDL_Cursor* c : {cursor_default_, cursor_ew_, cursor_ns_})
        if (c) SDL_DestroyCursor(c);
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

    // ---- Settings location ----
    {
        config_dir_ = platform::config_dir();
        themes_.set_config_dir(config_dir_);
    }
    if (std::string v = themes_.setting("opacity"); !v.empty())
        opacity_ = std::clamp(std::strtof(v.c_str(), nullptr), 0.5f, 1.0f);

    // A transparent window can composite per-pixel alpha (Wayland, X11 ARGB visual,
    // DWM blur-behind on Windows). It costs nothing while the swapchain stays opaque,
    // so Linux always asks for it and opacity changes apply live. On Windows SDL
    // implements it with DwmEnableBlurBehindWindow; only opt in when wanted.
    SDL_WindowFlags win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
#ifdef _WIN32
    if (opacity_ < 1.0f) win_flags |= SDL_WINDOW_TRANSPARENT;
#else
    win_flags |= SDL_WINDOW_TRANSPARENT;
#endif
    window_ = SDL_CreateWindow("Overheat", 1600, 900, win_flags);
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
    if (std::string v = themes_.setting("sidebar_width"); !v.empty()) sidebar_width_ = std::strtof(v.c_str(), nullptr);
    if (std::string v = themes_.setting("terminal_height"); !v.empty()) terminal_height_ = std::strtof(v.c_str(), nullptr);
    if (std::string v = themes_.setting("font_size"); !v.empty())
        font_size_ = std::clamp(std::strtof(v.c_str(), nullptr), 8.0f, 40.0f);
    font_family_ = themes_.setting("font_family");
    anim::enabled = themes_.setting("animations") != "false";
    show_breadcrumbs_ = themes_.setting("breadcrumbs") != "false";
    remote_monitor_ = themes_.setting("remote_monitor") != "false";
    local_monitor_ = themes_.setting("local_monitor") == "true";
    editor_view_.set_minimap(themes_.setting("minimap") != "false");
    Editor::auto_close_pairs = themes_.setting("auto_close_brackets") != "false";
    ScopeHighlight::bracket_colors = themes_.setting("bracket_colors") != "false";
    ScopeHighlight::scopes = themes_.setting("scope_highlight") != "false";
    ScopeHighlight::occurrences = themes_.setting("occurrence_underline") != "false";
    editor_view_.set_wrap(themes_.setting("word_wrap") == "true");
    editor_view2_.set_wrap(themes_.setting("word_wrap") == "true");
    sidebar_open_.snap(sidebar_visible_ ? 1.0f : 0.0f);
    terminal_open_.snap(terminal_visible_ ? 1.0f : 0.0f);
    font_.set_ligatures(themes_.setting("ligatures") != "false");
    font_chain_ = build_font_chain(font_family_);
    if (!font_.init(vk_, font_chain_, font_size_)) {
        std::fprintf(stderr, "Could not load any font\n");
        return false;
    }

    // ---- Renderers ----
    if (!renderer_.init(vk_, font_)) {
        std::fprintf(stderr, "Renderer2D::init failed\n");
        return false;
    }

    if (opacity_ < 1.0f) {
        const char* mode = apply_opacity();
        std::fprintf(stderr, "Window opacity %.2f: %s\n", opacity_, mode);
    }

    // ---- Theme ----
    {
        themes_.init(config_dir_);
        std::string choice = themes_.saved_choice();
        if (choice.empty() || !themes_.apply(choice, theme_)) themes_.apply("Overheat Dark", theme_);
    }

    // ---- Key bindings ----
    keymap_.init_defaults();

    // User key bindings overlay the defaults.
    if (fs::path user_keys = config_dir_ / "keybindings.json"; fs::exists(user_keys)) keymap_.load(user_keys);

    // ---- Commands ----
    register_commands();
    refresh_command_keys();

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
    setup_chat();
    tab_bar_.set_on_close_request([this](int i) { request_close_tab(i); });

    // ---- Terminal ----
    terminal_.spawn(local_shell(), project_root_);

    // ---- Open files from command line, or create empty editor ----
    if (!file_args.empty()) {
        for (auto& fp : file_args) {
            open_file(fp);
        }
    } else if (!restore_session()) {
        new_file();
    }

    // ---- Plugins ----
    plugins_.init(this);
    fs::path plugin_dir = config_dir_ / "plugins";
    plugins_.scan_directory(plugin_dir);

    // ---- Scan project files for command palette ----
    scan_project_files();
    command_palette_.set_commands(commands_);
    command_palette_.set_root(project_root_);
    command_palette_.set_file_list(all_files_);
    command_palette_.set_on_file_open([this](const fs::path& p) { open_file(p); });
    setup_workspaces();  // only points out a matching workspace; never opens one
    update_window_title();

    // ---- Text input ----
    SDL_StartTextInput(window_);

    if (!script_path.empty() && !script_.load(script_path, window_))
        return false;
    scripted_ = !script_path.empty();

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::run() {
    auto last_time = Clock::now();
    constexpr float FRAME_BUDGET_MS = 1000.0f / 60.0f;

    bool dirty = true;
    TimePoint last_render{};
    // Plain pointer motion and key releases change nothing on screen; skipping them
    // keeps a 1000 Hz mouse from forcing 1000 redraws per second.
    auto affects_frame = [this](const SDL_Event& ev) {
        if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            if (ev.motion.state & SDL_BUTTON_LMASK) return true;
            // Hover highlights live in the chrome (activity bar, sidebar, tab strip).
            Layout l = compute_layout();
            float x = ev.motion.x, y = ev.motion.y;
            bool in_chrome = l.activity_bar.contains(x, y) || l.sidebar.contains(x, y) || l.tab_bar.contains(x, y) ||
                             l.terminal_header.contains(x, y) || (welcome_visible_ && l.editor.contains(x, y));
            bool changed = in_chrome || hover_was_chrome_;  // one more frame to clear the highlight
            hover_was_chrome_ = in_chrome;
            return changed;
        }
        return ev.type != SDL_EVENT_KEY_UP;
    };

    // While the user drags a window edge on Windows, the OS runs its own modal loop and
    // this one is blocked. Without new frames, DWM stretches the last one to the new
    // size, so the text appears to grow or shrink. SDL delivers live-resize EXPOSED
    // events to event watches on the main thread from inside that loop; redraw there.
    SDL_AddEventWatch(&App::live_resize_watch, this);

    while (running_) {
        // Event-driven: sleep until input, terminal output (SDL_EVENT_USER), the next
        // cursor blink, or the next scripted step. At most one frame per display refresh.
        auto now = Clock::now();
        auto frame_interval = std::chrono::microseconds(static_cast<int64_t>(1e6 / refresh_rate()));
        int timeout = 500;
        if (script_.active()) timeout = std::min(timeout, script_.ms_until_next(now));
        if (dirty) {
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(last_render + frame_interval - now);
            timeout = std::max(0, static_cast<int>(wait.count()));
        }
        SDL_Event e;
        if (pending_probes_.empty()) {
            if (SDL_WaitEventTimeout(&e, timeout)) dirty |= affects_frame(e), handle_event(e);
            else dirty = true;  // timer wake: blink, polling, scripted step
        }
        while (SDL_PollEvent(&e)) {
            dirty |= affects_frame(e);
            handle_event(e);
        }
        if (!running_) break;
        if (pending_probes_.empty() && (!dirty || Clock::now() < last_render + frame_interval)) continue;
        dirty = false;
        last_render = Clock::now();

        auto frame_start = Clock::now();
        float dt = std::chrono::duration<float>(frame_start - last_time).count();
        last_time = frame_start;

        anim::dt = std::min(dt, 0.1f);  // a long idle gap must not make animations jump
        anim::active = false;
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

        rendering_ = true;
        render();
        rendering_ = false;
        frame_count_++;
        if (anim::active) dirty = true;  // keep drawing until animations settle

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
    SDL_RemoveEventWatch(&App::live_resize_watch, this);
    autosave_workspace();
    save_session();
    print_frame_stats();
}

bool SDLCALL App::live_resize_watch(void* userdata, SDL_Event* e) {
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1)
        static_cast<App*>(userdata)->live_resize_frame();
    return true;
}

void App::live_resize_frame() {
    if (rendering_ || !first_paint_done_) return;
    rendering_ = true;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    VkExtent2D ext = vk_.extent();
    if (w > 0 && h > 0 && (static_cast<uint32_t>(w) != ext.width || static_cast<uint32_t>(h) != ext.height))
        vk_.recreate_swapchain();
    anim::dt = 0.0f;
    anim::active = false;
    update(0.0f);
    render();
    rendering_ = false;
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

float App::refresh_rate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode && mode->refresh_rate > 1.0f ? mode->refresh_rate : 60.0f;
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
                     ed->buffer().file_path().filename().string().c_str(), p.line, p.col, sel.size(),
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
        if (scripted_) quit();  // test runs never block on prompts
        else confirm_discard("Quit", [this] { quit(); });
        break;

    case SDL_EVENT_CLIPBOARD_UPDATE:
        if (!e.clipboard.owner) own_clipboard_.clear();
        break;

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:  // also DPI changes between monitors
        vk_.recreate_swapchain();
        break;

    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode kc = e.key.key;
        KeyMod mods = sdl_mods(e.key.mod);

        // Keybinding editor capture: the next real chord becomes the binding.
        if (!capture_command_.empty() && !command_palette_.is_visible()) {
            bool modifier_only = kc == SDLK_LCTRL || kc == SDLK_RCTRL || kc == SDLK_LSHIFT || kc == SDLK_RSHIFT ||
                                 kc == SDLK_LALT || kc == SDLK_RALT || kc == SDLK_LGUI || kc == SDLK_RGUI;
            if (modifier_only) break;
            if (kc == SDLK_ESCAPE) {
                set_status_message("Shortcut unchanged");
            } else {
                keymap_.rebind(capture_command_, kc, mods);
                keymap_.save(config_dir_ / "keybindings.json");
                refresh_command_keys();
                set_status_message(KeyMap::chord_to_string(kc, mods) + " now runs " + capture_command_);
            }
            capture_command_.clear();
            break;
        }
        // Command palette intercepts keys when visible
        if (command_palette_.is_visible()) {
            KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
            if (command_palette_.handle_key(ke)) break;
        }
        if (kc == SDLK_ESCAPE && monitor_bar_.detail_open() && !command_palette_.is_visible()) {
            monitor_bar_.set_detail(false);
            break;
        }

        // The settings card is modal.
        if (settings_page_.visible()) {
            KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
            settings_page_.handle_key(ke);
            break;
        }
        // Sidebar text inputs (commit message, search) take typing keys before global bindings.
        if (sidebar_visible_ && !command_palette_.is_visible()) {
            KeyEvent ke{static_cast<uint32_t>(kc), mods, true};
            if (sidebar_view_ == SidebarView::SourceControl && scm_.input_focused() && scm_.handle_key(ke)) break;
            if (sidebar_view_ == SidebarView::Search && search_.input_focused() && search_.handle_key(ke)) break;
            if (sidebar_view_ == SidebarView::Chat && chat_.input_focused() && chat_.handle_key(ke)) break;
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
        // The Ctrl+Tab switcher commits when Ctrl is released.
        if (switcher_open_ && (e.key.key == SDLK_LCTRL || e.key.key == SDLK_RCTRL)) {
            if (command_palette_.is_visible()) command_palette_.accept();
            switcher_open_ = false;
            break;
        }
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
        } else if (chat_.input_focused()) {
            chat_.handle_text(text);
        } else if (terminal_.focused() && terminal_visible_) {
            terminal_.handle_text(text);
        } else {
            editor_view_.handle_text(text);
            lsp_maybe_complete(text, false);
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

        // Clicking into the other editor group makes it the active one.
        if (split_ && !command_palette_.is_visible() &&
            (layout.tab_bar2.contains(mx, my) || layout.editor2.contains(mx, my) || layout.breadcrumbs2.contains(mx, my))) {
            focus_group(!active_right_);
            layout = compute_layout();
        }
        // Determine which panel was clicked and set focus
        if (!command_palette_.is_visible() && e.button.button == SDL_BUTTON_LEFT &&
            (splitter_drag_ = splitter_at(mx, my)) != Splitter::None) {
            break;
        }
        // The monitor detail card: clicks inside stay there, clicks elsewhere close it.
        if (monitor_bar_.detail_open() && !command_palette_.is_visible() && !settings_page_.visible()) {
            if (monitor_bar_.detail_rect().contains(mx, my)) break;
            if (!layout.monitor.contains(mx, my)) monitor_bar_.set_detail(false);
        }
        if (command_palette_.is_visible()) {
            command_palette_.handle_mouse(me);
        } else if (settings_page_.visible()) {
            settings_page_.handle_mouse(me);
        } else if (layout.monitor.contains(mx, my)) {
            monitor_bar_.handle_mouse(me);
            update_monitor();  // detail samples start right away
        } else if (layout.activity_bar.contains(mx, my)) {
            int slot = static_cast<int>((my - layout.activity_bar.y) / ACTIVITY_BAR_W);
            SidebarView v = static_cast<SidebarView>(std::clamp(slot, 0, 3));
            if (slot <= 3) {
                if (sidebar_visible_ && v == sidebar_view_) sidebar_visible_ = false;
                else show_sidebar_view(v);
            }
        } else if (sidebar_visible_ && layout.sidebar.contains(mx, my)) {
            focus(&sidebar_panel());
            sidebar_panel().handle_mouse(me);
        } else if (layout.terminal_header.contains(mx, my)) {
            if (term_close_btn_.contains(mx, my)) terminal_visible_ = false;
            else if (term_restart_btn_.contains(mx, my)) restart_terminal();
            else focus(&terminal_);
        } else if (terminal_visible_ && layout.terminal.contains(mx, my)) {
            focus(&terminal_);
            terminal_.handle_mouse(me);
        } else if (layout.tab_bar.contains(mx, my)) {
            tab_bar_.handle_mouse(me);
        } else if (layout.editor.contains(mx, my) && !welcome_click(mx, my)) {
            focus(&editor_view_);
            editor_view_.handle_mouse(me);
        }
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (splitter_drag_ == Splitter::Groups) {
            splitter_drag_ = Splitter::None;
            break;
        }
        if (splitter_drag_ != Splitter::None) {
            themes_.set_setting(splitter_drag_ == Splitter::Sidebar ? "sidebar_width" : "terminal_height",
                                std::to_string(static_cast<int>(splitter_drag_ == Splitter::Sidebar
                                                                    ? sidebar_width_ : terminal_height_)));
            splitter_drag_ = Splitter::None;
            update_cursor_shape(splitter_at(e.button.x, e.button.y));
            break;
        }
        MouseEvent me{e.button.x, e.button.y, static_cast<int>(e.button.button),
                      false, false, 0, 0};
        if (tab_bar_.dragging()) {
            tab_bar_.handle_mouse(me);
            break;
        }
        editor_view_.handle_mouse(me);
        terminal_.handle_mouse(me);
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        float mx = e.wheel.mouse_x;
        float my = e.wheel.mouse_y;
        Layout layout = compute_layout();

        MouseEvent me{mx, my, 0, false, false, e.wheel.x, e.wheel.y};

        if (settings_page_.visible()) {
            settings_page_.handle_mouse(me);
            break;
        }
        if ((SDL_GetModState() & SDL_KMOD_CTRL) && layout.editor.contains(mx, my)) {
            set_font_size(font_size_ + (e.wheel.y > 0 ? 1.0f : -1.0f));
            break;
        }
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
        if (splitter_drag_ != Splitter::None) {
            Layout l = compute_layout();
            if (splitter_drag_ == Splitter::Groups) {
                float left = std::min(l.editor.x, l.editor2.x);
                float total = l.editor.w + l.editor2.w;
                split_ratio_ = std::clamp((e.motion.x - left) / total, 0.15f, 0.85f);
            } else if (splitter_drag_ == Splitter::Sidebar)
                sidebar_width_ = std::clamp(e.motion.x - l.sidebar.x, 140.0f, l.sidebar.w + l.editor.w - 200.0f);
            else
                terminal_height_ = std::clamp(l.terminal.y + l.terminal.h - e.motion.y, 60.0f,
                                              l.terminal.h + l.terminal_header.h + l.editor.h - 80.0f);
            request_redraw();
            break;
        }
        if (!(e.motion.state & SDL_BUTTON_LMASK) && !command_palette_.is_visible()) {
            Splitter s = splitter_at(e.motion.x, e.motion.y);
            if (s != splitter_hover_) {
                update_cursor_shape(s);
                request_redraw();  // hover highlight changed
            }
        }
        if (e.motion.state & SDL_BUTTON_LMASK) {
            MouseEvent me{e.motion.x, e.motion.y, 1, true, false, 0, 0, true};
            if (tab_bar_.dragging()) tab_bar_.handle_mouse(me);
            else if (terminal_.focused()) terminal_.handle_mouse(me);
            else editor_view_.handle_mouse(me);
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
    if (cmd.rfind("workspace_", 0) == 0 || cmd == "open_folder") {
        workspace_command(cmd);
    } else if (cmd == "save") {
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
    } else if (cmd == "quick_switch" || cmd == "quick_switch_back") {
        if (switcher_open_ && command_palette_.is_visible()) {
            command_palette_.move_selection(cmd == "quick_switch" ? 1 : -1);
        } else if (tab_bar_.tabs().size() > 1) {
            auto order = tab_bar_.mru_order();
            std::vector<std::string> items;
            std::vector<std::shared_ptr<Editor>> eds;
            for (int i : order) {
                auto& ed = tab_bar_.tabs()[static_cast<size_t>(i)].editor;
                fs::path p = ed->buffer().file_path();
                std::string rel = p.empty() ? "" : p.parent_path().lexically_relative(project_root_).string();
                items.push_back((p.empty() ? std::string("Untitled") : p.filename().string()) +
                                (rel.empty() || rel == "." ? "" : "  " + rel));
                eds.push_back(ed);
            }
            CommandPalette::PickOptions opts;
            opts.placeholder = "Open editors (release Ctrl to switch)";
            opts.items = items;
            opts.initial = cmd == "quick_switch" ? 1 : static_cast<int>(items.size()) - 1;
            opts.on_pick = [this, eds](int i, const std::string&) {
                switcher_open_ = false;
                if (i < 0) return;
                for (size_t t = 0; t < tab_bar_.tabs().size(); t++)
                    if (tab_bar_.tabs()[t].editor == eds[static_cast<size_t>(i)]) tab_bar_.set_active(static_cast<int>(t));
                focus_editor();
            };
            opts.on_cancel = [this] { switcher_open_ = false; };
            command_palette_.pick(std::move(opts));
            switcher_open_ = true;
        }
    } else if (cmd == "next_tab") {
        tab_bar_.next_tab();
    } else if (cmd == "prev_tab") {
        tab_bar_.prev_tab();
    } else if (cmd == "command_palette_files") {
        command_palette_.show(CommandPalette::Mode::Files);
    } else if (cmd == "command_palette_commands") {
        command_palette_.show(CommandPalette::Mode::Commands);
    } else if (cmd == "toggle_terminal") {
        // Like VS Code: show + focus; hide when it already has focus; else just focus it.
        if (terminal_visible_ && !terminal_.focused()) {
            focus(&terminal_);
        } else {
            terminal_visible_ = !terminal_visible_;
            if (terminal_visible_) {
                if (!terminal_.is_alive()) restart_terminal();
                focus(&terminal_);
            } else if (terminal_.focused()) {
                focus(&editor_view_);
            }
        }
    } else if (cmd == "toggle_sidebar") {
        sidebar_visible_ = !sidebar_visible_;
    } else if (cmd == "copy" || cmd == "cut") {
        if (terminal_.focused() && terminal_visible_) {
            if (terminal_.has_selection()) {
                own_clipboard_ = terminal_.selection_text();
                SDL_SetClipboardText(own_clipboard_.c_str());
                clipboard_line_text_.clear();
            }
            return;
        }
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
        if (Editor* ed = current_editor()) {
            ed->add_next_occurrence();
            editor_view_.reveal_cursor();
        }
    } else if (cmd == "add_cursor_above" || cmd == "add_cursor_below") {
        if (Editor* ed = current_editor()) ed->add_cursor_vertical(cmd == "add_cursor_above" ? -1 : 1);
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
        set_font_size(font_size_ + 1.0f);
    } else if (cmd == "zoom_out") {
        set_font_size(font_size_ - 1.0f);
    } else if (cmd == "zoom_reset") {
        set_font_size(15.0f);
    } else if (cmd == "remote_connect") {
        remote_connect_prompt();
    } else if (cmd == "remote_disconnect") {
        confirm_discard("Disconnect", [this] { remote_disconnect(); });
    } else if (cmd == "view_explorer") {
        show_sidebar_view(SidebarView::Explorer);
    } else if (cmd == "trigger_suggest") {
        lsp_maybe_complete("", true);
    } else if (cmd == "goto_definition") {
        lsp_goto_definition();
    } else if (cmd == "view_search") {
        show_sidebar_view(SidebarView::Search);
        search_.focus_input();
    } else if (cmd == "view_scm") {
        show_sidebar_view(SidebarView::SourceControl);
    } else if (cmd == "git_checkout") {
        git_checkout();
    } else if (cmd == "toggle_blame") {
        editor_view_.toggle_blame();
    } else if (cmd == "view_chat") {
        show_sidebar_view(SidebarView::Chat);
        chat_.focus_input();
    } else if (cmd == "ai_new_chat") {
        llm_.cancel();
        chat_.clear();
        show_sidebar_view(SidebarView::Chat);
    } else if (cmd == "ai_backend") {
        pick_llm_backend();
    } else if (cmd == "ai_ask_selection") {
        if (Editor* ed = current_editor()) {
            bool line_mode = false;
            std::string code = ed->copy_text(line_mode);
            std::string lang = ed->syntax().language_id();
            std::string name = ed->buffer().file_path().filename().string();
            chat_.set_draft("In " + (name.empty() ? std::string("this file") : name) + ":\n```" + lang + "\n" +
                            code + (code.empty() || code.back() == '\n' ? "" : "\n") + "```\n");
            show_sidebar_view(SidebarView::Chat);
            chat_.focus_input();
        }
    } else if (cmd == "open_settings") {
        open_settings();
    } else if (cmd == "keybindings") {
        open_keybindings();
    } else if (cmd == "split_editor") {
        split_editor();
    } else if (cmd == "focus_group_1" || cmd == "focus_group_2") {
        focus_group(cmd == "focus_group_2");
    } else if (cmd == "toggle_word_wrap") {
        bool on = !editor_view_.wrap();
        editor_view_.set_wrap(on);
        editor_view2_.set_wrap(on);
        themes_.set_setting("word_wrap", on ? "true" : "false");
        set_status_message(std::string("Word wrap ") + (on ? "on" : "off"));
    } else if (cmd == "toggle_minimap") {
        editor_view_.set_minimap(!editor_view_.minimap());
        themes_.set_setting("minimap", editor_view_.minimap() ? "true" : "false");
    } else if (cmd == "toggle_breadcrumbs") {
        show_breadcrumbs_ = !show_breadcrumbs_;
        themes_.set_setting("breadcrumbs", show_breadcrumbs_ ? "true" : "false");
    } else if (cmd == "toggle_remote_monitor") {
        remote_monitor_ = !remote_monitor_;
        themes_.set_setting("remote_monitor", remote_monitor_ ? "true" : "false");
        set_status_message(std::string("Remote monitor ") + (remote_monitor_ ? "on" : "off"));
    } else if (cmd == "toggle_local_monitor") {
        local_monitor_ = !local_monitor_;
        themes_.set_setting("local_monitor", local_monitor_ ? "true" : "false");
        set_status_message(std::string("Local system monitor ") + (local_monitor_ ? "on" : "off"));
    } else if (cmd == "monitor_details") {
        if (!monitor_visible()) {
            if (remote_active_) remote_monitor_ = true;
            else local_monitor_ = true;
            update_monitor();
        }
        monitor_bar_.set_detail(!monitor_bar_.detail_open());
    } else if (cmd == "toggle_animations") {
        anim::enabled = !anim::enabled;
        themes_.set_setting("animations", anim::enabled ? "true" : "false");
        set_status_message(std::string("Animations ") + (anim::enabled ? "on" : "off"));
    } else if (cmd == "select_font") {
        select_font();
    } else if (cmd == "toggle_ligatures") {
        font_.set_ligatures(!font_.ligatures());
        themes_.set_setting("ligatures", font_.ligatures() ? "true" : "false");
        set_status_message(std::string("Ligatures ") + (font_.ligatures() ? "on" : "off"));
    } else if (cmd == "select_theme") {
        select_theme();
    } else if (cmd == "quit") {
        confirm_discard("Quit", [this] { quit(); });
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
    if (monitor_visible()) {
        content_h -= MonitorBar::HEIGHT;
        l.monitor = {0, content_h, fw, MonitorBar::HEIGHT};
    }

    // Activity bar (always visible), then the sidebar view.
    float ab_w = ACTIVITY_BAR_W;
    l.activity_bar = {0, 0, ab_w, content_h};
    float sb_w = std::floor(std::min(sidebar_width_, (fw - ab_w) * 0.5f) * sidebar_open_.value);
    l.sidebar = {ab_w, 0, sb_w, content_h};

    float right_x = ab_w + sb_w;
    float right_w = fw - right_x;

    // Tab bar at the top of the editor region, breadcrumbs under it; two columns when split.
    float tab_h = 36.0f;
    float crumbs_h = show_breadcrumbs_ ? 24.0f : 0.0f;
    float left_w = split_ ? std::floor(right_w * split_ratio_) : right_w;
    float act_x = right_x, act_w = left_w, oth_x = right_x + left_w, oth_w = right_w - left_w;
    if (split_ && active_right_) {
        std::swap(act_x, oth_x);
        std::swap(act_w, oth_w);
    }
    l.tab_bar = {act_x, 0, act_w, tab_h};
    l.breadcrumbs = {act_x, tab_h, act_w, crumbs_h};
    if (split_) {
        l.tab_bar2 = {oth_x, 0, oth_w, tab_h};
        l.breadcrumbs2 = {oth_x, tab_h, oth_w, crumbs_h};
        l.group_split_x = right_x + left_w;
    }
    tab_h += crumbs_h;

    // Terminal: at bottom of right region (above status bar)
    float term_h = std::floor(std::min(terminal_height_, (content_h - tab_h) * 0.8f) * terminal_open_.value);
    // A header strip sits on top of the terminal.
    float header_h = std::min(term_h, 28.0f);
    l.terminal_header = {right_x, content_h - term_h, right_w, header_h};
    l.terminal = {right_x, content_h - term_h + header_h, right_w, term_h - header_h};

    // Editor: remaining space
    float editor_y = tab_h;
    float editor_h = content_h - tab_h - term_h;
    if (editor_h < 0) editor_h = 0;
    l.editor = {act_x, editor_y, act_w, editor_h};
    if (split_) l.editor2 = {oth_x, editor_y, oth_w, editor_h};

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
    std::vector<std::string> names, details;
    for (auto& info : themes_.list()) {
        names.push_back(info.name);
        details.push_back(std::string(info.user ? "user, " : "") + (info.dark ? "dark" : "light"));
    }
    std::string original = themes_.current();
    int initial = 0;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == original) initial = static_cast<int>(i);
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select color theme (live preview)";
    opts.items = names;
    opts.details = std::move(details);
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
    watch_timer_ += dt;
    if (watch_timer_ > 1.0f) {
        watch_timer_ = 0;
        watch_open_files();
    }
    // Panels slide open/closed instead of popping.
    sidebar_open_.follow(sidebar_visible_ ? 1.0f : 0.0f, anim::NORMAL, 0.002f);
    terminal_open_.follow(terminal_visible_ ? 1.0f : 0.0f, anim::NORMAL, 0.002f);
    lsp_update();
    poll_remote_connect();
    update_monitor();
    search_.tick(dt);
    poll_search();
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

    if (split_) {
        editor_view2_.set_editor(tab_bar2_.active_editor());
        editor_view2_.update(dt);
    }

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
    ctx.dt    = anim::dt;
    SDL_GetMouseState(&ctx.mouse_x, &ctx.mouse_y);

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
    renderer_.push_clip(layout.breadcrumbs);
    render_breadcrumbs(ctx, layout.breadcrumbs, current_editor());
    renderer_.pop_clip();
    clipped(editor_view_, layout.editor);
    renderer_.push_clip(layout.editor);
    render_welcome(ctx, layout.editor);
    renderer_.pop_clip();
    if (split_) {
        clipped(tab_bar2_, layout.tab_bar2);
        auto other = tab_bar2_.active_editor();
        renderer_.push_clip(layout.breadcrumbs2);
        render_breadcrumbs(ctx, layout.breadcrumbs2, other.get());
        renderer_.pop_clip();
        // Same document in both groups: draw the other view with its own cursors/scroll.
        bool shared = other && other == tab_bar_.active_editor();
        ViewState saved;
        if (shared) {
            if (!other_state_) other_state_ = ViewState{other->cursors(), other->scroll_x(), other->scroll_y()};
            saved = {other->cursors(), other->scroll_x(), other->scroll_y()};
            other->cursors() = other_state_->cursors;
            other->set_scroll(other_state_->sx, other_state_->sy);
        }
        clipped(editor_view2_, layout.editor2);
        if (shared) {
            other->cursors() = saved.cursors;
            other->set_scroll(saved.sx, saved.sy);
        }
        Color div = theme_.border;
        renderer_.add_rect({layout.group_split_x, 0, 1.0f, layout.editor.y + layout.editor.h}, div);
    }
    if (layout.terminal_header.h > 0) {
        renderer_.push_clip(layout.terminal_header);
        render_terminal_header(ctx, layout.terminal_header);
        renderer_.pop_clip();
    }
    if (layout.terminal.h > 0) clipped(terminal_, layout.terminal);
    if (layout.monitor.h > 0) clipped(monitor_bar_, layout.monitor);
    clipped(status_bar_, layout.status_bar);
    if (Splitter sp = splitter_drag_ != Splitter::None ? splitter_drag_ : splitter_hover_; sp != Splitter::None) {
        Color c = theme_.accent;
        if (sp == Splitter::Groups)
            renderer_.add_rect({layout.group_split_x - 1.0f, 0, 2.0f, layout.editor.y + layout.editor.h}, c);
        else if (sp == Splitter::Sidebar)
            renderer_.add_rect({layout.sidebar.x + layout.sidebar.w - 1.0f, 0, 2.0f, layout.sidebar.h}, c);
        else
            renderer_.add_rect({layout.terminal_header.x, layout.terminal_header.y - 1.0f, layout.terminal.w, 2.0f}, c);
    }
    if (layout.monitor.h > 0)
        monitor_bar_.render_detail(ctx, {0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)});
    if (settings_page_.visible())
        clipped(settings_page_, {0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)});
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
        std::error_code ec;
        if (auto t = fs::last_write_time(ed->buffer().file_path(), ec); !ec)
            disk_mtimes_[ed->buffer().file_path().string()] = t;  // our own write is not an external change
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

App::Splitter App::splitter_at(float x, float y) const {
    Layout l = compute_layout();
    constexpr float GRAB = 4.0f;
    if (sidebar_visible_ && std::abs(x - (l.sidebar.x + l.sidebar.w)) <= GRAB && y < l.sidebar.h)
        return Splitter::Sidebar;
    if (terminal_visible_ && std::abs(y - l.terminal_header.y) <= GRAB && x >= l.terminal.x)
        return Splitter::Terminal;
    if (split_ && std::abs(x - l.group_split_x) <= GRAB && y < l.editor.y + l.editor.h)
        return Splitter::Groups;
    return Splitter::None;
}

void App::update_cursor_shape(Splitter s) {
    splitter_hover_ = s;
    if (!cursor_default_) {
        cursor_default_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        cursor_ew_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
        cursor_ns_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    }
    SDL_SetCursor(s == Splitter::Sidebar || s == Splitter::Groups ? cursor_ew_
                  : s == Splitter::Terminal ? cursor_ns_ : cursor_default_);
}

std::vector<std::string> App::build_font_chain(const std::string& family) const {
    // Chosen family first; then fonts that cover icons (Nerd Font) and wide Unicode.
    std::vector<std::string> chain;
    if (!family.empty())
        if (std::string f = font_file_for_family(family); !f.empty()) chain.push_back(f);
    std::error_code ec;
    for (const std::string& fp : platform::default_font_files()) {
        if (!fs::exists(fp, ec)) continue;
        std::string canon = fs::weakly_canonical(fp, ec).string();
        bool dup = false;
        for (const auto& c : chain) dup |= fs::weakly_canonical(c, ec).string() == canon;
        // One copy of the bundled JetBrains Mono Nerd Font is enough.
        for (const auto& c : chain) dup |= fs::path(c).filename() == fs::path(fp).filename();
        if (!dup) chain.push_back(fp);
    }
    return chain;
}

bool App::reload_font(const std::string& family, float size) {
    vkDeviceWaitIdle(vk_.device());  // frames in flight still sample the old atlas
    font_.cleanup(vk_);
    auto chain = build_font_chain(family);
    if (!font_.init(vk_, chain, size)) {
        font_.init(vk_, font_chain_, font_size_);
        renderer_.rebind_font(vk_, font_);
        return false;
    }
    font_chain_ = std::move(chain);
    font_family_ = family;
    font_size_ = size;
    renderer_.rebind_font(vk_, font_);
    return true;
}

void App::select_font() {
    auto families = list_monospace_families();
    std::vector<std::string> names, details;
    int initial = 0;
    for (auto& f : families) {
        if (f.name == font_family_) initial = static_cast<int>(names.size());
        names.push_back(f.name);
        details.push_back(f.bundled ? "bundled" : "");
    }
    std::string current = font_family_.empty() ? "JetBrainsMono Nerd Font Mono" : font_family_;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == current) initial = static_cast<int>(i);
    std::string original = font_family_;
    CommandPalette::PickOptions opts;
    opts.placeholder = "Select font family (live preview)";
    opts.items = names;
    opts.details = std::move(details);
    opts.initial = initial;
    opts.on_preview = [this, names, current](int i) {
        const std::string& name = names[static_cast<size_t>(i)];
        if (name != (font_family_.empty() ? current : font_family_)) reload_font(name, font_size_);
    };
    opts.on_cancel = [this, original] { reload_font(original, font_size_); };
    opts.on_pick = [this, names](int i, const std::string&) {
        if (i < 0) return;
        const std::string& name = names[static_cast<size_t>(i)];
        if (name != font_family_) reload_font(name, font_size_);
        themes_.set_setting("font_family", "\"" + name + "\"");
        set_status_message("Font: " + name);
    };
    command_palette_.pick(std::move(opts));
}

void App::set_font_size(float size) {
    size = std::clamp(size, 8.0f, 40.0f);
    if (size == font_size_ || !reload_font(font_family_, size)) return;
    themes_.set_setting("font_size", std::to_string(static_cast<int>(size)));
    set_status_message("Font size " + std::to_string(static_cast<int>(size)));
}

const char* App::apply_opacity() {
    bool want = opacity_ < 0.999f;
    bool per_pixel = vk_.set_translucent(want && renderer_.can_translucent());
    renderer_.set_window_opacity(per_pixel ? opacity_ : 1.0f);
    // Fallback: the whole window, text included, through the window manager
    // (_NET_WM_WINDOW_OPACITY on X11, a layered window on Windows). Wayland has none.
    // Windows: per-pixel alpha needs DWM blur-behind (SDL_WINDOW_TRANSPARENT) and a
    // driver whose Vulkan surface offers PRE/POST_MULTIPLIED; many only offer OPAQUE.
    // Terminals that get it everywhere present through DXGI/DirectComposition
    // (Windows Terminal, Zed) or WGL (Alacritty, WezTerm's OpenGL front end).
    bool whole = want && !per_pixel && SDL_SetWindowOpacity(window_, opacity_);
    if (window_opacity_set_ && !whole) SDL_SetWindowOpacity(window_, 1.0f);
    window_opacity_set_ = whole;
    if (!want) return "opaque";
    if (per_pixel) return "translucent background";
    if (whole) return "whole-window opacity (per-pixel alpha unsupported here)";
    return "unsupported by this window system";
}

void App::set_opacity(float o) {
    o = std::clamp(std::round(o * 20.0f) / 20.0f, 0.5f, 1.0f);
    if (o == opacity_) return;
    opacity_ = o;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", o);
    themes_.set_setting("opacity", buf);
    const char* mode = apply_opacity();
    set_status_message("Window opacity " + std::to_string(static_cast<int>(std::lround(o * 100))) + "%: " + mode);
}

void App::focus(Panel* p) {
    for (Panel* q : std::initializer_list<Panel*>{&editor_view_, &terminal_, &file_tree_, &scm_, &search_, &chat_})
        q->set_focused(q == p);
}

void App::show_sidebar_view(SidebarView v) {
    sidebar_view_ = v;
    sidebar_visible_ = true;
    focus(&sidebar_panel());
}

fs::path App::session_file() const {
    // One session per project folder, keyed by a hash of its path.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : project_root_.string()) h = (h ^ c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.session", static_cast<unsigned long long>(h));
    return config_dir_ / "sessions" / name;
}

void App::save_session() {
    if (remote_active_) return;  // remote sessions are not restored (yet)
    if (workspace_) return;      // an open workspace keeps its session in its own file
    std::error_code ec;
    fs::create_directories(session_file().parent_path(), ec);
    std::ofstream out(session_file());
    out << "root\t" << project_root_.string() << "\n";
    out << "layout\t" << sidebar_visible_ << "\t" << terminal_visible_ << "\t"
        << static_cast<int>(sidebar_view_) << "\n";
    int active = tab_bar_.active_index();
    for (size_t i = 0; i < tab_bar_.tabs().size(); i++) {
        auto& ed = tab_bar_.tabs()[i].editor;
        if (!ed || ed->buffer().file_path().empty()) continue;
        Position p = ed->cursors().primary_pos();
        out << "tab\t" << (static_cast<int>(i) == active) << "\t" << p.line << "\t" << p.col << "\t"
            << ed->scroll_y() << "\t" << ed->buffer().file_path().string() << "\n";
    }
}

bool App::restore_session() {
    std::ifstream in(session_file());
    if (!in) return false;
    std::shared_ptr<Editor> active_ed;
    bool any = false;
    for (std::string line; std::getline(in, line);) {
        std::istringstream ss(line);
        std::string kind;
        std::getline(ss, kind, '\t');
        if (kind == "layout") {
            int sv = 1, tv = 1, view = 0;
            ss >> sv >> tv >> view;
            sidebar_visible_ = sv != 0;
            terminal_visible_ = tv != 0;
            sidebar_view_ = static_cast<SidebarView>(std::clamp(view, 0, 3));
            sidebar_open_.snap(sidebar_visible_ ? 1.0f : 0.0f);
            terminal_open_.snap(terminal_visible_ ? 1.0f : 0.0f);
        } else if (kind == "tab") {
            int active = 0;
            size_t l = 0, c = 0;
            float sy = 0;
            ss >> active >> l >> c >> sy;
            ss.ignore(1);
            std::string path;
            std::getline(ss, path);
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;
            open_file(path);
            if (Editor* ed = current_editor()) {
                l = std::min(l, ed->buffer().line_count() - 1);
                ed->cursors().set_primary({l, std::min(c, ed->buffer().line_length(l))});
                ed->set_scroll(0, sy);
                any = true;
                if (active) active_ed = tab_bar_.active_editor();
            }
        }
    }
    if (active_ed)
        for (size_t i = 0; i < tab_bar_.tabs().size(); i++)
            if (tab_bar_.tabs()[i].editor == active_ed) tab_bar_.set_active(static_cast<int>(i));
    return any;
}

void App::split_editor() {
    auto ed = tab_bar_.active_editor();
    if (!ed) return;
    if (split_) {
        focus_group(!active_right_);
        return;
    }
    split_ = true;
    tab_bar2_ = TabBar{};
    tab_bar2_.set_on_close_request([this](int i) { request_close_tab(i); });
    tab_bar2_.add_tab(ed);
    other_state_ = ViewState{ed->cursors(), ed->scroll_x(), ed->scroll_y()};
    focus_group(true);
}

void App::focus_group(bool right) {
    if (!split_ || right == active_right_) return;
    auto a = tab_bar_.active_editor(), b = tab_bar2_.active_editor();
    if (a && a == b) {  // same document: exchange per-view cursors and scroll
        ViewState current{a->cursors(), a->scroll_x(), a->scroll_y()};
        if (other_state_) {
            a->cursors() = other_state_->cursors;
            a->set_scroll(other_state_->sx, other_state_->sy);
        }
        other_state_ = current;
    }
    std::swap(tab_bar_, tab_bar2_);
    std::swap(editor_view_, editor_view2_);
    active_right_ = right;
    editor_view2_.set_focused(false);
    focus_editor();
}

void App::unsplit_if_empty() {
    if (!split_) return;
    if (tab_bar_.empty()) {  // keep the surviving group as the only one
        std::swap(tab_bar_, tab_bar2_);
        std::swap(editor_view_, editor_view2_);
    }
    if (!tab_bar2_.empty() && !tab_bar_.empty()) return;
    split_ = false;
    active_right_ = false;
    other_state_.reset();
    tab_bar2_ = TabBar{};
    auto ed = tab_bar_.active_editor();
    editor_view_.set_editor(ed);
    status_bar_.set_editor(ed);
}

void App::refresh_command_keys() {
    for (auto& c : commands_)
        if (std::string k = keymap_.binding_for(c.name); !k.empty()) c.keybinding = k;
    command_palette_.set_commands(commands_);
}

void App::open_settings() {
    using Row = SettingsPage::Row;
    auto toggle = [this](std::string label, std::string desc, std::function<bool()> on, std::string cmd) {
        return Row{Row::Toggle, std::move(label), std::move(desc), nullptr, std::move(on),
                   [this, cmd](int) { handle_command(cmd); }};
    };
    std::vector<Row> rows;
    rows.push_back({Row::Choice, "Color theme", "Built-in and ~/.config/overheat/themes/*.toml",
                    [this] { return themes_.current(); }, nullptr, [this](int) { select_theme(); }});
    rows.push_back({Row::Choice, "Font family", "Any installed monospace font; icons fall back to Nerd Fonts",
                    [this] { return font_family_.empty() ? std::string("JetBrainsMono Nerd Font Mono") : font_family_; },
                    nullptr, [this](int) { select_font(); }});
    rows.push_back({Row::Stepper, "Font size", "Also Ctrl+= / Ctrl+- / Ctrl+wheel",
                    [this] { return std::to_string(static_cast<int>(font_size_)); }, nullptr,
                    [this](int d) { if (d) set_font_size(font_size_ + static_cast<float>(d)); }});
    rows.push_back({Row::Stepper, "Window opacity",
                    "Translucent background, opaque text (50-100%)",
                    [this] { return std::to_string(static_cast<int>(std::lround(opacity_ * 100))) + "%"; }, nullptr,
                    [this](int d) { if (d) set_opacity(opacity_ + 0.05f * static_cast<float>(d)); }});
    rows.push_back(toggle("Font ligatures", "Render => != -> as joined glyphs when the font has them",
                          [this] { return font_.ligatures(); }, "toggle_ligatures"));
    rows.push_back(toggle("Animations", "Smooth scrolling, caret glide, panel and popup motion",
                          [] { return anim::enabled; }, "toggle_animations"));
    rows.push_back(toggle("Word wrap", "Wrap long lines at the editor width (Alt+Z)",
                          [this] { return editor_view_.wrap(); }, "toggle_word_wrap"));
    rows.push_back(toggle("Minimap", "Code overview at the editor's right edge",
                          [this] { return editor_view_.minimap(); }, "toggle_minimap"));
    rows.push_back(toggle("Breadcrumbs", "Path and enclosing symbols above the editor",
                          [this] { return show_breadcrumbs_; }, "toggle_breadcrumbs"));
    rows.push_back(toggle("Remote monitor", "Live CPU, RAM, disk and network of the SSH host",
                          [this] { return remote_monitor_; }, "toggle_remote_monitor"));
    rows.push_back(toggle("Local system monitor", "The same strip for this computer",
                          [this] { return local_monitor_; }, "toggle_local_monitor"));
    rows.push_back(toggle("Inline git blame", "Author and commit at the end of the cursor line",
                          [this] { return editor_view_.blame(); }, "toggle_blame"));
    rows.push_back({Row::Toggle, "Auto-close brackets and quotes", "Typing ( [ { \" ' ` inserts the pair",
                    nullptr, [] { return Editor::auto_close_pairs; }, [this](int) {
                        Editor::auto_close_pairs = !Editor::auto_close_pairs;
                        themes_.set_setting("auto_close_brackets", Editor::auto_close_pairs ? "true" : "false");
                    }});
    auto flag = [this](std::string label, std::string desc, bool* v, std::string key) {
        return Row{Row::Toggle, std::move(label), std::move(desc), nullptr, [v] { return *v; },
                   [this, v, key](int) {
                       *v = !*v;
                       themes_.set_setting(key, *v ? "true" : "false");
                   }};
    };
    rows.push_back(flag("Bracket colours", "Colour ( [ { by nesting depth (4coder style)",
                        &ScopeHighlight::bracket_colors, "bracket_colors"));
    rows.push_back(flag("Scope highlighting", "Tint the scopes around the cursor and box the bracket pair",
                        &ScopeHighlight::scopes, "scope_highlight"));
    rows.push_back(flag("Occurrence underlines", "Mark the identifier under the cursor wherever it is visible",
                        &ScopeHighlight::occurrences, "occurrence_underline"));
    rows.push_back({Row::Button, "Keyboard shortcuts", "Search commands and assign new key chords",
                    [] { return std::string("Edit"); }, nullptr, [this](int) { settings_page_.close(); open_keybindings(); }});
    rows.push_back({Row::Button, "Settings file", (config_dir_ / "settings.toml").string(),
                    [] { return std::string("Open"); }, nullptr, [this](int) {
                        settings_page_.close();
                        fs::path p = config_dir_ / "settings.toml";
                        if (!fs::exists(p)) themes_.set_setting("theme", "\"" + themes_.current() + "\"");
                        open_file(p);
                    }});
    settings_page_.open(std::move(rows));
}

void App::open_keybindings() {
    std::vector<std::string> items;
    std::vector<std::string> names;
    for (auto& c : commands_) {
        std::string k = keymap_.binding_for(c.name);
        items.push_back(c.label + (k.empty() ? "" : "   [" + k + "]"));
        names.push_back(c.name);
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = "Keyboard shortcuts: pick a command, then press its new key chord";
    opts.items = items;
    opts.on_pick = [this, names](int i, const std::string&) {
        if (i < 0) return;
        capture_command_ = names[static_cast<size_t>(i)];
        set_status_message("Press the new shortcut (Esc cancels)...", 30.0f);
    };
    command_palette_.pick(std::move(opts));
}

void App::watch_open_files() {
    if (vfs_->is_remote()) return;
    for (auto& tab : tab_bar_.tabs()) {
        if (!tab.editor) continue;
        const fs::path& p = tab.editor->buffer().file_path();
        if (p.empty()) continue;
        std::error_code ec;
        auto t = fs::last_write_time(p, ec);
        if (ec) continue;
        auto [it, inserted] = disk_mtimes_.try_emplace(p.string(), t);
        if (inserted || it->second == t) continue;
        it->second = t;
        if (!tab.editor->is_modified()) {
            tab.editor->reload();
            attach_git(*tab.editor);
            set_status_message("Reloaded " + p.filename().string() + " (changed on disk)");
        } else {
            set_status_message(p.filename().string() + " changed on disk; you have unsaved edits", 8.0f);
        }
    }
}

void App::render_terminal_header(RenderContext& ctx, Rect r) {
    ctx.r->add_rect(r, theme_.terminal_bg);
    ctx.r->add_rect({r.x, r.y, r.w, 1.0f}, theme_.border);
    float cw = font_.char_width();
    float base = r.y + (r.h - font_.line_height()) * 0.5f + font_.ascender();
    float x = ctx.r->add_text(r.x + 14.0f, base, "TERMINAL", terminal_.focused() ? theme_.fg : theme_.fg_muted, font_);
    std::string label = remote_active_ ? "ssh: " + remote_target_.alias : "bash";
    if (!remote_active_) label = fs::path(local_shell().front()).stem().string();
    ctx.r->add_text(x + 2.0f * cw, base, label, theme_.fg_muted, font_);
    // Underline the active "tab", like the editor tabs.
    ctx.r->add_rect({r.x + 10.0f, r.y + r.h - 2.0f, x - r.x - 6.0f, 2.0f}, theme_.accent);
    term_close_btn_ = {r.x + r.w - 32.0f, r.y + 2.0f, 24.0f, r.h - 4.0f};
    term_restart_btn_ = {r.x + r.w - 60.0f, r.y + 2.0f, 24.0f, r.h - 4.0f};
    for (auto [btn, icon] : {std::pair{term_restart_btn_, 0xEB37u}, std::pair{term_close_btn_, 0xEA76u}}) {
        bool hover = btn.contains(ctx.mouse_x, ctx.mouse_y);
        if (hover) ctx.r->add_rect(btn, theme_.surface_hover, 4.0f);
        ctx.r->add_char(btn.x + (btn.w - cw) * 0.5f, base, icon, hover ? theme_.fg : theme_.fg_muted, font_);
    }
}

std::vector<std::string> App::local_shell() const {
    // settings: terminal_shell = "pwsh -NoLogo" / "cmd.exe" / "C:\\Program Files\\Git\\bin\\bash.exe --login"
    // Words split on spaces; double quotes group a path with spaces.
    std::string spec = themes_.setting("terminal_shell");
    std::vector<std::string> argv;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : spec) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (c == ' ' && !quoted) {
            if (any) argv.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) argv.push_back(cur);
    return argv.empty() ? platform::default_shell() : argv;
}

void App::restart_terminal() {
    if (terminal_argv_.empty()) terminal_.spawn(local_shell(), project_root_);
    else terminal_.spawn(terminal_argv_);
    focus(&terminal_);
}

void App::render_breadcrumbs(RenderContext& ctx, Rect r, Editor* ed) {
    if (r.h <= 0) return;
    ctx.r->add_rect(r, theme_.bg);
    if (!ed) return;
    float cw = font_.char_width();
    float base = r.y + (r.h - font_.line_height()) * 0.5f + font_.ascender();
    float x = r.x + 14.0f;
    auto sep = [&] {
        ctx.r->add_char(x + 2.0f, base, 0xEAB6, theme_.line_number, font_);
        x += cw + 8.0f;
    };
    const fs::path& p = ed->buffer().file_path();
    if (p.empty()) {
        ctx.r->add_text(x, base, "Untitled", theme_.fg_muted, font_);
        return;
    }
    fs::path rel = p.lexically_relative(project_root_);
    if (rel.empty() || rel.generic_string().rfind("..", 0) == 0) rel = p.filename();
    bool first = true;
    for (auto& part : rel) {
        if (!first) sep();
        first = false;
        bool file = part == rel.filename();
        x = ctx.r->add_text(x, base, part.string(), file ? theme_.fg : theme_.fg_muted, font_) + 4.0f;
    }
    Position cur = ed->cursors().primary_pos();
    auto scopes = ed->syntax().scope_at(ed->buffer(), ed->buffer().offset_of(cur.line, cur.col));
    for (auto& s : scopes) {
        sep();
        uint32_t icon = s.kind == SyntaxHighlighter::Scope::Function ? 0xEA8C
                      : s.kind == SyntaxHighlighter::Scope::Class ? 0xEB5B : 0xEA8B;
        Color ic = s.kind == SyntaxHighlighter::Scope::Function ? theme_.function_name : theme_.type_color;
        ctx.r->add_char(x, base, icon, ic, font_);
        x = ctx.r->add_text(x + 2.0f * cw, base, s.name, theme_.fg_muted, font_) + 4.0f;
    }
}

void App::render_activity_bar(RenderContext& ctx, Rect r) {
    ctx.r->add_rect(r, theme_.tab_bg);
    const uint32_t icons[] = {0xEAF0, 0xEA6D, 0xEA68, 0xEAC7};
    float cw = font_.char_width();
    for (int i = 0; i < 4; i++) {
        Rect slot{r.x, r.y + i * ACTIVITY_BAR_W, ACTIVITY_BAR_W, ACTIVITY_BAR_W};
        bool active = sidebar_visible_ && static_cast<int>(sidebar_view_) == i;
        bool hover = slot.contains(ctx.mouse_x, ctx.mouse_y);
        if (active) ctx.r->add_rect({slot.x, slot.y + 10.0f, 2.0f, slot.h - 20.0f}, theme_.accent, 1.0f);
        // Icons at a fixed, larger size than the text, centred in the slot.
        const GlyphInfo* gi = font_.glyph_sized(icons[i], 22);
        if (gi) {
            float bx = std::round(slot.x + (slot.w - gi->width) * 0.5f - gi->bearing_x);
            float by = std::round(slot.y + (slot.h - gi->height) * 0.5f + gi->bearing_y);
            ctx.r->add_glyph(bx, by, gi, active || hover ? theme_.fg : theme_.fg_muted);
        }
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

void App::apply_llm_backend(const std::string& kind, const std::string& endpoint) {
    llm_backend_.kind = kind == "omp" ? LLMBackend::Omp : LLMBackend::OpenAI;
    llm_backend_.endpoint = endpoint.empty() ? "http://127.0.0.1:8000" : endpoint;
    llm_backend_.model = themes_.setting("llm_model");
    std::string key_env = platform::getenv_str("OVERHEAT_LLM_API_KEY");  // never stored in settings
    const char* key = key_env.empty() ? nullptr : key_env.c_str();
    llm_backend_.api_key = key ? key : "";
    llm_backend_.name = llm_backend_.kind == LLMBackend::Omp ? "OMP agent"
                      : llm_backend_.endpoint.find("127.0.0.1:8000") != std::string::npos ? "Flint"
                      : llm_backend_.endpoint;
    chat_.set_backend_name(llm_backend_.name);
}

void App::setup_chat() {
    apply_llm_backend(themes_.setting("llm_backend"), themes_.setting("llm_endpoint"));
    ChatPanel::Callbacks cb;
    cb.send = [this](std::vector<ChatMessage> history) {
        history.insert(history.begin(), {"system", "You are a concise coding assistant inside the Overheat editor. "
                                                   "Project folder: " + project_root_.string() + "."});
        llm_.send(llm_backend_, std::move(history), project_root_,
                  [this](const std::string& delta) { chat_.append_reply(delta); request_redraw(); },
                  [this](const std::string& err) { chat_.finish_reply(err); request_redraw(); });
    };
    cb.stop = [this] { llm_.cancel(); };
    cb.pick_backend = [this] { pick_llm_backend(); };
    chat_.set_callbacks(std::move(cb));
}

void App::pick_llm_backend() {
    CommandPalette::PickOptions opts;
    opts.placeholder = "AI backend (or type an OpenAI-compatible base URL)";
    opts.items = {"Flint (local, http://127.0.0.1:8000)", "OMP agent (omp CLI in the project folder)"};
    opts.allow_free_text = true;
    opts.on_pick = [this](int i, const std::string& query) {
        std::string kind = i == 1 ? "omp" : "openai";
        std::string endpoint = i < 0 ? query : "http://127.0.0.1:8000";
        while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        if (i < 0 && endpoint.size() > 3 && endpoint.compare(endpoint.size() - 3, 3, "/v1") == 0)
            endpoint.resize(endpoint.size() - 3);  // we append /v1/... ourselves
        themes_.set_setting("llm_backend", "\"" + kind + "\"");
        themes_.set_setting("llm_endpoint", "\"" + endpoint + "\"");
        apply_llm_backend(kind, endpoint);
        set_status_message("AI backend: " + llm_backend_.name);
    };
    command_palette_.pick(std::move(opts));
}

void App::setup_search() {
    SearchPanel::Callbacks cb;
    cb.run = [this](const SearchQuery& q) { run_search(q); };
    cb.open = [this](const SearchHit& h) { open_at(h.file, h.line, h.col, h.len); };
    cb.cancel = [this] { cancel_search(); };
    search_.set_callbacks(std::move(cb));
    search_.set_root(project_root_);
}

void App::cancel_search() {
    if (search_stream_) search_stream_->cancel.store(true);
}

void App::run_search(const SearchQuery& q) {
    cancel_search();
    search_stream_.reset();
    if (q.text.empty()) {
        search_.set_results({}, false);
        return;
    }
    if (vfs_->is_remote()) {
        auto hits = search_remote(q);  // runs on the SSH session; brief UI wait
        bool truncated = hits.size() >= SEARCH_MAX_RESULTS;
        if (truncated) hits.resize(SEARCH_MAX_RESULTS);
        search_.set_results(std::move(hits), truncated);
        return;
    }
    // Results stream into the panel as they are found; the previous job was cancelled
    // above, so replacing its future (which waits for it) only blocks for a moment.
    auto stream = std::make_shared<SearchStream>();
    search_stream_ = stream;
    search_.begin_results();
    search_job_ = std::async(std::launch::async, [files = all_files_, q, stream] {
        stream->stats = search_files(files, q, stream->cancel, [&](std::vector<SearchHit>&& batch) {
            stream->push(std::move(batch));
            request_redraw();
        });
        stream->done.store(true);
        request_redraw();
    });
}

void App::poll_search() {
    if (!search_stream_) return;
    auto hits = search_stream_->take();
    if (!hits.empty()) search_.append_results(std::move(hits));
    if (search_stream_->done.load()) {
        if (auto rest = search_stream_->take(); !rest.empty()) search_.append_results(std::move(rest));
        search_.finish_results(search_stream_->stats.truncated);
        search_stream_.reset();
    }
}

void App::open_at(const fs::path& path, size_t line, size_t col, size_t len) {
    open_file(path);
    if (Editor* ed = current_editor()) {
        line = std::min(line, ed->buffer().line_count() - 1);
        ed->cursors().set_primary({line, col});
        ed->cursors().extend_primary({line, col + len});
        ed->set_jump_line(line);
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
    if (tab_bar_.active_index() >= 0) request_close_tab(tab_bar_.active_index());
}

void App::close_tab_now(int idx) {
    tab_bar_.close_tab(idx);
    if (split_ && tab_bar_.empty()) {
        unsplit_if_empty();
        return;
    }
    if (tab_bar_.empty()) {
        new_file();
    } else {
        auto ed = tab_bar_.active_editor();
        editor_view_.set_editor(ed);
        status_bar_.set_editor(ed);
    }
}

void App::request_close_tab(int idx) {
    if (idx < 0 || idx >= static_cast<int>(tab_bar_.tabs().size())) return;
    auto ed = tab_bar_.tabs()[static_cast<size_t>(idx)].editor;
    if (!ed || !ed->is_modified()) {
        close_tab_now(idx);
        return;
    }
    std::string name = ed->buffer().file_path().empty() ? "Untitled" : ed->buffer().file_path().filename().string();
    CommandPalette::PickOptions opts;
    opts.placeholder = "Save changes to " + name + " before closing?";
    opts.items = {"Save", "Don't Save", "Cancel"};
    opts.on_pick = [this, ed](int choice, const std::string&) {
        // Find the tab again: the list may have changed while the prompt was open.
        int idx = -1;
        for (size_t i = 0; i < tab_bar_.tabs().size(); i++)
            if (tab_bar_.tabs()[i].editor == ed) idx = static_cast<int>(i);
        if (idx < 0 || choice == 2 || choice < 0) return;
        if (choice == 0) {
            tab_bar_.set_active(idx);
            editor_view_.set_editor(ed);
            if (ed->buffer().file_path().empty()) { save_as(); return; }  // closes after a later save
            save_current();
            if (ed->is_modified()) return;  // save failed; keep the tab
        }
        close_tab_now(idx);
    };
    command_palette_.pick(std::move(opts));
}

void App::confirm_discard(const std::string& action, std::function<void()> then) {
    std::vector<std::shared_ptr<Editor>> dirty;
    for (auto& t : tab_bar_.tabs())
        if (t.editor && t.editor->is_modified()) dirty.push_back(t.editor);
    if (dirty.empty()) {
        then();
        return;
    }
    CommandPalette::PickOptions opts;
    opts.placeholder = std::to_string(dirty.size()) + " unsaved file(s). " + action + "?";
    opts.items = {"Save All and " + action, action + " Without Saving", "Cancel"};
    opts.on_pick = [this, dirty, then](int choice, const std::string&) {
        if (choice == 0) {
            for (auto& ed : dirty) {
                if (ed->buffer().file_path().empty()) continue;  // untitled: nowhere to save
                if (!vfs_->write(ed->buffer().file_path(), ed->buffer().text())) {
                    set_status_message("Save FAILED: " + ed->buffer().file_path().string(), 8.0f);
                    return;
                }
                ed->buffer().set_unmodified();
            }
            then();
        } else if (choice == 1) {
            then();
        }
    };
    command_palette_.pick(std::move(opts));
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
    add("quick_switch",            "Switch Editor (recent)",   "Ctrl+Tab");
    add("next_tab",                "Next Tab",                 "Ctrl+PageDown");
    add("prev_tab",                "Previous Tab",             "Ctrl+PageUp");
    add("command_palette_files",   "Quick Open",               "Ctrl+P");
    add("command_palette_commands","Command Palette",           "Ctrl+Shift+P");
    add("toggle_terminal",         "Toggle Terminal",           "Ctrl+`");
    add("toggle_sidebar",          "Toggle Sidebar",            "Ctrl+B");
    add("copy",                    "Copy",                      "Ctrl+C");
    add("cut",                     "Cut",                       "Ctrl+X");
    add("paste",                   "Paste",                     "Ctrl+V");
    add("undo",                    "Undo",                      "Ctrl+Z");
    add("redo",                    "Redo",                      "Ctrl+Shift+Z");
    add("select_word",             "Add Selection To Next Find Match", "Ctrl+D");
    add("add_cursor_above",        "Add Cursor Above",          "Ctrl+Alt+Up");
    add("add_cursor_below",        "Add Cursor Below",          "Ctrl+Alt+Down");
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
    add("zoom_reset",              "Reset Zoom",                "Ctrl+0");
    add("remote_connect",          "Remote-SSH: Connect to Host...", "");
    add("remote_disconnect",       "Remote-SSH: Close Remote Connection", "");
    add("toggle_remote_monitor",   "Remote-SSH: Toggle Remote Monitor", "");
    add("monitor_details",         "Remote-SSH: Show System Monitor Details", "");
    add("toggle_local_monitor",    "View: Toggle Local System Monitor", "");
    add("view_explorer",           "View: Explorer",            "Ctrl+Shift+E");
    add("trigger_suggest",         "Trigger Suggest",           "Ctrl+Space");
    add("goto_definition",         "Go to Definition",          "F12");
    add("view_search",             "Search: Find in Files",     "Ctrl+Shift+F");
    add("view_scm",                "View: Source Control",      "Ctrl+Shift+G");
    add("git_checkout",            "Git: Checkout to...",       "");
    add("toggle_blame",            "Git: Toggle Inline Blame",  "");
    add("view_chat",               "AI: Open Chat",             "Ctrl+Alt+I");
    add("ai_ask_selection",        "AI: Ask About Selection",   "");
    add("ai_backend",              "AI: Select Backend",        "");
    add("ai_new_chat",             "AI: New Chat",              "");
    add("open_settings",           "Preferences: Open Settings", "Ctrl+,");
    add("keybindings",             "Preferences: Keyboard Shortcuts", "");
    add("split_editor",            "View: Split Editor",        "Ctrl+\\");
    add("focus_group_1",           "View: Focus Left Editor Group", "Ctrl+1");
    add("focus_group_2",           "View: Focus Right Editor Group", "Ctrl+2");
    add("toggle_word_wrap",        "View: Toggle Word Wrap",    "Alt+Z");
    add("toggle_minimap",          "View: Toggle Minimap",      "");
    add("toggle_breadcrumbs",      "View: Toggle Breadcrumbs",  "");
    add("toggle_animations",       "Preferences: Toggle Animations", "");
    add("select_font",             "Preferences: Font Family",  "");
    add("toggle_ligatures",        "Preferences: Toggle Font Ligatures", "");
    add("select_theme",            "Preferences: Color Theme",  "Ctrl+K Ctrl+T");
    add("workspace_switch",        "Workspaces: Open Workspace...", "Ctrl+R");
    add("workspace_save_as",       "Workspaces: Save Workspace As...", "");
    add("workspace_add_folder",    "Workspaces: Add Folder to Workspace...", "");
    add("workspace_close",         "Workspaces: Close Workspace", "");
    add("workspace_delete",        "Workspaces: Delete Workspace...", "");
    add("workspace_edit",          "Workspaces: Edit Workspace File", "");
    add("open_folder",             "File: Open Folder...",      "");
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
    for (auto& root : extra_roots_) scan(root, 0);  // multi-root workspace folders
    std::sort(all_files_.begin(), all_files_.end());
}

// ---------------------------------------------------------------------------
// LSP helper
// ---------------------------------------------------------------------------

} // namespace oh
