#pragma once
#include "types.h"
#include "editor/buffer.h"
#include "editor/cursor.h"
#include "editor/syntax.h"
#include <memory>
#include <string>

namespace oh {

class Editor {
public:
    Editor();

    void load_file(const fs::path& path);
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

    // State
    bool is_modified() const { return buffer_.is_modified(); }
    std::string title() const;

private:
    TextBuffer buffer_;
    CursorSet cursors_;
    SyntaxHighlighter syntax_;
    float scroll_x_ = 0, scroll_y_ = 0;

    void on_text_changed();
};

} // namespace oh
