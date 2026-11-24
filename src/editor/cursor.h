#pragma once
#include "types.h"
#include <vector>

namespace oh {

class TextBuffer;

class CursorSet {
public:
    void set_primary(Position pos);
    void add_cursor(Position pos);
    void clear_secondary();

    void move_left(const TextBuffer& buf, bool select = false);
    void move_right(const TextBuffer& buf, bool select = false);
    void move_up(const TextBuffer& buf, bool select = false);
    void move_down(const TextBuffer& buf, bool select = false);
    void move_home(const TextBuffer& buf, bool select = false);
    void move_end(const TextBuffer& buf, bool select = false);
    void move_word_left(const TextBuffer& buf, bool select = false);
    void move_word_right(const TextBuffer& buf, bool select = false);
    void move_page_up(const TextBuffer& buf, size_t page_lines, bool select = false);
    void move_page_down(const TextBuffer& buf, size_t page_lines, bool select = false);
    void move_to_start(const TextBuffer& buf, bool select = false);
    void move_to_end(const TextBuffer& buf, bool select = false);

    void select_all(const TextBuffer& buf);
    void select_word(const TextBuffer& buf);
    void select_line(const TextBuffer& buf);

    const std::vector<Selection>& selections() const { return selections_; }
    const Selection& primary() const { return selections_[0]; }
    Position primary_pos() const { return selections_[0].head; }

    size_t preferred_col() const { return preferred_col_; }
    void set_preferred_col(size_t c) { preferred_col_ = c; }

private:
    std::vector<Selection> selections_ = {{{0,0},{0,0}}};
    size_t preferred_col_ = 0;

    void update_preferred_col();
    void merge_overlapping();
};

} // namespace oh
