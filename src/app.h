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
#include "ui/settings_page.h"
#include "ui/chat_panel.h"
#include "ui/monitor_bar.h"
#include "ui/pets.h"
#include "ui/problems_panel.h"
#include "tasks/runner.h"
#include "ui/command_center.h"
#include "ui/debug_panel.h"
#include "ui/debug_console.h"
#include "ui/debug_toolbar.h"
#include "debug/debugger.h"
#include "llm.h"
#include "git/repository.h"
#include "ssh/session.h"
#include "ssh/ssh_config.h"
#include "lsp/client.h"
#include "plugin/api.h"
#include "keys.h"
#include "script.h"
#include "theme.h"
#include "anim.h"
#include "vfs.h"
#include "workspace.h"
#include <future>
#include <optional>
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
    fs::path config_dir_;
    std::vector<std::string> font_chain_;
    float font_size_ = 15.0f;
    float opacity_ = 1.0f;          // window background opacity (settings: opacity)
    bool window_opacity_set_ = false;  // whole-window fallback in use
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
    ChatPanel chat_;
    LLMClient llm_;  // after chat_: destroyed first, so no reply lands in a dead panel
    LLMBackend llm_backend_;
    void setup_chat();
    void pick_llm_backend();
    void apply_llm_backend(const std::string& kind, const std::string& endpoint);
    enum class SidebarView { Explorer, Search, SourceControl, Chat, Debug };  // activity bar order
    SidebarView sidebar_view_ = SidebarView::Explorer;
    Panel& sidebar_panel() {
        switch (sidebar_view_) {
            case SidebarView::Search: return search_;
            case SidebarView::SourceControl: return scm_;
            case SidebarView::Chat: return chat_;
            case SidebarView::Debug: return debug_panel_;
            default: return file_tree_;
        }
    }
    std::future<void> search_job_;
    std::shared_ptr<SearchStream> search_stream_;
    void poll_search();
    void cancel_search();
    void setup_search();
    void run_search(const SearchQuery& q);
    std::vector<SearchHit> search_remote(const SearchQuery& q);
    void open_at(const fs::path& path, size_t line, size_t col, size_t len);
    void focus(Panel* p);
    void show_sidebar_view(SidebarView v);
    void render_activity_bar(RenderContext& ctx, Rect r);
    void render_breadcrumbs(RenderContext& ctx, Rect r, Editor* ed);
    void restart_terminal();
    std::vector<std::string> terminal_argv_;  // empty = local shell (local_shell())
    std::vector<std::string> local_shell() const;
    Rect term_restart_btn_{}, term_close_btn_{};

    // Split editors: tab_bar_/editor_view_ are always the *active* group; the
    // other group lives in tab_bar2_/editor_view2_ and the two are swapped on focus.
    TabBar tab_bar2_;
    EditorView editor_view2_;
    bool split_ = false, active_right_ = false;
    float split_ratio_ = 0.5f;
    struct ViewState { CursorSet cursors; float sx = 0, sy = 0; };
    std::optional<ViewState> other_state_;  // inactive view's state when both show one file
    void split_editor();
    void focus_group(bool right);
    void unsplit_if_empty();
    bool show_breadcrumbs_ = true;
    SettingsPage settings_page_;
    void open_settings();
    void open_keybindings();
    std::string capture_command_;  // keybinding editor: waiting for a chord
    std::optional<std::pair<SDL_Keycode, KeyMod>> chord_prefix_;  // first key of a chord, armed
    bool chord_eat_text_ = false;  // drop the text input of a chord's second key
    std::unordered_map<std::string, fs::file_time_type> disk_mtimes_;
    float watch_timer_ = 0;
    void watch_open_files();
    void refresh_command_keys();
    // Keymap preset from settings.toml `keymap` ("overheat" or "vscode") + keybindings.json.
    std::string keymap_preset() const;
    void load_keymap();
    // Cursor motion / syntax-selection commands; false when nothing applies.
    bool run_motion(Editor& ed, const std::string& cmd);
    fs::path session_file() const;
    void save_session();
    bool restore_session();
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
    // System monitor strip (monitor.h): the remote host while connected, else
    // optionally this machine. Settings: remote_monitor (on), local_monitor (off).
    SysMonitor monitor_;
    MonitorBar monitor_bar_;
    bool remote_monitor_ = true, local_monitor_ = false;
    std::string monitor_key_, monitor_folder_;
    bool monitor_visible() const { return !monitor_key_.empty(); }
    void update_monitor();
    // Status bar items and the Command Center (status_glue.cpp).
    CommandCenter command_center_;
    bool show_command_center_ = true;  // settings: command_center
    std::vector<fs::path> recent_files_;  // Quick Open MRU, newest first
    struct Notification {
        std::string text;
        int64_t time = 0;  // unix seconds
    };
    std::vector<Notification> notifications_;  // newest last
    int unread_notifications_ = 0;
    std::future<std::pair<bool, std::string>> git_op_job_;  // pull / push / sync
    std::string git_op_label_;
    void setup_status_bar();
    bool status_command(const std::string& cmd);  // true when handled
    void status_click(const std::string& id);
    void update_status_bar();
    void note_recent_file(const fs::path& p);
    void remote_menu();
    void show_problems();
    void goto_next_problem();
    void pick_indentation();
    void pick_line_endings();
    void pick_encoding();
    void pick_language();
    void lsp_menu();
    void show_notifications();
    void status_items_menu();
    void git_remote_op(const std::string& op);  // "pull" | "push" | "sync" | "publish"
    void poll_git_op();
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

    // LSP navigation (nav_glue.cpp): hover, Ctrl+click, peek definition/references,
    // rename, symbol pickers, switch header/source, inlay hints, back/forward history.
    struct NavState;
    struct NavStateDeleter { void operator()(NavState* s) const; };
    std::unique_ptr<NavState, NavStateDeleter> nav_;
    void setup_nav();
    bool nav_command(const std::string& cmd);     // true when handled
    bool nav_key(SDL_Keycode key, KeyMod mods);   // popups and the peek view first
    void nav_key_up(SDL_Keycode key);
    bool nav_text(const std::string& text);
    bool nav_mouse(const SDL_Event& e);           // true when consumed
    void nav_update();
    void nav_render_editor(RenderContext& ctx);   // the peek view in the editor's zone
    void nav_render_popups(RenderContext& ctx, Rect window);
    void nav_probe();
    float nav_peek_height();
    PluginManager plugins_;

    // Debugger (debug_glue.cpp; session model in debug/debugger.h)
    Debugger debugger_;
    DebugPanel debug_panel_;
    DebugConsole debug_console_;
    DebugToolbar debug_toolbar_;
    struct CachedMarks {
        bool valid = false;
        uint64_t bp_gen = 0, dbg_gen = 0, buf_ver = 0;
        DebugMarks marks;
    };
    std::unordered_map<std::string, CachedMarks> debug_marks_;  // by file
    void show_debug_console(bool focus_it);
    void setup_debugger();
    void debug_register_commands();
    void debug_update();
    void debug_probe();
    bool debug_command(const std::string& cmd);
    void debug_breakpoint_menu(const fs::path& path, size_t line);
    void debug_edit_breakpoint(const fs::path& path, size_t line, const std::string& what);
    const DebugMarks* debug_marks_for(Editor* ed);
    // Build tasks, the Problems panel, terminal links, quick fixes and formatting
    // (tasks_glue.cpp). The bottom panel shows Problems, the shell or the task terminal.
    enum class BottomView { Problems, Terminal, Task, DebugConsole };
    BottomView bottom_view_ = BottomView::Terminal;
    TaskRunner tasks_;
    Terminal task_term_;  // after tasks_: its reader thread (the task filter) stops first
    bool task_term_used_ = false;
    ProblemsPanel problems_;
    std::vector<Problem> task_problems_;  // the last run's, absolute paths, byte columns
    uint64_t task_problems_version_ = UINT64_MAX;
    std::unordered_map<std::string, std::vector<LSPDiagnostic>> merged_diags_;  // LSP + build, per file
    uint64_t problems_dirty_ = 1;
    bool format_on_save_ = false;
    std::optional<TaskDef> last_task_;
    std::vector<std::pair<Rect, BottomView>> panel_tabs_;
    void setup_tasks();
    void tasks_update();
    bool tasks_command(const std::string& cmd);  // true when handled
    void pick_task(const std::string& group);    // "" = all tasks
    void run_task(const TaskDef& t);
    void run_default_task(const std::string& group);
    TaskContext task_context();
    Panel& bottom_panel();
    Terminal* bottom_terminal();  // nullptr while Problems shows
    void show_bottom(BottomView v, bool take_focus);
    void render_panel_header(RenderContext& ctx, Rect r);
    bool panel_header_click(float x, float y);
    bool open_link(const TextLink& link, const fs::path& cwd, bool open);
    fs::path resolve_path(const std::string& p, const fs::path& cwd) const;
    void refresh_problems();
    void goto_problem(int dir);
    void code_actions();
    void format_document(std::function<void()> then);
    // Applies text edits (one undo step per open file; other files are rewritten on
    // disk). Columns are bytes (EDIT_BYTES), UTF-16 units or characters.
    enum { EDIT_BYTES, EDIT_UTF16, EDIT_CHARS };
    bool apply_edits(const std::vector<ProblemEdit>& edits, int unit, std::string* summary);
    int lsp_edit_unit() const { return lsp_.utf8_positions() ? EDIT_BYTES : EDIT_UTF16; }
    struct PendingFormat {
        uint64_t id = 0;
        TimePoint deadline{};
        std::function<void()> then;
    };
    std::optional<PendingFormat> pending_format_;  // format on save waits for the server, briefly
    uint64_t format_request_ = 0;

    // Workspaces (workspace.cpp). Never opened on their own: only from the
    // switcher (Ctrl+R) or the welcome screen. Autosaved only once opened or saved.
    WorkspaceStore workspaces_;
    std::optional<Workspace> workspace_;          // the open workspace, if any
    std::optional<Workspace> pending_workspace_;  // waiting for its SSH connection
    std::vector<fs::path> extra_roots_;           // root folders after project_root_
    std::vector<std::function<void()>> workspace_undo_;  // reverts settings overrides
    std::optional<Workspace> hint_workspace_;     // a saved workspace this folder belongs to
    std::vector<std::pair<Rect, std::function<void()>>> welcome_hits_;
    bool welcome_visible_ = false;
    std::vector<Workspace> recent_workspaces_;  // welcome screen list, newest first
    bool recent_stale_ = true;
    void setup_workspaces();
    void workspace_command(const std::string& cmd);
    void workspace_switcher();
    void open_workspace(Workspace ws);
    void finish_open_workspace(Workspace ws);
    void restore_workspace_tabs(const Workspace& ws);
    Workspace capture_workspace() const;
    void autosave_workspace();
    void close_workspace(bool keep_tabs);
    void save_workspace_as(const std::string& suggested = "");
    void add_folder_to_workspace();
    void delete_workspace();
    void open_folder_prompt();
    void switch_local_root(const fs::path& root);
    void apply_workspace_settings(const Workspace& ws);
    void revert_workspace_settings();
    void update_workspace_hint(bool announce);
    void update_window_title();
    std::string folder_uri(const fs::path& p) const;
    void render_welcome(RenderContext& ctx, Rect r);
    bool welcome_click(float x, float y);

    ScriptRunner script_;
    bool scripted_ = false;
    bool switcher_open_ = false;  // Ctrl+Tab list is showing
    std::vector<std::string> pending_probes_;
    bool debug_hit_ = false;
    std::string shot_dir_;  // --shot-dir: frames for shot: probes are captured in-app
    std::string own_clipboard_;        // what we last put on the clipboard
    std::string clipboard_line_text_;  // last whole-line copy, to paste as lines
    float last_click_x_ = -1, last_click_y_ = -1;
    float script_mouse_x_ = -1, script_mouse_y_ = -1;  // --script: synthetic motion moves no real pointer
    TimePoint start_time_ = Clock::now();
    bool first_paint_done_ = false;
    uint64_t frame_count_ = 0;
    std::vector<float> frame_times_ms_;

    bool running_ = false;
    bool sidebar_visible_ = true;
    bool terminal_visible_ = true;
    float sidebar_width_ = 260.0f;
    anim::Smooth sidebar_open_, terminal_open_, split_open_;  // 0..1 slide fractions
    void load_motion_settings();  // cursor_animation, cursor_vfx, scroll_animation_ms, wobbly
    bool hover_was_chrome_ = false;
    // Pet foxes in a strip under the editor (ui/pets.h). Settings: pets, pet_count,
    // pet_kit_color, pets_sleep_after.
    Pets pets_;
    bool pets_on_ = false;
    TimePoint pets_deadline_{};  // when the yard wants its next frame (max() = never)
    void load_pet_settings();
    bool window_shown() const;
    static constexpr float ACTIVITY_BAR_W = 48.0f;
    float terminal_height_ = 200.0f;
    // compute_layout(): below these the sidebar / terminal hide so the editor keeps its minimum.
    static constexpr float MIN_EDITOR_W = 280.0f, MIN_EDITOR_H = 150.0f;
    static constexpr float MIN_SIDEBAR_W = 170.0f, MIN_TERMINAL_H = 100.0f;

    fs::path project_root_;
    std::vector<Command> commands_;
    std::vector<fs::path> all_files_;

    // Layout
    struct Layout {
        Rect title_bar;  // Command Center row (h = 0 when hidden)
        Rect activity_bar;
        Rect sidebar;
        Rect tab_bar;
        Rect breadcrumbs;
        Rect editor;
        // The other editor group when split (empty otherwise).
        Rect tab_bar2, breadcrumbs2, editor2;
        float group_split_x = -1;  // x of the divider between groups
        Rect yard;  // pets strip between the editor column and the terminal (h = 0 when hidden)
        Rect terminal_header;
        Rect terminal;
        Rect monitor;  // system monitor strip above the status bar (h = 0 when hidden)
        Rect status_bar;
    };
    Layout compute_layout() const;

    void handle_event(const SDL_Event& e);
    float refresh_rate() const;
    void handle_command(const std::string& cmd);
    void update(float dt);
    void render();
    // Live resize: redraw from SDL's event watch while the OS runs its modal
    // size/move loop (Windows), so the compositor never stretches a stale frame.
    static bool SDLCALL live_resize_watch(void* userdata, SDL_Event* e);
    void live_resize_frame();
    bool rendering_ = false;

    void register_commands();
    void scan_project_files();
    void setup_lsp_for_file(const fs::path& path);
    // Reads `path` into `ed` (large-file mode above filecheck::LARGE_MODE; binaries read-only).
    bool load_editor_file(Editor& ed, const fs::path& path, bool binary);
    void open_gated(Editor& ed);  // "Open Anyway" on a binary / large file card
    KeyMod sdl_mods(SDL_Keymod m) const;
    void emit_probe(const std::string& label);
    void focus_editor();
    void set_font_size(float size);
    // Applies opacity_: per-pixel translucency when the surface supports it, else
    // whole-window opacity (X11/Windows), else opaque. Returns a short mode name.
    const char* apply_opacity();
    void set_opacity(float o);
    std::vector<std::string> build_font_chain(const std::string& family) const;
    bool reload_font(const std::string& family, float size);
    void select_font();
    std::string font_family_;

    // Draggable splitters between sidebar/editor and editor/terminal.
    enum class Splitter { None, Sidebar, Terminal, Groups };
    Splitter splitter_hover_ = Splitter::None, splitter_drag_ = Splitter::None;
    Splitter splitter_at(float x, float y) const;
    void update_cursor_shape(Splitter s);
    SDL_Cursor* cursor_ew_ = nullptr;
    SDL_Cursor* cursor_ns_ = nullptr;
    SDL_Cursor* cursor_default_ = nullptr;
    void select_theme();
    void select_file_icons();
    void print_frame_stats() const;
};

} // namespace oh
