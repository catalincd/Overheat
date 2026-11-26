#include "editor/cursor.h"
#include "editor/utf8.h"
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

void CursorSet::add_selection(Selection s) {
    selections_.push_back(s);
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::set_selections(std::vector<Selection> sels) {
    if (sels.empty()) return;
    selections_ = std::move(sels);
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
    preferred_col_ = SIZE_MAX;  // recomputed from the primary head on the next vertical move
}

size_t CursorSet::vertical_target_col(const TextBuffer& buf, size_t line) {
    if (preferred_col_ == SIZE_MAX) {
        const Position& h = selections_[0].head;
        preferred_col_ = utf8::display_col(buf.line_text(h.line), h.col);
    }
    return utf8::byte_col_at(buf.line_text(line), preferred_col_);
}

void CursorSet::extend_primary(Position pos) {
    selections_[0].head = pos;
    update_preferred_col();
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
            apply_move(sel, {h.line, utf8::prev_boundary(buf.line_text(h.line), h.col)}, select);
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
            apply_move(sel, {h.line, utf8::next_boundary(buf.line_text(h.line), h.col)}, select);
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
            apply_move(sel, {h.line - 1, vertical_target_col(buf, h.line - 1)}, select);
        }
    }
    merge_overlapping();
    // Don't update preferred_col for up/down
}

void CursorSet::move_down(const TextBuffer& buf, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        if (h.line + 1 < buf.line_count()) {
            apply_move(sel, {h.line + 1, vertical_target_col(buf, h.line + 1)}, select);
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

namespace {
// Byte accessor over the piece table for motion::seek_*: caches one line, so scans in
// either direction copy each line once. Bytes past a line's text read as '\n'.
struct LineCursor {
    explicit LineCursor(const TextBuffer& b) : buf(b) {}
    const TextBuffer& buf;
    size_t start = SIZE_MAX;
    std::string text;
    char operator()(size_t i) {
        if (start == SIZE_MAX || i < start || i > start + text.size()) {
            Position p = buf.position_of(i);
            start = i - p.col;
            text = buf.line_text(p.line);
        }
        size_t col = i - start;
        return col < text.size() ? text[col] : '\n';
    }
};
}  // namespace

void CursorSet::move_boundary(const TextBuffer& buf, int dir, motion::Boundary b, bool select) {
    size_t total = buf.length();
    LineCursor at(buf);
    for (auto& sel : selections_) {
        size_t offset = buf.offset_of(sel.head.line, sel.head.col);
        size_t next = dir < 0 ? motion::seek_left(at, total, offset, b) : motion::seek_right(at, total, offset, b);
        apply_move(sel, buf.position_of(next), select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_blank_line(const TextBuffer& buf, int dir, bool select) {
    size_t n = buf.line_count();
    auto blank = [&](size_t line) {
        std::string t = buf.line_text(line);
        return std::all_of(t.begin(), t.end(), [](char c) { return motion::is_space(static_cast<unsigned char>(c)); });
    };
    for (auto& sel : selections_) {
        size_t line = motion::blank_line_hop(blank, n, sel.head.line, dir);
        Position to;
        if (line == SIZE_MAX) to = {0, 0};                              // above the first paragraph
        else if (line >= n) to = {n - 1, buf.line_length(n - 1)};      // below the last one
        else to = {line, buf.line_length(line)};                        // 4coder's *_blank_line_end
        apply_move(sel, to, select);
    }
    merge_overlapping();
    update_preferred_col();
}

void CursorSet::move_page_up(const TextBuffer& buf, size_t page_lines, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t new_line = (h.line >= page_lines) ? h.line - page_lines : 0;
        apply_move(sel, {new_line, vertical_target_col(buf, new_line)}, select);
    }
    merge_overlapping();
}

void CursorSet::move_page_down(const TextBuffer& buf, size_t page_lines, bool select) {
    for (auto& sel : selections_) {
        Position h = sel.head;
        size_t new_line = std::min(h.line + page_lines, buf.line_count() - 1);
        apply_move(sel, {new_line, vertical_target_col(buf, new_line)}, select);
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
        // A cursor just after a word selects that word (as in VS Code).
        if (!is_word_char(line[col]) && col > 0 && h.col > 0 && is_word_char(line[h.col - 1])) col = h.col - 1;

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
