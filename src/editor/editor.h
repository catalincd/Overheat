#pragma once
#include "types.h"
#include "editor/buffer.h"
#include "editor/cursor.h"
#include "editor/syntax.h"
#include "git/repository.h"
#include <future>
#include <memory>
#include <string>

namespace oh {

class Editor {
public:
    Editor();

    void load_file(const fs::path& path);
    void load_content(const fs::path& path, const std::string& content);
    void save();
    void save_as(const fs::path& path);
    void new_file();

    // Text manipulation
    void insert_text(const std::string& text);
    void delete_backward();
    void delete_forward();
    void delete_word_backward();
    void delete_word_forward();
    void delete_line();
    void duplicate_line();
    void move_line_up();
    void move_line_down();
    void indent();
    void unindent();
    void toggle_comment();
    void undo();
    void redo();
    // Replace byte ranges (sorted, non-overlapping) with `text` as one undo step.
    void replace_ranges(const std::vector<std::pair<size_t, size_t>>& ranges, const std::string& text);
    void goto_line(size_t line);

    // Clipboard helpers. With no selection, copy/cut take the whole line
    // (including its newline) and report line_mode so paste inserts above.
    std::string copy_text(bool& line_mode) const;
    std::string cut_text(bool& line_mode);
    void paste_text(const std::string& text, bool line_mode);
    void newline();
    void tab();

    // Selection/cursor
    CursorSet& cursors() { return cursors_; }
    const CursorSet& cursors() const { return cursors_; }

    // Buffer access
    TextBuffer& buffer() { return buffer_; }
    const TextBuffer& buffer() const { return buffer_; }

    // Syntax
    SyntaxHighlighter& syntax() { return syntax_; }
    const std::vector<Token>& tokens_for_line(size_t line) const;

    // Scroll state
    float scroll_x() const { return scroll_x_; }
    float scroll_y() const { return scroll_y_; }
    void set_scroll(float x, float y) { scroll_x_ = x; scroll_y_ = y; }
    void scroll_to_cursor(float view_width, float view_height, float char_w, float line_h);

    // Brings the syntax tree up to date with all buffer edits (including undo/redo).
    void sync_syntax();

    // Git baseline (file at HEAD); nullopt for untracked files or outside a repo.
    void set_git_base(std::optional<std::string> base);
    bool has_git_base() const { return git_base_.has_value(); }
    // Recomputed lazily when the buffer changed since the last call.
    const std::vector<GutterMark>& git_marks();

    // Blame of the committed file (computed off-thread by the caller).
    void set_blame_job(std::future<std::vector<GitBlame>> job) { blame_job_ = std::move(job); blame_.clear(); }
    // Blame for a current-buffer line; nullptr if unknown. `uncommitted` is set
    // when the line differs from HEAD.
    const GitBlame* blame_for_line(size_t line, bool& uncommitted);

    // Re-reads the file from disk keeping cursor and scroll (e.g. after checkout).
    void reload();

    // State
    bool is_modified() const { return buffer_.is_modified(); }
    std::string title() const;

private:
    TextBuffer buffer_;
    CursorSet cursors_;
    SyntaxHighlighter syntax_;
    float scroll_x_ = 0, scroll_y_ = 0;
    std::optional<std::string> git_base_;
    std::vector<GutterMark> git_marks_;
    uint64_t git_marks_version_ = UINT64_MAX;
    std::future<std::vector<GitBlame>> blame_job_;
    std::vector<GitBlame> blame_;  // index = HEAD line - 1

    void on_text_changed();
};

} // namespace oh
