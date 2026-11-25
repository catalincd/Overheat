#include "editor/editor.h"
#include "editor/utf8.h"
#include <algorithm>
#include <sstream>

namespace oh {

Editor::Editor() = default;

void Editor::load_file(const fs::path& path) {
    buffer_.load_file(path);
    cursors_.set_primary({0, 0});
    scroll_x_ = 0;
    scroll_y_ = 0;

    std::string lang = SyntaxHighlighter::detect_language(path);
    syntax_.set_language(lang);
    constexpr size_t SYNC_PARSE_LIMIT = 256 * 1024;  // small files never show uncoloured
    if (buffer_.length() <= SYNC_PARSE_LIMIT) syntax_.update(buffer_);
    else syntax_.parse_async(buffer_.text());
}

void Editor::save() {
    if (!buffer_.file_path().empty()) {
        buffer_.save_file(buffer_.file_path());
    }
}

void Editor::save_as(const fs::path& path) {
    buffer_.save_file(path);
}

void Editor::new_file() {
    buffer_.load_string("");
    cursors_.set_primary({0, 0});
    scroll_x_ = 0;
    scroll_y_ = 0;
    syntax_.set_language("");
}

std::string Editor::title() const {
    if (buffer_.file_path().empty()) {
        return "Untitled";
    }
    std::string name = buffer_.file_path().filename().string();
    if (buffer_.is_modified()) {
        return name + " *";
    }
    return name;
}

const std::vector<Token>& Editor::tokens_for_line(size_t line) const {
    static thread_local std::vector<Token> cached;
    size_t start = buffer_.line_start(line);
    size_t len = buffer_.line_length(line);
    cached = syntax_.tokens_for_line(line, start, len);
    return cached;
}

// Get sorted indices of selections in reverse offset order (highest first)
static std::vector<size_t> reverse_sorted_indices(const std::vector<Selection>& sels,
                                                   const TextBuffer& buf) {
    std::vector<size_t> indices(sels.size());
    for (size_t i = 0; i < sels.size(); ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        size_t off_a = buf.offset_of(sels[a].head.line, sels[a].head.col);
        size_t off_b = buf.offset_of(sels[b].head.line, sels[b].head.col);
        return off_a > off_b;
    });
    return indices;
}

void Editor::insert_text(const std::string& text) {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    auto indices = reverse_sorted_indices(sels, buffer_);

    // We need to modify cursors, so work with a copy of positions
    struct CursorEdit {
        size_t sel_idx;
        size_t offset;
        size_t delete_len; // length of selection to delete first
    };
    std::vector<CursorEdit> edits;
    for (size_t idx : indices) {
        const Selection& sel = sels[idx];
        size_t offset, del_len;
        if (sel.empty()) {
            offset = buffer_.offset_of(sel.head.line, sel.head.col);
            del_len = 0;
        } else {
            Position mn = sel.min();
            Position mx = sel.max();
            offset = buffer_.offset_of(mn.line, mn.col);
            del_len = buffer_.offset_of(mx.line, mx.col) - offset;
        }
        edits.push_back({idx, offset, del_len});
    }

    for (auto& e : edits) {
        if (e.delete_len > 0) {
            buffer_.erase(e.offset, e.delete_len);
        }
        buffer_.insert(e.offset, text);
    }

    buffer_.end_undo_group();

    // Update cursor positions: each cursor moves to after inserted text
    // Process in forward order to adjust offsets
    // Simple approach: recalculate from buffer
    // For each edit (processed in reverse order), cursor goes to offset + text.size()
    // Since we processed in reverse, earlier cursors' offsets are still valid
    for (auto& e : edits) {
        Position new_pos = buffer_.position_of(e.offset + text.size());
        // We can't directly modify selections_ since it's const ref, use set_primary for single
    }

    // For simplicity with multi-cursor, recalculate positions
    // The edits were applied in reverse order, so we can compute final positions
    // by replaying in forward order with accumulated offset changes
    if (sels.size() == 1) {
        auto& e = edits[0];
        Position new_pos = buffer_.position_of(e.offset + text.size());
        cursors_.set_primary(new_pos);
    } else {
        // Multi-cursor: collect new positions (forward order)
        std::vector<Position> new_positions;
        // edits are in reverse offset order; reverse to get forward order
        int64_t offset_delta = 0;
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            size_t actual_offset = static_cast<size_t>(
                static_cast<int64_t>(it->offset) + offset_delta);
            new_positions.push_back(buffer_.position_of(actual_offset + text.size()));
            offset_delta += static_cast<int64_t>(text.size()) -
                            static_cast<int64_t>(it->delete_len);
        }
        // Set cursors
        if (!new_positions.empty()) {
            cursors_.set_primary(new_positions[0]);
            for (size_t i = 1; i < new_positions.size(); ++i) {
                cursors_.add_cursor(new_positions[i]);
            }
        }
    }

    on_text_changed();
}

void Editor::delete_backward() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    auto indices = reverse_sorted_indices(sels, buffer_);

    std::vector<Position> new_positions;

    for (size_t idx : indices) {
        const Selection& sel = sels[idx];
        if (!sel.empty()) {
            Position mn = sel.min();
            size_t offset = buffer_.offset_of(mn.line, mn.col);
            size_t end_offset = buffer_.offset_of(sel.max().line, sel.max().col);
            buffer_.erase(offset, end_offset - offset);
            new_positions.push_back(mn);
        } else {
            size_t offset = buffer_.offset_of(sel.head.line, sel.head.col);
            if (offset == 0) {
                new_positions.push_back(sel.head);
                continue;
            }
            size_t n = 1;  // at column 0 this joins with the previous line
            if (sel.head.col > 0) {
                std::string line = buffer_.line_text(sel.head.line);
                n = sel.head.col - utf8::prev_boundary(line, sel.head.col);
            }
            Position new_pos = buffer_.position_of(offset - n);
            buffer_.erase(offset - n, n);
            new_positions.push_back(new_pos);
        }
    }

    buffer_.end_undo_group();

    // Positions were collected in reverse order
    std::reverse(new_positions.begin(), new_positions.end());
    if (!new_positions.empty()) {
        cursors_.set_primary(new_positions[0]);
        for (size_t i = 1; i < new_positions.size(); ++i) {
            cursors_.add_cursor(new_positions[i]);
        }
    }

    on_text_changed();
}

void Editor::delete_forward() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    auto indices = reverse_sorted_indices(sels, buffer_);

    std::vector<Position> new_positions;

    for (size_t idx : indices) {
        const Selection& sel = sels[idx];
        if (!sel.empty()) {
            Position mn = sel.min();
            size_t offset = buffer_.offset_of(mn.line, mn.col);
            size_t end_offset = buffer_.offset_of(sel.max().line, sel.max().col);
            buffer_.erase(offset, end_offset - offset);
            new_positions.push_back(mn);
        } else {
            size_t offset = buffer_.offset_of(sel.head.line, sel.head.col);
            if (offset >= buffer_.length()) {
                new_positions.push_back(sel.head);
                continue;
            }
            size_t n = 1;
            std::string line = buffer_.line_text(sel.head.line);
            if (sel.head.col < line.size())
                n = utf8::next_boundary(line, sel.head.col) - sel.head.col;
            buffer_.erase(offset, n);
            new_positions.push_back(sel.head);
        }
    }

    buffer_.end_undo_group();

    std::reverse(new_positions.begin(), new_positions.end());
    if (!new_positions.empty()) {
        cursors_.set_primary(new_positions[0]);
        for (size_t i = 1; i < new_positions.size(); ++i) {
            cursors_.add_cursor(new_positions[i]);
        }
    }

    on_text_changed();
}

void Editor::delete_word_backward() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    auto indices = reverse_sorted_indices(sels, buffer_);

    std::vector<Position> new_positions;

    for (size_t idx : indices) {
        const Selection& sel = sels[idx];
        if (!sel.empty()) {
            Position mn = sel.min();
            size_t offset = buffer_.offset_of(mn.line, mn.col);
            size_t end_offset = buffer_.offset_of(sel.max().line, sel.max().col);
            buffer_.erase(offset, end_offset - offset);
            new_positions.push_back(mn);
            continue;
        }

        size_t offset = buffer_.offset_of(sel.head.line, sel.head.col);
        if (offset == 0) {
            new_positions.push_back(sel.head);
            continue;
        }

        size_t start = offset - 1;
        // Skip whitespace backwards
        auto is_word = [&](size_t o) -> bool {
            char c = buffer_.char_at(o);
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
        };
        while (start > 0 && !is_word(start)) --start;
        while (start > 0 && is_word(start - 1)) --start;
        if (!is_word(start) && start < offset - 1) ++start;

        // If we only moved over non-word chars, delete at least one
        if (start >= offset) start = offset - 1;

        buffer_.erase(start, offset - start);
        new_positions.push_back(buffer_.position_of(start));
    }

    buffer_.end_undo_group();

    std::reverse(new_positions.begin(), new_positions.end());
    if (!new_positions.empty()) {
        cursors_.set_primary(new_positions[0]);
        for (size_t i = 1; i < new_positions.size(); ++i) {
            cursors_.add_cursor(new_positions[i]);
        }
    }

    on_text_changed();
}

void Editor::delete_word_forward() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    auto indices = reverse_sorted_indices(sels, buffer_);

    std::vector<Position> new_positions;

    for (size_t idx : indices) {
        const Selection& sel = sels[idx];
        if (!sel.empty()) {
            Position mn = sel.min();
            size_t offset = buffer_.offset_of(mn.line, mn.col);
            size_t end_offset = buffer_.offset_of(sel.max().line, sel.max().col);
            buffer_.erase(offset, end_offset - offset);
            new_positions.push_back(mn);
            continue;
        }

        size_t offset = buffer_.offset_of(sel.head.line, sel.head.col);
        size_t total = buffer_.length();
        if (offset >= total) {
            new_positions.push_back(sel.head);
            continue;
        }

        size_t end = offset;
        auto is_word = [&](size_t o) -> bool {
            char c = buffer_.char_at(o);
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
        };

        if (is_word(end)) {
            while (end < total && is_word(end)) ++end;
        }
        while (end < total && !is_word(end)) ++end;

        if (end == offset) end = offset + 1; // delete at least one char

        buffer_.erase(offset, end - offset);
        new_positions.push_back(sel.head);
    }

    buffer_.end_undo_group();

    std::reverse(new_positions.begin(), new_positions.end());
    if (!new_positions.empty()) {
        cursors_.set_primary(new_positions[0]);
        for (size_t i = 1; i < new_positions.size(); ++i) {
            cursors_.add_cursor(new_positions[i]);
        }
    }

    on_text_changed();
}

void Editor::delete_line() {
    buffer_.begin_undo_group();

    // Collect unique lines to delete (from cursors), process in reverse order
    std::vector<size_t> lines;
    for (auto& sel : cursors_.selections()) {
        lines.push_back(sel.head.line);
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    // Delete from bottom to top
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        size_t line = *it;
        size_t start = buffer_.line_start(line);
        size_t end;
        if (line + 1 < buffer_.line_count()) {
            end = buffer_.line_start(line + 1);
        } else {
            end = buffer_.length();
            // If not the first line, also delete the preceding newline
            if (start > 0) --start;
        }
        if (end > start) {
            buffer_.erase(start, end - start);
        }
    }

    buffer_.end_undo_group();

    // Position cursor at the first deleted line (or last line if at end)
    size_t new_line = lines.empty() ? 0 : lines[0];
    if (new_line >= buffer_.line_count()) {
        new_line = buffer_.line_count() > 0 ? buffer_.line_count() - 1 : 0;
    }
    cursors_.set_primary({new_line, 0});

    on_text_changed();
}

void Editor::duplicate_line() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    // Collect lines, process bottom-to-top so offsets stay valid
    std::vector<size_t> lines;
    for (auto& sel : sels) {
        lines.push_back(sel.head.line);
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        size_t line = *it;
        std::string lt = buffer_.line_text(line);
        // Insert after the line (including newline)
        size_t insert_offset;
        if (line + 1 < buffer_.line_count()) {
            insert_offset = buffer_.line_start(line + 1);
        } else {
            insert_offset = buffer_.length();
            // Add a newline first if the last line doesn't end with one
            lt = "\n" + lt;
        }
        buffer_.insert(insert_offset, lt + "\n");
    }

    buffer_.end_undo_group();

    // Move cursors down one line
    for (size_t i = 0; i < lines.size(); ++i) {
        // Adjust: after duplication, cursor should be on the new (duplicated) line
    }
    // Simple: move primary cursor down
    Position pos = cursors_.primary_pos();
    if (pos.line + 1 < buffer_.line_count()) {
        cursors_.set_primary({pos.line + 1, pos.col});
    }

    on_text_changed();
}

void Editor::move_line_up() {
    Position pos = cursors_.primary_pos();
    if (pos.line == 0) return;

    buffer_.begin_undo_group();

    size_t cur_line = pos.line;
    size_t prev_line = cur_line - 1;

    std::string cur_text = buffer_.line_text(cur_line);
    std::string prev_text = buffer_.line_text(prev_line);

    // Erase both lines and reinsert swapped
    size_t start = buffer_.line_start(prev_line);
    size_t end;
    if (cur_line + 1 < buffer_.line_count()) {
        end = buffer_.line_start(cur_line + 1);
    } else {
        end = buffer_.length();
    }

    std::string replacement;
    replacement += cur_text + "\n" + prev_text;
    if (cur_line + 1 < buffer_.line_count()) {
        replacement += "\n";
    }

    buffer_.erase(start, end - start);
    buffer_.insert(start, replacement);

    buffer_.end_undo_group();

    cursors_.set_primary({prev_line, pos.col});
    on_text_changed();
}

void Editor::move_line_down() {
    Position pos = cursors_.primary_pos();
    if (pos.line + 1 >= buffer_.line_count()) return;

    buffer_.begin_undo_group();

    size_t cur_line = pos.line;
    size_t next_line = cur_line + 1;

    std::string cur_text = buffer_.line_text(cur_line);
    std::string next_text = buffer_.line_text(next_line);

    size_t start = buffer_.line_start(cur_line);
    size_t end;
    if (next_line + 1 < buffer_.line_count()) {
        end = buffer_.line_start(next_line + 1);
    } else {
        end = buffer_.length();
    }

    std::string replacement;
    replacement += next_text + "\n" + cur_text;
    if (next_line + 1 < buffer_.line_count()) {
        replacement += "\n";
    }

    buffer_.erase(start, end - start);
    buffer_.insert(start, replacement);

    buffer_.end_undo_group();

    cursors_.set_primary({next_line, pos.col});
    on_text_changed();
}

void Editor::newline() {
    // Auto-indent: copy leading whitespace from current line
    Position pos = cursors_.primary_pos();
    std::string line = buffer_.line_text(pos.line);

    std::string indent;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            indent += c;
        } else {
            break;
        }
    }

    insert_text("\n" + indent);
}

void Editor::tab() {
    // If there's a selection, indent; otherwise insert spaces
    if (!cursors_.primary().empty()) {
        indent();
    } else {
        insert_text("    ");
    }
}

void Editor::indent() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    // Collect lines covered by selections
    std::vector<size_t> lines;
    for (auto& sel : sels) {
        size_t start_line = sel.min().line;
        size_t end_line = sel.max().line;
        // If selection ends at column 0, don't include that line
        if (sel.max().col == 0 && end_line > start_line) --end_line;
        for (size_t l = start_line; l <= end_line; ++l) {
            lines.push_back(l);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    // Insert 4 spaces at the beginning of each line, bottom to top
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        size_t offset = buffer_.line_start(*it);
        buffer_.insert(offset, "    ");
    }

    buffer_.end_undo_group();
    on_text_changed();
}

void Editor::unindent() {
    buffer_.begin_undo_group();

    auto& sels = cursors_.selections();
    std::vector<size_t> lines;
    for (auto& sel : sels) {
        size_t start_line = sel.min().line;
        size_t end_line = sel.max().line;
        if (sel.max().col == 0 && end_line > start_line) --end_line;
        for (size_t l = start_line; l <= end_line; ++l) {
            lines.push_back(l);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    // Remove up to 4 leading spaces from each line, bottom to top
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string lt = buffer_.line_text(*it);
        size_t to_remove = 0;
        for (size_t i = 0; i < 4 && i < lt.size(); ++i) {
            if (lt[i] == ' ') ++to_remove;
            else if (lt[i] == '\t') { ++to_remove; break; }
            else break;
        }
        if (to_remove > 0) {
            size_t offset = buffer_.line_start(*it);
            buffer_.erase(offset, to_remove);
        }
    }

    buffer_.end_undo_group();
    on_text_changed();
}

void Editor::toggle_comment() {
    buffer_.begin_undo_group();

    // Determine comment prefix based on language
    std::string lang = syntax_.language_id();
    std::string comment_prefix = "// ";
    if (lang == "python" || lang == "ruby" || lang == "shell" || lang == "bash") {
        comment_prefix = "# ";
    } else if (lang == "lua") {
        comment_prefix = "-- ";
    } else if (lang == "html" || lang == "xml") {
        // For simplicity, use // for html/xml (block comments are complex)
        comment_prefix = "// ";
    }

    auto& sels = cursors_.selections();
    std::vector<size_t> lines;
    for (auto& sel : sels) {
        size_t start_line = sel.min().line;
        size_t end_line = sel.max().line;
        if (sel.max().col == 0 && end_line > start_line) --end_line;
        for (size_t l = start_line; l <= end_line; ++l) {
            lines.push_back(l);
        }
    }
    if (lines.empty()) {
        lines.push_back(cursors_.primary_pos().line);
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    // Check if all lines are already commented
    bool all_commented = true;
    for (size_t line : lines) {
        std::string lt = buffer_.line_text(line);
        // Find first non-whitespace
        size_t ws = 0;
        while (ws < lt.size() && (lt[ws] == ' ' || lt[ws] == '\t')) ++ws;
        if (ws < lt.size()) {
            std::string trimmed_prefix = comment_prefix;
            // Also match without trailing space
            while (!trimmed_prefix.empty() && trimmed_prefix.back() == ' ') {
                trimmed_prefix.pop_back();
            }
            if (lt.substr(ws, comment_prefix.size()) != comment_prefix &&
                lt.substr(ws, trimmed_prefix.size()) != trimmed_prefix) {
                all_commented = false;
                break;
            }
        }
    }

    // Process bottom to top
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        size_t line = *it;
        std::string lt = buffer_.line_text(line);
        size_t ws = 0;
        while (ws < lt.size() && (lt[ws] == ' ' || lt[ws] == '\t')) ++ws;

        if (all_commented) {
            // Remove comment prefix
            size_t offset = buffer_.line_start(line) + ws;
            if (lt.substr(ws, comment_prefix.size()) == comment_prefix) {
                buffer_.erase(offset, comment_prefix.size());
            } else {
                // Try without trailing space
                std::string trimmed = comment_prefix;
                while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
                if (lt.substr(ws, trimmed.size()) == trimmed) {
                    buffer_.erase(offset, trimmed.size());
                }
            }
        } else {
            // Add comment prefix
            size_t offset = buffer_.line_start(line) + ws;
            buffer_.insert(offset, comment_prefix);
        }
    }

    buffer_.end_undo_group();
    on_text_changed();
}

void Editor::scroll_to_cursor(float view_width, float view_height,
                               float char_w, float line_h) {
    Position pos = cursors_.primary_pos();
    float cursor_x = static_cast<float>(utf8::display_col(buffer_.line_text(pos.line), pos.col)) * char_w;
    float cursor_y = static_cast<float>(pos.line) * line_h;

    // Margins shrink with the view so tiny panes don't oscillate between both edges.
    float margin_x = std::min(char_w * 4, view_width * 0.25f);
    float margin_y = std::min(line_h * 3, std::max(0.0f, (view_height - line_h) * 0.5f));

    // Horizontal scroll
    if (cursor_x < scroll_x_ + margin_x) {
        scroll_x_ = std::max(0.0f, cursor_x - margin_x);
    }
    if (cursor_x > scroll_x_ + view_width - margin_x) {
        scroll_x_ = cursor_x - view_width + margin_x;
    }

    // Vertical scroll
    if (cursor_y < scroll_y_ + margin_y) {
        scroll_y_ = std::max(0.0f, cursor_y - margin_y);
    }
    if (cursor_y + line_h > scroll_y_ + view_height - margin_y) {
        scroll_y_ = cursor_y + line_h - view_height + margin_y;
    }
}

std::string Editor::copy_text(bool& line_mode) const {
    const auto& sels = cursors_.selections();
    line_mode = std::all_of(sels.begin(), sels.end(), [](const Selection& s) { return s.empty(); });
    std::string out;
    for (size_t i = 0; i < sels.size(); i++) {
        if (i) out += '\n';
        if (line_mode) {
            out += buffer_.line_text(sels[i].head.line);
            out += '\n';
            continue;
        }
        size_t a = buffer_.offset_of(sels[i].min().line, sels[i].min().col);
        size_t b = buffer_.offset_of(sels[i].max().line, sels[i].max().col);
        for (size_t o = a; o < b;) {
            std::string_view chunk = buffer_.chunk_at(o);
            size_t n = std::min(chunk.size(), b - o);
            out.append(chunk.data(), n);
            o += n;
        }
    }
    if (line_mode && sels.size() > 1) out.pop_back();  // keep one newline per copied line
    return out;
}

std::string Editor::cut_text(bool& line_mode) {
    std::string text = copy_text(line_mode);
    if (line_mode) {
        buffer_.begin_undo_group();
        // Remove whole lines bottom-up so earlier offsets stay valid.
        std::vector<size_t> lines;
        for (auto& s : cursors_.selections()) lines.push_back(s.head.line);
        std::sort(lines.rbegin(), lines.rend());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        for (size_t line : lines) {
            size_t start = buffer_.line_start(line);
            size_t len = buffer_.line_length(line);
            bool has_newline = line + 1 < buffer_.line_count();
            if (!has_newline && start > 0) { start--; len++; }  // last line: eat the preceding newline
            else if (has_newline) len++;
            buffer_.erase(start, len);
        }
        buffer_.end_undo_group();
        Position p = cursors_.primary_pos();
        cursors_.set_primary({std::min(p.line, buffer_.line_count() - 1), 0});
        on_text_changed();
    } else {
        delete_backward();  // deletes the selections
    }
    return text;
}

void Editor::paste_text(const std::string& text, bool line_mode) {
    if (line_mode && cursors_.selections().size() == 1 && cursors_.primary().empty()) {
        Position p = cursors_.primary_pos();
        size_t at = buffer_.line_start(p.line);
        buffer_.begin_undo_group();
        buffer_.insert(at, text);
        buffer_.end_undo_group();
        size_t added = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
        cursors_.set_primary({p.line + added, p.col});
        on_text_changed();
        return;
    }
    insert_text(text);
}

void Editor::undo() {
    if (auto off = buffer_.undo()) cursors_.set_primary(buffer_.position_of(*off));
    on_text_changed();
}

void Editor::redo() {
    if (auto off = buffer_.redo()) cursors_.set_primary(buffer_.position_of(*off));
    on_text_changed();
}

void Editor::on_text_changed() {
    sync_syntax();
}

void Editor::sync_syntax() {
    if (!syntax_.poll()) return;  // background parse running; edits stay queued
    syntax_.apply_changes(buffer_, buffer_.take_changes());
}

} // namespace oh
