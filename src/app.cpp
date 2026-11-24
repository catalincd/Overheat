#include "app.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace oh {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

App::App() = default;

App::~App() {
    plugins_.shutdown();

    if (lsp_.is_running()) lsp_.stop();
    if (ssh_.is_connected()) ssh_.disconnect();
    terminal_.kill();

    font_.cleanup(vk_);
    text_renderer_.cleanup(vk_);
    ui_renderer_.cleanup(vk_);
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
    project_root_ = fs::current_path();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--project=", 0) == 0) {
            project_root_ = fs::absolute(arg.substr(10));
        } else if (arg == "--project" && i + 1 < argc) {
            project_root_ = fs::absolute(argv[++i]);
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
    // Try several common font paths
    const char* font_paths[] = {
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto-cjk/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        nullptr
    };

    bool font_loaded = false;
    for (const char** fp = font_paths; *fp; ++fp) {
        if (fs::exists(*fp)) {
            if (font_.init(vk_, *fp, 15.0f)) {
                font_loaded = true;
                break;
            }
        }
    }

    if (!font_loaded) {
        // Last resort: try fontconfig
        // (In production we'd use fc-match, but for now just fail gracefully)
        std::fprintf(stderr, "Could not find a monospace font\n");
        return false;
    }

    // ---- Renderers ----
    if (!text_renderer_.init(vk_, font_)) {
        std::fprintf(stderr, "TextRenderer::init failed\n");
        return false;
    }
    if (!ui_renderer_.init(vk_)) {
        std::fprintf(stderr, "UIRenderer::init failed\n");
        return false;
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
    }

    // ---- File tree ----
    file_tree_.set_root(project_root_);
    file_tree_.set_on_open([this](const fs::path& p) { open_file(p); });

    // ---- Terminal ----
    terminal_.spawn();

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
    command_palette_.set_file_list(all_files_);
    command_palette_.set_on_file_open([this](const fs::path& p) { open_file(p); });

    // ---- Text input ----
    SDL_StartTextInput(window_);

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::run() {
    auto last_time = Clock::now();

    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            handle_event(e);
        }

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;

        update(dt);
        render();
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

        // Check keymap
        std::string cmd = keymap_.lookup(kc, mods);
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
        Layout layout = compute_layout();

        MouseEvent me{mx, my, static_cast<int>(e.button.button), true,
                      (e.button.clicks >= 2), 0, 0};

        // Determine which panel was clicked and set focus
        if (command_palette_.is_visible()) {
            command_palette_.handle_mouse(me);
        } else if (sidebar_visible_ && layout.sidebar.contains(mx, my)) {
            file_tree_.set_focused(true);
            terminal_.set_focused(false);
            editor_view_.set_focused(false);
            file_tree_.handle_mouse(me);
        } else if (terminal_visible_ && layout.terminal.contains(mx, my)) {
            terminal_.set_focused(true);
            file_tree_.set_focused(false);
            editor_view_.set_focused(false);
            terminal_.handle_mouse(me);
        } else if (layout.tab_bar.contains(mx, my)) {
            tab_bar_.handle_mouse(me);
        } else if (layout.editor.contains(mx, my)) {
            editor_view_.set_focused(true);
            file_tree_.set_focused(false);
            terminal_.set_focused(false);
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
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        Layout layout = compute_layout();

        MouseEvent me{mx, my, 0, false, false, e.wheel.x, e.wheel.y};

        if (sidebar_visible_ && layout.sidebar.contains(mx, my)) {
            file_tree_.handle_mouse(me);
        } else if (terminal_visible_ && layout.terminal.contains(mx, my)) {
            terminal_.handle_mouse(me);
        } else if (layout.editor.contains(mx, my)) {
            editor_view_.handle_mouse(me);
        }
        break;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        if (e.motion.state & SDL_BUTTON_LMASK) {
            MouseEvent me{e.motion.x, e.motion.y, 1, true, false, 0, 0};
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
        // TODO: file dialog
        save_current();
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
    } else if (cmd == "undo") {
        if (Editor* ed = current_editor()) ed->buffer().undo();
    } else if (cmd == "redo") {
        if (Editor* ed = current_editor()) ed->buffer().redo();
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
        // TODO: implement goto line dialog
        command_palette_.show(CommandPalette::Mode::Commands);
    } else if (cmd == "find") {
        // TODO: implement find/replace
        command_palette_.show(CommandPalette::Mode::Commands);
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
    float status_h = 24.0f;
    l.status_bar = {0, fh - status_h, fw, status_h};

    float content_h = fh - status_h;

    // Sidebar: left side
    float sb_w = sidebar_visible_ ? sidebar_width_ : 0.0f;
    l.sidebar = {0, 0, sb_w, content_h};

    float right_x = sb_w;
    float right_w = fw - sb_w;

    // Tab bar: 32px tall at top of right region
    float tab_h = 32.0f;
    l.tab_bar = {right_x, 0, right_w, tab_h};

    // Terminal: at bottom of right region (above status bar)
    float term_h = terminal_visible_ ? terminal_height_ : 0.0f;
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

void App::update(float dt) {
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
    uint32_t image_index;
    if (!vk_.begin_frame(image_index)) {
        vk_.recreate_swapchain();
        return;
    }

    VkCommandBuffer cmd = vk_.current_frame_data().cmd;
    VkExtent2D extent = vk_.extent();

    Layout layout = compute_layout();

    RenderContext ctx;
    ctx.vk    = &vk_;
    ctx.text  = &text_renderer_;
    ctx.ui    = &ui_renderer_;
    ctx.font  = &font_;
    ctx.theme = &theme_;
    ctx.dt    = 0; // already handled in update

    // Begin both batches - panels will add to both
    ui_renderer_.begin_batch();
    text_renderer_.begin_batch();

    if (sidebar_visible_) {
        ui_renderer_.add_rect(layout.sidebar, theme_.sidebar_bg);
        ui_renderer_.add_rect({layout.sidebar.x + layout.sidebar.w - 1, layout.sidebar.y, 1, layout.sidebar.h},
                              theme_.border);
    }
    ui_renderer_.add_rect(layout.tab_bar, theme_.tab_bg);
    ui_renderer_.add_rect(layout.editor, theme_.bg);
    if (terminal_visible_) {
        ui_renderer_.add_rect(layout.terminal, theme_.terminal_bg);
        ui_renderer_.add_rect({layout.terminal.x, layout.terminal.y, layout.terminal.w, 1},
                              theme_.border);
    }
    ui_renderer_.add_rect(layout.status_bar, theme_.status_bg);

    // All panels add their UI rects (selections, highlights, gutter bg)
    if (sidebar_visible_) file_tree_.render(ctx, layout.sidebar);
    tab_bar_.render(ctx, layout.tab_bar);
    editor_view_.render(ctx, layout.editor);
    if (terminal_visible_) terminal_.render(ctx, layout.terminal);
    status_bar_.render(ctx, layout.status_bar);
    if (command_palette_.is_visible()) {
        float palette_w = std::min(600.0f, layout.editor.w * 0.8f);
        float palette_h = 400.0f;
        float palette_x = layout.editor.x + (layout.editor.w - palette_w) * 0.5f;
        float palette_y = layout.editor.y + 50.0f;
        command_palette_.render(ctx, {palette_x, palette_y, palette_w, palette_h});
    }

    // Flush all UI rects first (backgrounds behind text)
    ui_renderer_.flush(vk_, cmd, extent);

    // Flush all text on top
    text_renderer_.flush(vk_, cmd, extent);

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
            return;
        }
    }

    auto editor = std::make_shared<Editor>();
    editor->load_file(path);

    // Detect language and set syntax highlighting
    std::string lang = SyntaxHighlighter::detect_language(path);
    if (!lang.empty()) {
        editor->syntax().set_language(lang);
        editor->syntax().update(editor->buffer());
    }

    tab_bar_.add_tab(editor);
    tab_bar_.set_active(static_cast<int>(tab_bar_.tabs().size()) - 1);
    editor_view_.set_editor(editor);
    status_bar_.set_editor(editor);

    setup_lsp_for_file(path);
}

void App::save_current() {
    if (Editor* ed = current_editor()) {
        ed->save();
        set_status_message("Saved " + ed->buffer().file_path().filename().string());

        // Notify LSP
        if (lsp_.is_running()) {
            lsp_.did_save(ed->buffer().file_path());
        }
    }
}

void App::new_file() {
    auto editor = std::make_shared<Editor>();
    editor->new_file();
    tab_bar_.add_tab(editor);
    tab_bar_.set_active(static_cast<int>(tab_bar_.tabs().size()) - 1);
    editor_view_.set_editor(editor);
    status_bar_.set_editor(editor);
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

void App::set_status_message(const std::string& msg) {
    status_bar_.set_message(msg);
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
    add("toggle_fullscreen",       "Toggle Fullscreen",         "F11");
    add("zoom_in",                 "Zoom In",                   "Ctrl++");
    add("zoom_out",                "Zoom Out",                  "Ctrl+-");
    add("quit",                    "Quit",                      "Ctrl+Q");
}

// ---------------------------------------------------------------------------
// Project scanning
// ---------------------------------------------------------------------------

void App::scan_project_files() {
    all_files_.clear();

    static const std::vector<std::string> skip_dirs = {
        ".git", "node_modules", ".cache", "__pycache__", ".tox",
        "build", "dist", ".idea", ".vscode", "target", ".eggs",
        "vendor", "third_party", ".hg", ".svn"
    };

    std::function<void(const fs::path&)> scan = [&](const fs::path& dir) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const auto& p = entry.path();
            std::string name = p.filename().string();

            if (entry.is_directory(ec)) {
                if (std::find(skip_dirs.begin(), skip_dirs.end(), name) == skip_dirs.end()
                    && name[0] != '.') {
                    scan(p);
                }
            } else if (entry.is_regular_file(ec)) {
                all_files_.push_back(p);
            }
        }
    };

    scan(project_root_);

    // Sort for consistent ordering
    std::sort(all_files_.begin(), all_files_.end());
}

// ---------------------------------------------------------------------------
// LSP helper
// ---------------------------------------------------------------------------

void App::setup_lsp_for_file(const fs::path& path) {
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
