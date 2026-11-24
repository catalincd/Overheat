#include "editor/cursor.h"
#include "editor/buffer.h"
#include <algorithm>

namespace oh {

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

void CursorSet::set_primary(Position pos) {
    selections_.clear();
    selections_.push_back({pos, pos});
    update_preferred_col();
}

void CursorSet::add_cursor(Position pos) {
    selections_.push_back({pos, pos});
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::clear_secondary() {
    if (selections_.size() > 1) {
        Selection primary = selections_[0];
        selections_.clear();
        selections_.push_back(primary);
    }
}

void CursorSet::update_preferred_col() {
    preferred_col_ = selections_[0].head.col;
}

void CursorSet::merge_overlapping() {
    if (selections_.size() <= 1) return;

    // Sort by min position
    std::sort(selections_.begin(), selections_.end(),
              [](const Selection& a, const Selection& b) {
                  return a.min() < b.min();
              });

    std::vector<Selection> merged;
    merged.push_back(selections_[0]);

    for (size_t i = 1; i < selections_.size(); ++i) {
        Selection& last = merged.back();
        const Selection& cur = selections_[i];

        // If overlapping or adjacent, merge
        if (cur.min() <= last.max()) {
            // Extend to the larger range
            Position new_min = last.min() < cur.min() ? last.min() : cur.min();
            Position new_max = last.max() < cur.max() ? cur.max() : last.max();
            // Keep the head of the later selection as the head
            last.anchor = new_min;
            last.head = new_max;
        } else {
            merged.push_back(cur);
        }
    }

    selections_ = std::move(merged);
    if (selections_.empty()) {
        selections_.push_back({{0, 0}, {0, 0}});
    }
}

// Helper: move the head of a selection, optionally keeping anchor
static void apply_move(Selection& sel, Position new_head, bool select) {
    sel.head = new_head;
    if (!select) {
        sel.anchor = new_head;
    }
}

void CursorSet::move_left(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        if (!select && !sel.empty()) {
            // Collapse to min
            Position p = sel.min();
            sel.anchor = sel.head = p;
            continue;
        }
        Position h = sel.head;
        if (h.col > 0) {
            apply_move(sel, {h.line, h.col - 1}, select);
        } else if (h.line > 0) {
            size_t prev_len = buf.line_length(h.line - 1);
            apply_move(sel, {h.line - 1, prev_len}, select);
        }
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_right(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        if (!select && !sel.empty()) {
            Position p = sel.max();
            sel.anchor = sel.head = p;
            continue;
        }
        Position h = sel.head;
        size_t ll = buf.line_length(h.line);
        if (h.col < ll) {
            apply_move(sel, {h.line, h.col + 1}, select);
        } else if (h.line + 1 < buf.line_count()) {
            apply_move(sel, {h.line + 1, 0}, select);
        }
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_up(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        if (h.line > 0) {
            size_t target_col = std::max(preferred_col_, h.col);
            // Use preferred_col_ for vertical movement
            target_col = preferred_col_;
            size_t prev_len = buf.line_length(h.line - 1);
            size_t col = std::min(target_col, prev_len);
            apply_move(sel, {h.line - 1, col}, select);
        }
    }
    merge_overlapping();
    // Don't update preferred_col for up/down
}

void CursorSet::move_down(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        if (h.line + 1 < buf.line_count()) {
            size_t target_col = preferred_col_;
            size_t next_len = buf.line_length(h.line + 1);
            size_t col = std::min(target_col, next_len);
            apply_move(sel, {h.line + 1, col}, select);
        }
    }
    merge_overlapping();
}

void CursorSet::move_home(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        // Smart home: go to first non-whitespace, or column 0 if already there
        std::string line = buf.line_text(sel.head.line);
        size_t first_non_ws = 0;
        while (first_non_ws < line.size() && (line[first_non_ws] == ' ' || line[first_non_ws] == '\t')) {
            ++first_non_ws;
        }
        size_t target = (sel.head.col == first_non_ws) ? 0 : first_non_ws;
        apply_move(sel, {sel.head.line, target}, select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_end(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        size_t ll = buf.line_length(sel.head.line);
        apply_move(sel, {sel.head.line, ll}, select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_word_left(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t offset = buf.offset_of(h.line, h.col);
        if (offset == 0) continue;

        --offset;
        // Skip whitespace backwards
        while (offset > 0 && !is_word_char(buf.char_at(offset))) {
            --offset;
        }
        // Skip word chars backwards
        while (offset > 0 && is_word_char(buf.char_at(offset - 1))) {
            --offset;
        }
        // If we stopped on a non-word char and we were skipping non-word,
        // we might need to adjust
        if (!is_word_char(buf.char_at(offset)) && offset > 0) {
            // We're on whitespace/punct; the word boundary is at offset
        } else {
            // offset is at start of word
        }

        apply_move(sel, buf.position_of(offset), select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_word_right(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t offset = buf.offset_of(h.line, h.col);
        size_t total = buf.length();
        if (offset >= total) continue;

        // Skip word chars forward
        if (is_word_char(buf.char_at(offset))) {
            while (offset < total && is_word_char(buf.char_at(offset))) {
                ++offset;
            }
        }
        // Skip non-word chars forward
        while (offset < total && !is_word_char(buf.char_at(offset))) {
            ++offset;
        }

        apply_move(sel, buf.position_of(offset), select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_page_up(const TextBuffer& buf, size_t page_lines, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t new_line = (h.line >= page_lines) ? h.line - page_lines : 0;
        size_t ll = buf.line_length(new_line);
        size_t col = std::min(preferred_col_, ll);
        apply_move(sel, {new_line, col}, select);
    }
    merge_overlapping();
}

void CursorSet::move_page_down(const TextBuffer& buf, size_t page_lines, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t new_line = std::min(h.line + page_lines, buf.line_count() - 1);
        size_t ll = buf.line_length(new_line);
        size_t col = std::min(preferred_col_, ll);
        apply_move(sel, {new_line, col}, select);
    }
    merge_overlapping();
}

void CursorSet::move_to_start(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        apply_move(sel, {0, 0}, select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_to_end(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        size_t last_line = (buf.line_count() > 0) ? buf.line_count() - 1 : 0;
        size_t ll = buf.line_length(last_line);
        apply_move(sel, {last_line, ll}, select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::select_all(const TextBuffer& buf) {
    selections_.clear();
    size_t last_line = (buf.line_count() > 0) ? buf.line_count() - 1 : 0;
    size_t ll = buf.line_length(last_line);
    selections_.push_back({{0, 0}, {last_line, ll}});
    update_preferred_col();
}

void CursorSet::select_word(const TextBuffer& buf) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        std::string line = buf.line_text(h.line);
        if (line.empty()) continue;

        size_t col = std::min(h.col, line.size() > 0 ? line.size() - 1 : 0);

        if (is_word_char(line[col])) {
            size_t start = col;
            size_t end = col;
            while (start > 0 && is_word_char(line[start - 1])) --start;
            while (end < line.size() && is_word_char(line[end])) ++end;
            sel.anchor = {h.line, start};
            sel.head = {h.line, end};
        }
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::select_line(const TextBuffer& buf) {
    for (auto& sel : selections_) {
        size_t line = sel.head.line;
        sel.anchor = {line, 0};
        if (line + 1 < buf.line_count()) {
            sel.head = {line + 1, 0};
        } else {
            sel.head = {line, buf.line_length(line)};
        }
    }
    merge_overlapping();
    update_preferred_col();
}

} // namespace oh
