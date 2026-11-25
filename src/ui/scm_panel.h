#pragma once
#include "ui/panel.h"
#include "git/repository.h"
#include <functional>

namespace oh {

// Source control view: commit message box, commit button, staged and unstaged
// change lists with per-file stage/unstage actions.
class ScmPanel : public Panel {
public:
    struct Callbacks {
        std::function<void(const fs::path&)> open;     // absolute path
        std::function<void(const fs::path&)> stage;
        std::function<void(const fs::path&)> unstage;
        std::function<void()> stage_all;
        std::function<void(std::string)> commit;
    };
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }
    void set_state(const fs::path& workdir, const std::string& branch, std::vector<GitStatus> status);
    size_t change_count() const { return status_.size(); }
    void clear_message() { message_.clear(); }
    bool input_focused() const { return focused_ && input_active_; }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;

private:
    enum class RowKind { SectionStaged, SectionChanges, Staged, Change };
    struct Row {
        RowKind kind;
        size_t index;  // into status_
    };
    struct Hit {
        Rect rect;
        int row;      // -1: commit button, -2: input box
        bool action;  // the +/- button
    };

    Callbacks cb_;
    fs::path workdir_;
    std::string branch_;
    std::vector<GitStatus> status_;
    std::vector<Row> rows_;
    std::vector<Hit> hits_;
    std::string message_;
    bool input_active_ = false;
    float scroll_y_ = 0;
    float row_h_ = 24.0f;

    void rebuild_rows();
    void do_commit();
};

} // namespace oh
