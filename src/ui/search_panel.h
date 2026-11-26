#pragma once
#include "ui/panel.h"
#include "search.h"
#include <functional>

namespace oh {

class SearchPanel : public Panel {
public:
    struct Callbacks {
        std::function<void(const SearchQuery&)> run;  // host starts the search
        std::function<void(const SearchHit&)> open;
        std::function<void()> cancel;  // the query changed: stop the running search
    };
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }
    void set_root(const fs::path& root) { root_ = root; }
    void set_results(std::vector<SearchHit> hits, bool truncated);
    // Streaming: clear, append batches as they arrive, then mark the search finished.
    void begin_results();
    void append_results(std::vector<SearchHit>&& hits);
    void finish_results(bool truncated);
    void set_busy(bool busy) { busy_ = busy; }
    void focus_input() { input_active_ = true; }
    bool input_focused() const { return focused_ && input_active_; }
    // Called each frame; fires the debounced search.
    void tick(float dt);

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;

private:
    struct Row { int file; int hit; };  // hit == -1: file header
    Callbacks cb_;
    fs::path root_;
    SearchQuery query_;
    std::vector<SearchHit> hits_;
    std::vector<fs::path> files_;              // distinct files in result order
    std::vector<int> file_counts_;
    std::vector<std::vector<int>> file_hits_;
    std::vector<bool> collapsed_;
    std::vector<Row> rows_;
    bool truncated_ = false, busy_ = false, input_active_ = true;
    float debounce_ = -1.0f;
    float scroll_y_ = 0, row_h_ = 24.0f, list_y_ = 0;
    Rect input_{}, case_btn_{}, word_btn_{};

    void rebuild_rows();
    void schedule() {
        debounce_ = 0.25f;
        if (cb_.cancel) cb_.cancel();
    }
};

} // namespace oh
