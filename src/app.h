#pragma once
#include "types.h"
#include "renderer/vulkan_context.h"
#include "renderer/font_atlas.h"
#include "renderer/text_renderer.h"
#include "renderer/ui_renderer.h"
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
    TextRenderer text_renderer_;
    UIRenderer ui_renderer_;

    Theme theme_;
    KeyMap keymap_;

    TabBar tab_bar_;
    EditorView editor_view_;
    FileTree file_tree_;
    Terminal terminal_;
    StatusBar status_bar_;
    CommandPalette command_palette_;

    GitRepository git_;
    SSHSession ssh_;
    LSPClient lsp_;
    PluginManager plugins_;

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
};

} // namespace oh
