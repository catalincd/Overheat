#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include "ui/tab_bar.h"
#include "ui/file_tree.h"
#include "ui/terminal.h"
#include "ui/status_bar.h"
#include "ui/command_palette.h"
#include "ui/editor_view.h"
#include "git/repository.h"
#include "ssh/session.h"
#include "lsp/client.h"
#include "plugin/api.h"
#include "keys.h"
#include "script.h"
#include "theme.h"
#include "vfs.h"
#include <future>
#include <SDL3/SDL.h>

namespace oh {

class App {
public:
    App();
    ~App();

    bool init(int argc, char** argv);
    void run();
    void quit();

    // Public API for plugins
    void open_file(const fs::path& path);
    void save_current();
    void new_file();
    void close_current_tab();
    void set_status_message(const std::string& msg);
    Editor* current_editor();

    Theme& theme() { return theme_; }

private:
    SDL_Window* window_ = nullptr;
    VulkanContext vk_;
    FontAtlas font_;
    Renderer2D renderer_;

    Theme theme_;
    ThemeManager themes_;
    float theme_poll_timer_ = 0;
    KeyMap keymap_;

    TabBar tab_bar_;
    EditorView editor_view_;
    FileTree file_tree_;
    Terminal terminal_;
    StatusBar status_bar_;
    CommandPalette command_palette_;

    GitRepository git_;
    std::unique_ptr<Vfs> vfs_ = std::make_unique<LocalVfs>();
    std::future<std::vector<GitStatus>> git_job_;
    std::vector<GitStatus> git_statuses_;
    float git_poll_timer_ = 0;
    bool git_refresh_pending_ = false;
    void request_git_refresh();
    void poll_git_refresh();
    SSHSession ssh_;
    LSPClient lsp_;
    PluginManager plugins_;

    ScriptRunner script_;
    std::vector<std::string> pending_probes_;
    bool debug_hit_ = false;
    std::string shot_dir_;  // --shot-dir: frames for shot: probes are captured in-app
    std::string own_clipboard_;        // what we last put on the clipboard
    std::string clipboard_line_text_;  // last whole-line copy, to paste as lines
    float last_click_x_ = -1, last_click_y_ = -1;
    TimePoint start_time_ = Clock::now();
    bool first_paint_done_ = false;
    uint64_t frame_count_ = 0;
    std::vector<float> frame_times_ms_;

    bool running_ = false;
    bool sidebar_visible_ = true;
    bool terminal_visible_ = true;
    float sidebar_width_ = 240.0f;
    float terminal_height_ = 200.0f;

    fs::path project_root_;
    std::vector<Command> commands_;
    std::vector<fs::path> all_files_;

    // Layout
    struct Layout {
        Rect sidebar;
        Rect tab_bar;
        Rect editor;
        Rect terminal;
        Rect status_bar;
    };
    Layout compute_layout() const;

    void handle_event(const SDL_Event& e);
    void handle_command(const std::string& cmd);
    void update(float dt);
    void render();

    void register_commands();
    void scan_project_files();
    void setup_lsp_for_file(const fs::path& path);
    KeyMod sdl_mods(SDL_Keymod m) const;
    void emit_probe(const std::string& label);
    void focus_editor();
    void select_theme();
    void print_frame_stats() const;
};

} // namespace oh
