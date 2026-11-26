#pragma once
#include "types.h"
#include "editor/buffer.h"
#include "editor/cursor.h"
#include "editor/file_check.h"
#include "editor/indent.h"
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

    // VS Code-style multi-cursor helpers.
    void add_next_occurrence();          // Ctrl+D
    void add_cursor_vertical(int dir);   // Ctrl+Alt+Up/Down

    // 4coder-style word motion for Ctrl+Left/Right: alphanumeric runs in code,
    // whitespace-separated runs in plain text (4coder's code map vs file map).
    void move_word_4coder(int dir, bool select);
    bool is_code() const;

    // Syntax-aware selection over the tree-sitter tree (Helix/Kakoune, VS Code's
    // smart select). Every cursor works independently; false when there is no tree.
    bool expand_selection();           // to the enclosing node
    bool shrink_selection();           // back to the previous expand step (or a child)
    bool select_sibling(int dir);      // next (+1) / previous (-1) sibling node
    // Typing with auto-closing brackets/quotes, skip-over and selection wrapping.
    void type_text(const std::string& text);
    static inline bool auto_close_pairs = true;  // setting: auto_close_brackets
    // Matching bracket for the bracket at/before the primary cursor.
    bool find_bracket_pair(Position& a, Position& b) const;

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

    // Jump location (4coder-style line highlight): the line of the last search hit,
    // definition or (later) build error jumped to. Dropped when edits shift lines.
    void set_jump_line(size_t line) { jump_ = std::pair{line, buffer_.line_count()}; }
    void clear_jump_line() { jump_.reset(); }
    std::optional<size_t> jump_line() const {
        if (jump_ && jump_->second == buffer_.line_count()) return jump_->first;
        return std::nullopt;
    }

    // Re-reads the file from disk keeping cursor and scroll (e.g. after checkout).
    void reload();

    // Indentation used by Tab / indent / unindent: detected on load, changed from the
    // status bar.
    struct IndentStyle {
        bool tabs = false;
        int size = 4;
    };
    IndentStyle indent_style() const { return indent_style_; }
    void set_indent_style(IndentStyle s) { indent_style_ = s; }
    std::string indent_unit() const;
    // Rewrites every line's leading whitespace from the current style to `to` (one undo step).
    void convert_indentation(IndentStyle to);
    // Line endings and BOM as stored in the buffer; conversions are one undo step.
    bool uses_crlf() const;
    void set_line_endings(bool crlf);
    bool has_bom() const;
    void set_bom(bool on);
    // Language mode override (syntax highlighting); "" = plain text.
    void set_language(const std::string& lang_id);
    // Binary / very large files (App::open_file). A gated editor has no text yet;
    // its view shows a card asking whether to open the file anyway.
    filecheck::Verdict gate() const { return gate_; }
    uint64_t file_size() const { return file_size_; }
    void set_gate(filecheck::Verdict g, uint64_t size) {
        gate_ = g;
        file_size_ = size;
        set_read_only(g != filecheck::Verdict::Open);
    }
    // Read-only editors ignore every edit (binaries opened anyway, gated files).
    bool read_only() const { return read_only_; }
    void set_read_only(bool on) { read_only_ = on; buffer_.set_read_only(on); }
    // Large-file mode (also for binaries): no syntax tree, LSP, minimap, wrap or blame.
    bool large_file() const { return large_file_; }
    bool binary() const { return binary_; }
    void set_large_file(bool large, bool binary) { large_file_ = large || binary; binary_ = binary; }

    // State
    bool is_modified() const { return buffer_.is_modified(); }
    std::string title() const;

    // ---- Structural indentation (indent_mode; see editor/indent.h, indent_editor.cpp) ----
    // Computed indent of a line in display columns, or IndentEngine::KEEP.
    int computed_indent(size_t line) const;
    bool indent_supported() const;
    // Re-indent the lines touched by the selections / the whole file (one undo step).
    // Return the number of lines changed.
    size_t reindent_selection();
    size_t reindent_file();
    // Virtual whitespace (indent_mode = "virtual"): leading whitespace in the buffer is
    // not shown; lines are drawn at their computed indent. view_col/view_byte map byte
    // columns <-> display columns (plain utf8 columns when virtual whitespace is off).
    bool virtual_ws() const;
    size_t view_col(size_t line, const std::string& text, size_t byte) const;
    size_t view_byte(size_t line, const std::string& text, size_t dcol) const;
    // Keeps carets out of the invisible leading whitespace. With the selections from
    // before a key: moving left out of the indent goes to the previous line, and
    // vertical moves keep their on-screen column.
    enum class Nav { Other, Home, Vertical };
    void virtual_normalize(const std::vector<Selection>* before = nullptr, Nav nav = Nav::Other,
                           size_t pref_before = SIZE_MAX);
    // Called before writing the file: virtual mode with indent_write_on_save writes the
    // computed indentation into the buffer first.
    void before_save();

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
    std::optional<std::pair<size_t, size_t>> jump_;  // (line, line count when set)
    IndentStyle indent_style_;
    void detect_indentation();
    void replace_all_text(const std::string& text);
    filecheck::Verdict gate_ = filecheck::Verdict::Open;
    uint64_t file_size_ = 0;
    bool read_only_ = false, large_file_ = false, binary_ = false;

    void on_text_changed();

    // Expand history for shrink_selection: the selection sets before each expand step,
    // valid while the cursors still hold `expanded_`.
    std::vector<std::vector<Selection>> expand_history_;
    std::vector<Selection> expanded_;
    // Indentation state (indent_editor.cpp).
    mutable IndentEngine indent_;
    mutable uint64_t indent_version_ = UINT64_MAX, indent_loads_ = UINT64_MAX;
    bool indent_hold_ = false;   // undo/redo and reindent commands: no auto re-indent
    size_t virtual_pref_ = 0;    // display column kept by vertical moves in virtual mode
    void indent_sync() const;
    void indent_consume_dirty(std::vector<size_t>* lines);
    void indent_after_edit();
    bool indent_newline();       // auto/virtual Enter; false = default behaviour
    bool indent_tab();           // auto/virtual Tab at the indent re-indents the line
    void virtual_extend_delete(bool backward);
    std::string virtual_copy(bool& line_mode) const;
    size_t reindent_lines(std::vector<size_t> lines);
    void set_selections(const std::vector<Selection>& sels);
};

} // namespace oh
