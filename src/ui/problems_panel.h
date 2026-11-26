#pragma once
// Problems panel (bottom panel, next to the terminal): language-server diagnostics
// for all files plus build-task problems, grouped by file, with severity icons and
// counts, a text filter and severity toggles. Click or Enter jumps to the location.
#include "ui/panel.h"
#include "tasks/problem.h"
#include <functional>

namespace oh {

class ProblemsPanel : public Panel {
public:
    struct Item {
        std::string file;  // absolute
        size_t line = 0, col = 0;  // 0-based; col in characters
        int severity = 1;          // 1 error, 2 warning, 3 info, 4 hint
        std::string message, code, source;
        std::vector<ProblemNote> notes;
    };

    // Replaces the list; selection, scroll and collapsed files are kept where possible.
    void set_items(std::vector<Item> items);
    const std::vector<Item>& items() const { return items_; }
    void set_root(const fs::path& root) { root_ = root; }
    // Header text on the right ("cargo build: running...", "Build failed: 2 errors").
    void set_status(const std::string& s, int tone = 0) { status_ = s; status_tone_ = tone; }
    void set_on_open(std::function<void(const std::string& file, size_t line, size_t col)> cb) { on_open_ = std::move(cb); }
    // Selects the row of this problem (F8 went there) and scrolls it into view.
    void reveal(const std::string& file, size_t line, size_t col);
    int count(int severity) const;  // all items of a severity (3 also counts hints)

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool wants_focus() const override { return true; }

private:
    struct Row {
        int file = -1;  // index into files_
        int item = -1;  // -1: file header
        int note = -1;  // >= 0: a note of `item`
    };
    std::vector<Item> items_;
    std::vector<std::string> files_;              // distinct files, sorted
    std::vector<std::vector<int>> file_items_;    // per file, visible items (after the filter)
    std::vector<std::string> collapsed_;          // collapsed file paths
    std::vector<Row> rows_;
    fs::path root_;
    std::string filter_, status_;
    int status_tone_ = 0;  // 0 muted, 1 error, 2 success
    bool show_[4] = {true, true, true, true};  // error, warning, info, hint
    bool filter_active_ = false;
    int selected_ = -1;
    float scroll_y_ = 0, row_h_ = 22.0f, list_y_ = 0;
    Rect filter_box_{}, toggle_[3]{};
    std::function<void(const std::string&, size_t, size_t)> on_open_;

    void rebuild();
    void open_row(int ri);
    void scroll_into_view();
    bool visible_item(const Item& it) const;
};

} // namespace oh
