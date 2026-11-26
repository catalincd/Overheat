#pragma once
#include "ui/panel.h"
#include <functional>
#include <set>

namespace oh {

class Editor;

// VS Code-style status bar: clickable items on both sides, each with a hover
// state. Items drop out by priority when the window is narrow, and any of them
// can be hidden by id (settings: status_bar_hidden = "encoding,eol").
//
// Item ids, left:  remote, branch, sync, problems, tasks (+ transient message)
//           right: position, indent, encoding, eol, language, lsp, bell
class StatusBar : public Panel {
public:
    static constexpr float HEIGHT = 26.0f;

    void set_editor(std::shared_ptr<Editor> e) { editor_ = e; }
    void set_git_branch(const std::string& b) { git_branch_ = b; }
    // upstream: the branch tracks a remote branch; has_remote: any remote exists
    // (no upstream but a remote shows a publish button instead of sync).
    void set_git_sync(int ahead, int behind, bool upstream = true, bool has_remote = true) {
        ahead_ = ahead;
        behind_ = behind;
        upstream_ = upstream;
        has_remote_ = has_remote;
    }
    void set_git_dirty(bool d) { git_dirty_ = d; }
    bool publish_mode() const { return !upstream_ && has_remote_; }
    void set_diagnostic_counts(int errors, int warnings) { errors_ = errors; warnings_ = warnings; }
    // Non-empty while connected to a remote host; shown as an accent badge.
    void set_remote(const std::string& label) { remote_ = label; }
    void set_message(const std::string& msg, float duration = 3.0f);
    // Running background work (LSP start, git, SSH): a spinner with the first one.
    void set_tasks(std::vector<std::string> tasks) { tasks_ = std::move(tasks); }
    // The last build task's outcome ("cargo build: 2 errors"), until the next run; "" hides it.
    void set_task_result(const std::string& text, bool ok) { task_result_ = text; task_ok_ = ok; }
    // Language server for the current file: "" = none. state: 0 starting, 1 ready, 2 stopped.
    void set_lsp(const std::string& name, int state) { lsp_name_ = name; lsp_state_ = state; }
    void set_unread(int n) { unread_ = n; }
    void set_hidden(std::set<std::string> ids) { hidden_ = std::move(ids); }
    const std::set<std::string>& hidden() const { return hidden_; }
    // Left click on an item (by id); right click anywhere on the bar sends "menu".
    void set_on_click(std::function<void(const std::string& id)> cb) { on_click_ = std::move(cb); }

    // All item ids with a human-readable name, in display order.
    static const std::vector<std::pair<std::string, std::string>>& item_names();
    // Language ids (as SyntaxHighlighter::detect_language returns them) and names.
    static const std::vector<std::pair<std::string, std::string>>& languages();
    static std::string language_name(const std::string& id);

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    std::shared_ptr<Editor> editor_;
    std::string git_branch_;
    std::string remote_;
    int ahead_ = 0, behind_ = 0;
    bool upstream_ = true, has_remote_ = true, git_dirty_ = false;
    int errors_ = 0, warnings_ = 0;
    std::string message_;
    float message_timer_ = 0;
    std::vector<std::string> tasks_;
    std::string task_result_;
    bool task_ok_ = true;
    std::string lsp_name_;
    int lsp_state_ = 0;
    int unread_ = 0;
    float spin_ = 0;  // spinner phase, seconds
    std::set<std::string> hidden_;
    std::function<void(const std::string&)> on_click_;
    std::vector<std::pair<Rect, std::string>> hits_;  // item rects of the last frame
    // File-format facts, recomputed only when the buffer changes.
    const Editor* format_editor_ = nullptr;
    uint64_t format_version_ = UINT64_MAX;
    bool crlf_ = false, bom_ = false;
};

} // namespace oh
