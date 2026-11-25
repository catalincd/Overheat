#pragma once
#include "ui/panel.h"
#include "editor/editor.h"

namespace oh {

// In-file find / replace overlay hosted by the editor view.
class FindBar {
public:
    struct Match { size_t start, len; };

    void open(Editor& ed, bool replace);
    void close() { open_ = false; input_focus_ = false; }
    bool is_open() const { return open_; }
    bool has_input_focus() const { return open_ && input_focus_; }
    void blur() { input_focus_ = false; }

    // Recompute matches if the buffer or query changed.
    void refresh(Editor& ed);
    const std::vector<Match>& matches() const { return matches_; }
    int current() const { return current_; }

    void next(Editor& ed, int dir);
    void replace_one(Editor& ed);
    void replace_all(Editor& ed);

    // Returns the rect it occupies (for hit testing by the host).
    Rect render(RenderContext& ctx, Rect editor_rect);
    bool handle_key(Editor& ed, const KeyEvent& e);
    bool handle_text(Editor& ed, const std::string& text);
    bool handle_mouse(Editor& ed, const MouseEvent& e);

private:
    bool open_ = false;
    bool replace_mode_ = false;
    bool input_focus_ = false;
    int field_ = 0;  // 0 = find, 1 = replace
    std::string query_, replacement_;
    bool case_sensitive_ = false, whole_word_ = false;
    std::vector<Match> matches_;
    int current_ = -1;
    uint64_t seen_version_ = UINT64_MAX;
    std::string seen_query_;
    bool seen_case_ = false, seen_word_ = false;
    Rect rect_{}, case_btn_{}, word_btn_{}, prev_btn_{}, next_btn_{}, close_btn_{}, find_box_{}, repl_box_{};

    void select_current(Editor& ed);
    void pick_nearest(Editor& ed);
};

} // namespace oh
