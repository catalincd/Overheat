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
#include "ui/scm_panel.h"
#include "ui/search_panel.h"
#include "git/repository.h"
#include "ssh/session.h"
#include "ssh/ssh_config.h"
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
    void save_as();
    void new_file();
    void close_current_tab();
    void request_close_tab(int idx);
    void close_tab_now(int idx);
    // Runs `then` once unsaved editors are saved or explicitly discarded.
    void confirm_discard(const std::string& action, std::function<void()> then);
    void set_status_message(const std::string& msg, float seconds = 3.0f);
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
    ScmPanel scm_;
    SearchPanel search_;
    enum class SidebarView { Explorer, Search, SourceControl };  // activity bar order
    SidebarView sidebar_view_ = SidebarView::Explorer;
    Panel& sidebar_panel() {
        switch (sidebar_view_) {
            case SidebarView::Search: return search_;
            case SidebarView::SourceControl: return scm_;
            default: return file_tree_;
        }
    }
    std::future<std::vector<SearchHit>> search_job_;
    std::shared_ptr<std::atomic<bool>> search_cancel_;
    void setup_search();
    void run_search(const SearchQuery& q);
    std::vector<SearchHit> search_remote(const SearchQuery& q);
    void open_at(const fs::path& path, size_t line, size_t col, size_t len);
    void focus(Panel* p);
    void show_sidebar_view(SidebarView v);
    void render_activity_bar(RenderContext& ctx, Rect r);
    void setup_scm();

    GitRepository git_;
    std::unique_ptr<Vfs> vfs_ = std::make_unique<LocalVfs>();
    std::future<std::vector<GitStatus>> git_job_;
    std::vector<GitStatus> git_statuses_;
    float git_poll_timer_ = 0;
    bool git_refresh_pending_ = false;
    void request_git_refresh();
    void poll_git_refresh();
    void attach_git(Editor& ed);
    void git_checkout();
    void after_head_change();
    SSHSession ssh_;
    SshConfig ssh_config_;
    SshTarget remote_target_;
    std::future<SSHConnectStatus> connect_job_;
    bool remote_active_ = false;
    fs::path local_root_;
    void remote_connect_prompt();
    void remote_connect(const std::string& target, bool trust_host, const std::string& secret);
    void poll_remote_connect();
    void open_remote_folder(const fs::path& path);
    void remote_disconnect();
    LSPClient lsp_;
    std::string lsp_server_;
    std::mutex lsp_mutex_;
    std::vector<std::function<void()>> lsp_tasks_;
    std::unordered_map<std::string, std::vector<LSPDiagnostic>> diagnostics_;
    struct LspDoc { uint64_t version = 0; TimePoint changed{}; uint64_t sent = 0; };
    std::unordered_map<std::string, LspDoc> lsp_docs_;
    uint64_t completion_request_ = 0;
    void lsp_post(std::function<void()> fn);
    void lsp_update();
    void lsp_maybe_complete(const std::string& typed, bool manual);
    void lsp_goto_definition();
    size_t lsp_col_to_byte(const fs::path& path, size_t line, size_t col);
    Position lsp_position(Editor& ed);
    PluginManager plugins_;

    ScriptRunner script_;
    bool scripted_ = false;
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
    float sidebar_width_ = 260.0f;
    static constexpr float ACTIVITY_BAR_W = 48.0f;
    float terminal_height_ = 200.0f;

    fs::path project_root_;
    std::vector<Command> commands_;
    std::vector<fs::path> all_files_;

    // Layout
    struct Layout {
        Rect activity_bar;
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
