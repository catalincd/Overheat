#include "editor/editor.h"
#include "vfs.h"
#include "editor/utf8.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <sstream>

namespace oh {

namespace {
bool word_byte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }
char closer_for(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        case '`': return '`';
        default: return 0;
    }
}
} // namespace

Editor::Editor() = default;

void Editor::load_file(const fs::path& path) {
    std::string content;
    LocalVfs().read(path, content);
    load_content(path, content);
}

void Editor::load_content(const fs::path& path, const std::string& content) {
    buffer_.load_string(content);
    buffer_.set_file_path(path);
    cursors_.set_primary({0, 0});
    scroll_x_ = 0;
    scroll_y_ = 0;

    detect_indentation();
    std::string lang = large_file_ ? "" : SyntaxHighlighter::detect_language(path);
    syntax_.set_language(lang);
    constexpr size_t SYNC_PARSE_LIMIT = 256 * 1024;  // small files never show uncoloured
    if (large_file_) return;  // large-file mode: no syntax tree
    if (buffer_.length() <= SYNC_PARSE_LIMIT) syntax_.update(buffer_);
    else syntax_.parse_async(buffer_.text());
}

void Editor::save() {
    before_save();
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
    cached = syntax_.tokens_for_line(buffer_, start, len);
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
    if (read_only_) return;
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
    if (read_only_) return;
    virtual_extend_delete(true);
    if (cursors_.selections().size() == 1 && cursors_.primary().empty()) {
        Position p = cursors_.primary_pos();
        std::string l = buffer_.line_text(p.line);
        if (p.col > 0 && p.col < l.size() && closer_for(l[p.col - 1]) && closer_for(l[p.col - 1]) == l[p.col]) {
            size_t off = buffer_.offset_of(p.line, p.col - 1);
            buffer_.begin_undo_group();
            buffer_.erase(off, 2);  // "(|)" -> "|"
            buffer_.end_undo_group();
            cursors_.set_primary({p.line, p.col - 1});
            on_text_changed();
            return;
        }
    }
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
    if (read_only_) return;
    virtual_extend_delete(false);
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
    if (read_only_) return;
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
    if (read_only_) return;
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
    if (read_only_) return;
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
    if (read_only_) return;
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
    if (read_only_) return;
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
    if (read_only_) return;
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
    if (read_only_) return;
    if (indent_newline()) return;  // indent_mode auto / virtual
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
    if (read_only_) return;
    // If there's a selection, indent; otherwise insert spaces
    if (indent_tab()) return;  // indent_mode auto / virtual: Tab at the indent re-indents
    if (!cursors_.primary().empty()) {
        indent();
    } else {
        insert_text(indent_unit());
    }
}

void Editor::indent() {
    if (read_only_) return;
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

    // Insert one indent level at the beginning of each line, bottom to top
    std::string unit = IndentSettings::use_tabs ? std::string("\t") : std::string(IndentSettings::width, ' ');
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        size_t offset = buffer_.line_start(*it);
        buffer_.insert(offset, indent_unit());
    }

    buffer_.end_undo_group();
    on_text_changed();
}

void Editor::unindent() {
    if (read_only_) return;
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

    // Remove up to one indent level of leading spaces from each line, bottom to top
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string lt = buffer_.line_text(*it);
        size_t to_remove = 0;
        for (size_t i = 0; i < static_cast<size_t>(indent_style_.size) && i < lt.size(); ++i) {
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
    if (read_only_) return;
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
    float cursor_x = static_cast<float>(view_col(pos.line, buffer_.line_text(pos.line), pos.col)) * char_w;
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
    if (virtual_ws()) return virtual_copy(line_mode);  // with the computed indentation
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
    if (read_only_) return copy_text(line_mode);
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
    if (read_only_) return;
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

void Editor::replace_ranges(const std::vector<std::pair<size_t, size_t>>& ranges, const std::string& text) {
    if (read_only_) return;
    if (ranges.empty()) return;
    buffer_.begin_undo_group();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {  // back to front keeps offsets valid
        buffer_.erase(it->first, it->second);
        buffer_.insert(it->first, text);
    }
    buffer_.end_undo_group();
    cursors_.set_primary(buffer_.position_of(std::min(ranges.front().first, buffer_.length())));
    on_text_changed();
}

void Editor::goto_line(size_t line) {
    line = std::min(line, buffer_.line_count() - 1);
    cursors_.set_primary({line, 0});
}


void Editor::add_next_occurrence() {
    const auto& sels = cursors_.selections();
    if (sels.size() == 1 && sels[0].empty()) {
        cursors_.select_word(buffer_);
        return;
    }
    const Selection& last = sels.back();
    if (last.empty()) return;
    size_t a = buffer_.offset_of(last.min().line, last.min().col);
    size_t b = buffer_.offset_of(last.max().line, last.max().col);
    std::string text = buffer_.text();
    std::string needle = text.substr(a, b - a);
    size_t pos = text.find(needle, b);
    if (pos == std::string::npos) pos = text.find(needle);  // wrap around
    for (int guard = 0; pos != std::string::npos && guard < 100000; guard++) {
        bool taken = false;
        for (auto& s : sels)
            if (buffer_.offset_of(s.min().line, s.min().col) == pos) taken = true;
        if (!taken) {
            cursors_.add_selection({buffer_.position_of(pos), buffer_.position_of(pos + needle.size())});
            return;
        }
        pos = text.find(needle, pos + 1);
        if (pos == std::string::npos) return;  // every occurrence is already selected
    }
}

void Editor::add_cursor_vertical(int dir) {
    std::vector<Position> extra;
    for (auto& s : cursors_.selections()) {
        long line = static_cast<long>(s.head.line) + dir;
        if (line < 0 || line >= static_cast<long>(buffer_.line_count())) continue;
        std::string from = buffer_.line_text(s.head.line), to = buffer_.line_text(static_cast<size_t>(line));
        extra.push_back({static_cast<size_t>(line), view_byte(static_cast<size_t>(line), to, view_col(s.head.line, from, s.head.col))});
    }
    for (auto& p : extra) cursors_.add_cursor(p);
}

void Editor::type_text(const std::string& text) {
    if (read_only_) return;
    if (text.size() != 1) {
        insert_text(text);
        return;
    }
    char c = text[0];
    const auto& sels = cursors_.selections();
    bool all_empty = std::all_of(sels.begin(), sels.end(), [](const Selection& s) { return s.empty(); });
    auto next_char = [&](const Selection& s) {
        std::string l = buffer_.line_text(s.head.line);
        return s.head.col < l.size() ? l[s.head.col] : '\0';
    };
    auto prev_char = [&](const Selection& s) {
        std::string l = buffer_.line_text(s.head.line);
        return s.head.col > 0 && s.head.col <= l.size() ? l[s.head.col - 1] : '\0';
    };
    // Typing a closer that is already there just steps over it.
    bool is_closer = c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || c == '`';
    if (all_empty && is_closer && std::all_of(sels.begin(), sels.end(), [&](const Selection& s) { return next_char(s) == c; })) {
        cursors_.move_right(buffer_, false);
        return;
    }
    char close = auto_close_pairs ? closer_for(c) : 0;
    // Rust: ' mostly starts a lifetime ('a, 'static), so don't pair it (as VS Code).
    if (c == '\'' && syntax_.language_id() == "rust") close = 0;
    if (!close) {
        insert_text(text);
        return;
    }
    // Wrap a single selection: "(" around "foo" gives "(foo)" with foo still selected.
    if (sels.size() == 1 && !sels[0].empty()) {
        Selection s = sels[0];
        size_t a = buffer_.offset_of(s.min().line, s.min().col), b = buffer_.offset_of(s.max().line, s.max().col);
        buffer_.begin_undo_group();
        buffer_.insert(b, std::string(1, close));
        buffer_.insert(a, std::string(1, c));
        buffer_.end_undo_group();
        cursors_.set_primary(buffer_.position_of(a + 1));
        cursors_.extend_primary(buffer_.position_of(b + 1));
        on_text_changed();
        return;
    }
    bool quote = c == '"' || c == '\'' || c == '`';
    bool pair_ok = all_empty && std::all_of(sels.begin(), sels.end(), [&](const Selection& s) {
        char n = next_char(s);
        bool next_ok = n == '\0' || std::isspace(static_cast<unsigned char>(n)) || n == ')' || n == ']' ||
                       n == '}' || n == ',' || n == ';';
        bool prev_ok = !quote || !word_byte(static_cast<unsigned char>(prev_char(s)));  // don't pair it's
        return next_ok && prev_ok;
    });
    if (!pair_ok) {
        insert_text(text);
        return;
    }
    insert_text(std::string{c, close});
    cursors_.move_left(buffer_, false);
}

bool Editor::find_bracket_pair(Position& a, Position& b) const {
    if (large_file_) return false;  // would scan huge (minified) lines every frame
    Position cur = cursors_.primary_pos();
    std::string line = buffer_.line_text(cur.line);
    auto is_bracket = [](char ch) { return ch && std::strchr("()[]{}", ch); };
    size_t col;
    if (cur.col < line.size() && is_bracket(line[cur.col])) col = cur.col;
    else if (cur.col > 0 && cur.col <= line.size() && is_bracket(line[cur.col - 1])) col = cur.col - 1;
    else return false;
    char open = line[col];
    static const char* pairs = "()[]{}";
    const char* p = std::strchr(pairs, open);
    size_t idx = static_cast<size_t>(p - pairs);
    bool forward = idx % 2 == 0;
    char want = forward ? pairs[idx + 1] : pairs[idx - 1];
    int depth = 0;
    long l = static_cast<long>(cur.line);
    long c2 = static_cast<long>(col);
    for (int scanned = 0; scanned < 5000 && l >= 0 && l < static_cast<long>(buffer_.line_count()); scanned++) {
        std::string text = buffer_.line_text(static_cast<size_t>(l));
        if (scanned > 0) c2 = forward ? 0 : static_cast<long>(text.size()) - 1;
        for (; forward ? c2 < static_cast<long>(text.size()) : c2 >= 0; c2 += forward ? 1 : -1) {
            char ch = text[static_cast<size_t>(c2)];
            if (ch == open) depth++;
            else if (ch == want && --depth == 0) {
                a = {cur.line, col};
                b = {static_cast<size_t>(l), static_cast<size_t>(c2)};
                return true;
            }
        }
        l += forward ? 1 : -1;
    }
    return false;
}

void Editor::undo() {
    if (read_only_) return;
    if (auto off = buffer_.undo()) cursors_.set_primary(buffer_.position_of(*off));
    indent_hold_ = true;  // undo must not re-indent what it restores
    on_text_changed();
    indent_hold_ = false;
}

void Editor::redo() {
    if (read_only_) return;
    if (auto off = buffer_.redo()) cursors_.set_primary(buffer_.position_of(*off));
    indent_hold_ = true;
    on_text_changed();
    indent_hold_ = false;
}

void Editor::on_text_changed() {
    sync_syntax();        // the indenter reads the fresh tree
    indent_after_edit();  // indent_mode = auto: re-indent the edited lines (same undo step)
}

void Editor::set_git_base(std::optional<std::string> base) {
    git_base_ = std::move(base);
    git_marks_version_ = UINT64_MAX;
    git_marks_.clear();
}

const std::vector<GutterMark>& Editor::git_marks() {
    constexpr size_t MAX_DIFF_BYTES = 4u << 20;  // skip huge files; diffing is O(n) per edit
    if (!git_base_ || buffer_.version() == git_marks_version_) return git_marks_;
    git_marks_version_ = buffer_.version();
    if (buffer_.length() > MAX_DIFF_BYTES) {
        git_marks_.clear();
        return git_marks_;
    }
    git_marks_ = GitRepository::diff_marks(*git_base_, buffer_.text());
    return git_marks_;
}

const GitBlame* Editor::blame_for_line(size_t line, bool& uncommitted) {
    uncommitted = false;
    if (!git_base_ && !blame_job_.valid()) return nullptr;  // untracked, remote or untitled
    if (blame_job_.valid() && blame_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto raw = blame_job_.get();
        blame_.clear();
        for (auto& b : raw) {
            if (b.line == 0) continue;
            if (blame_.size() < b.line) blame_.resize(b.line);
            blame_[b.line - 1] = std::move(b);
        }
    }
    // Map the current line back to its line at HEAD through the diff hunks.
    long delta = 0;
    for (const GutterMark& m : git_marks()) {
        if (m.kind != GutterMark::Deleted && line >= m.line && line < m.line + m.count) {
            uncommitted = true;
            return nullptr;
        }
        bool before = m.kind == GutterMark::Deleted ? m.line <= line : m.line + m.count <= line;
        if (before) delta += static_cast<long>(m.count) - static_cast<long>(m.old_count);
    }
    long head_line = static_cast<long>(line) - delta;
    if (!git_base_) { uncommitted = true; return nullptr; }
    if (head_line < 0 || head_line >= static_cast<long>(blame_.size())) return nullptr;
    const GitBlame& b = blame_[static_cast<size_t>(head_line)];
    return b.line ? &b : nullptr;
}

void Editor::reload() {
    if (gate_ != filecheck::Verdict::Open) return;  // not loaded yet: nothing to refresh
    Position cur = cursors_.primary_pos();
    float sx = scroll_x_, sy = scroll_y_;
    fs::path path = buffer_.file_path();
    load_file(path);  // reload is for local files (git checkout); remote editors are not reloaded
    size_t line = std::min(cur.line, buffer_.line_count() - 1);
    cursors_.set_primary({line, std::min(cur.col, buffer_.line_length(line))});
    scroll_x_ = sx;
    scroll_y_ = sy;
}

// ---------------------------------------------------------------------------
// 4coder-style motion and syntax-aware selection
// ---------------------------------------------------------------------------

bool Editor::is_code() const {
    std::string lang = syntax_.language_id();
    return !lang.empty() && lang != "markdown";
}

void Editor::move_word_4coder(int dir, bool select) {
    cursors_.move_boundary(buffer_, dir, is_code() ? motion::Boundary::AlphaNumeric : motion::Boundary::Whitespace, select);
}

namespace {

bool same_selections(const std::vector<Selection>& a, const std::vector<Selection>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!(a[i].anchor == b[i].anchor) || !(a[i].head == b[i].head)) return false;
    return true;
}

bool same_range(TSNode n, size_t a, size_t b) { return ts_node_start_byte(n) == a && ts_node_end_byte(n) == b; }

// The named node a selection stands for: the smallest named node covering it, raised
// to the outermost ancestor with the same range (an identifier that is a whole
// expression statement's operand stays itself, but a lone child spanning its parent
// moves up to the parent, which is the level that has siblings).
TSNode node_for(TSNode root, size_t a, size_t b) {
    TSNode n = ts_node_named_descendant_for_byte_range(root, static_cast<uint32_t>(a), static_cast<uint32_t>(b));
    if (ts_node_is_null(n)) return n;
    for (TSNode p = ts_node_parent(n); !ts_node_is_null(p) && same_range(p, ts_node_start_byte(n), ts_node_end_byte(n));
         p = ts_node_parent(p))
        n = p;
    return n;
}

}  // namespace

bool Editor::expand_selection() {
    sync_syntax();
    const TSTree* tree = syntax_.tree();
    if (!tree) return false;
    TSNode root = ts_tree_root_node(tree);
    std::vector<Selection> before = cursors_.selections();
    if (!same_selections(before, expanded_)) expand_history_.clear();

    std::vector<Selection> out;
    for (const Selection& sel : before) {
        size_t a = buffer_.offset_of(sel.min().line, sel.min().col);
        size_t b = buffer_.offset_of(sel.max().line, sel.max().col);
        TSNode n = ts_node_named_descendant_for_byte_range(root, static_cast<uint32_t>(a), static_cast<uint32_t>(b));
        if (sel.empty() && a > 0) {
            // A caret right after a token (`foo|(`, `x;|`) means that token, as in VS Code.
            TSNode left = ts_node_named_descendant_for_byte_range(root, static_cast<uint32_t>(a - 1), static_cast<uint32_t>(a));
            auto span = [](TSNode x) { return ts_node_end_byte(x) - ts_node_start_byte(x); };
            if (!ts_node_is_null(left) && ts_node_end_byte(left) == a && (ts_node_is_null(n) || span(left) < span(n))) n = left;
        }
        // Climb until the node is strictly larger than the selection (a cursor at a
        // spot no named node covers lands on the first enclosing one).
        while (!ts_node_is_null(n) && !sel.empty() && ts_node_start_byte(n) >= a && ts_node_end_byte(n) <= b)
            n = ts_node_parent(n);
        if (ts_node_is_null(n)) {
            out.push_back(sel);
            continue;
        }
        out.push_back({buffer_.position_of(ts_node_start_byte(n)), buffer_.position_of(ts_node_end_byte(n))});
    }
    cursors_.set_selections(std::move(out));
    if (same_selections(cursors_.selections(), before)) return true;  // already the whole file
    expand_history_.push_back(std::move(before));
    expanded_ = cursors_.selections();
    return true;
}

bool Editor::shrink_selection() {
    if (!expand_history_.empty() && same_selections(cursors_.selections(), expanded_)) {
        cursors_.set_selections(expand_history_.back());
        expand_history_.pop_back();
        expanded_ = cursors_.selections();
        return true;
    }
    expand_history_.clear();
    sync_syntax();
    const TSTree* tree = syntax_.tree();
    if (!tree) return false;
    TSNode root = ts_tree_root_node(tree);
    // No history (the selection came from elsewhere): step into the named child on
    // the head's side of a selection that is exactly one node.
    std::vector<Selection> out;
    for (const Selection& sel : cursors_.selections()) {
        size_t a = buffer_.offset_of(sel.min().line, sel.min().col);
        size_t b = buffer_.offset_of(sel.max().line, sel.max().col);
        TSNode n = sel.empty() ? TSNode{} : node_for(root, a, b);
        if (ts_node_is_null(n) || !same_range(n, a, b)) {
            out.push_back(sel);
            continue;
        }
        bool head_first = sel.head < sel.anchor;
        while (ts_node_named_child_count(n) > 0 && same_range(n, a, b))
            n = ts_node_named_child(n, head_first ? 0 : ts_node_named_child_count(n) - 1);
        if (same_range(n, a, b)) {
            out.push_back(sel);  // a leaf: nothing smaller
            continue;
        }
        out.push_back({buffer_.position_of(ts_node_start_byte(n)), buffer_.position_of(ts_node_end_byte(n))});
    }
    cursors_.set_selections(std::move(out));
    return true;
}

bool Editor::select_sibling(int dir) {
    sync_syntax();
    const TSTree* tree = syntax_.tree();
    if (!tree) return false;
    TSNode root = ts_tree_root_node(tree);
    expand_history_.clear();
    std::vector<Selection> out;
    for (const Selection& sel : cursors_.selections()) {
        size_t a = buffer_.offset_of(sel.min().line, sel.min().col);
        size_t b = buffer_.offset_of(sel.max().line, sel.max().col);
        TSNode n = node_for(root, a, b);
        if (ts_node_is_null(n) || ts_node_eq(n, root)) {
            out.push_back(sel);
            continue;
        }
        TSNode s = dir > 0 ? ts_node_next_named_sibling(n) : ts_node_prev_named_sibling(n);
        if (ts_node_is_null(s)) {
            out.push_back(sel);  // first / last child: stay
            continue;
        }
        Position s0 = buffer_.position_of(ts_node_start_byte(s)), s1 = buffer_.position_of(ts_node_end_byte(s));
        // The head leads in the direction of travel, so repeated presses keep it in view.
        out.push_back(dir > 0 ? Selection{s0, s1} : Selection{s1, s0});
    }
    cursors_.set_selections(std::move(out));
    return true;
}

void Editor::sync_syntax() {
    if (!syntax_.poll()) return;  // background parse running; edits stay queued
    syntax_.apply_changes(buffer_, buffer_.take_changes());
}

// ---------------------------------------------------------------------------
// File format: indentation, line endings, BOM, language mode
// ---------------------------------------------------------------------------

std::string Editor::indent_unit() const {
    return indent_style_.tabs ? std::string("\t") : std::string(static_cast<size_t>(std::max(1, indent_style_.size)), ' ');
}

void Editor::detect_indentation() {
    // Like VS Code: tab- vs space-indented lines decide the kind; the most common
    // positive step between consecutive space-indented lines decides the width.
    // Files with nothing to detect use the tab_width / insert_spaces settings.
    indent_style_ = {IndentSettings::use_tabs, IndentSettings::width};
    size_t tab_lines = 0, space_lines = 0;
    int steps[9] = {};
    size_t prev = 0;
    size_t n = std::min<size_t>(buffer_.line_count(), 2000);
    for (size_t l = 0; l < n; l++) {
        std::string lt = buffer_.line_text(l);
        if (lt.find_first_not_of(" \t\r") == std::string::npos) continue;  // blank lines say nothing
        if (lt[0] == '\t') {
            tab_lines++;
            continue;
        }
        size_t sp = lt.find_first_not_of(' ');
        if (sp > 0) space_lines++;
        if (sp > prev && sp - prev <= 8 && sp - prev >= 2) steps[sp - prev]++;
        prev = sp;
    }
    if (tab_lines > space_lines) {
        indent_style_.tabs = true;
        return;
    }
    if (space_lines) indent_style_.tabs = false;
    int best = 0;
    for (int w : {2, 4, 8, 3})
        if (steps[w] > steps[best]) best = w;
    if (best) indent_style_.size = best;
}

void Editor::replace_all_text(const std::string& text) {
    Position p = cursors_.primary_pos();
    buffer_.begin_undo_group();
    buffer_.replace(0, buffer_.length(), text);
    buffer_.end_undo_group();
    on_text_changed();
    size_t line = std::min(p.line, buffer_.line_count() - 1);
    cursors_.set_primary({line, std::min(p.col, buffer_.line_length(line))});
}

void Editor::convert_indentation(IndentStyle to) {
    IndentStyle from = indent_style_;
    int from_w = std::max(1, from.size), to_w = std::max(1, to.size);
    std::string text = buffer_.text(), out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        // Leading whitespace as a visual column, then re-emitted in the new style.
        int col = 0;
        size_t j = i;
        for (; j < text.size() && (text[j] == ' ' || text[j] == '\t'); j++)
            col = text[j] == '\t' ? (col / from_w + 1) * from_w : col + 1;
        size_t levels = static_cast<size_t>(col / from_w), rem = static_cast<size_t>(col % from_w);
        if (to.tabs) out.append(levels, '\t');
        else out.append(levels * static_cast<size_t>(to_w), ' ');
        out.append(rem, ' ');
        size_t nl = text.find('\n', j);
        size_t end = nl == std::string::npos ? text.size() : nl + 1;
        out.append(text, j, end - j);
        i = end;
    }
    indent_style_ = to;
    if (out != text) replace_all_text(out);
}

bool Editor::uses_crlf() const {
    if (buffer_.line_count() < 2) return false;
    std::string first = buffer_.line_text(0);
    return !first.empty() && first.back() == '\r';
}

void Editor::set_line_endings(bool crlf) {
    std::string text = buffer_.text(), out;
    out.reserve(text.size() + (crlf ? buffer_.line_count() : 0));
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;  // re-added below
        if (c == '\n' && crlf) out += '\r';
        out += c;
    }
    if (out != text) replace_all_text(out);
}

bool Editor::has_bom() const {
    std::string_view head = buffer_.chunk_at(0);
    return head.size() >= 3 && head.substr(0, 3) == "\xEF\xBB\xBF";
}

void Editor::set_bom(bool on) {
    if (on == has_bom()) return;
    buffer_.begin_undo_group();
    if (on) buffer_.insert(0, "\xEF\xBB\xBF");
    else buffer_.erase(0, 3);
    buffer_.end_undo_group();
    on_text_changed();
}

void Editor::set_language(const std::string& lang_id) {
    syntax_.set_language(lang_id);
    buffer_.take_changes();  // the fresh parse below already covers them
    syntax_.update(buffer_);
}

} // namespace oh
