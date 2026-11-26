#include "editor/buffer.h"
#include <fstream>
#include <algorithm>
#include <cassert>
#include <cstring>

namespace oh {

TextBuffer::TextBuffer() {
    // Start with a single empty piece referencing original buffer
    pieces_.push_back({Source::Original, 0, 0});
}

void TextBuffer::load_string(const std::string& text) {
    original_buf_ = text;
    add_buf_.clear();
    pieces_.clear();
    if (text.empty()) {
        pieces_.push_back({Source::Original, 0, 0});
    } else {
        pieces_.push_back({Source::Original, 0, text.size()});
    }
    line_cache_dirty_ = true;
    undo_stack_.clear();
    redo_stack_.clear();
    changes_.clear();
    version_++;
    log_edit(0);
    current_group_ = nullptr;
    save_point_ = 0;
    modified_ = false;
}

bool TextBuffer::load_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    auto size = f.tellg();
    f.seekg(0);
    std::string contents(static_cast<size_t>(size), '\0');
    f.read(contents.data(), size);
    if (!f) return false;

    load_string(contents);
    file_path_ = path;
    return true;
}

bool TextBuffer::save_file(const fs::path& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    std::string content = text();
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) return false;

    file_path_ = path;
    set_unmodified();
    return true;
}

const char* TextBuffer::piece_text(const Piece& p) const {
    if (p.source == Source::Original)
        return original_buf_.data() + p.start;
    else
        return add_buf_.data() + p.start;
}

size_t TextBuffer::piece_offset(size_t piece_idx) const {
    size_t offset = 0;
    for (size_t i = 0; i < piece_idx; ++i) {
        offset += pieces_[i].length;
    }
    return offset;
}

std::pair<size_t, size_t> TextBuffer::find_piece(size_t offset) const {
    size_t cumulative = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (offset <= cumulative + pieces_[i].length) {
            // If offset is exactly at the end of this piece, and it's not the last piece,
            // point to the start of the next piece (unless this is an empty piece or the last)
            if (offset == cumulative + pieces_[i].length && i + 1 < pieces_.size()) {
                return {i + 1, 0};
            }
            return {i, offset - cumulative};
        }
        cumulative += pieces_[i].length;
    }
    // Past the end: return last piece at its end
    if (pieces_.empty()) return {0, 0};
    return {pieces_.size() - 1, pieces_.back().length};
}

void TextBuffer::split_piece(size_t piece_idx, size_t local_offset) {
    if (local_offset == 0 || local_offset == pieces_[piece_idx].length) return;

    Piece& p = pieces_[piece_idx];
    Piece left = {p.source, p.start, local_offset};
    Piece right = {p.source, p.start + local_offset, p.length - local_offset};

    pieces_[piece_idx] = left;
    pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(piece_idx) + 1, right);
}

void TextBuffer::log_edit(size_t start) {
    if (edit_log_.size() >= 512) edit_log_.erase(edit_log_.begin(), edit_log_.begin() + 256);
    edit_log_.push_back({version_, start});
}

size_t TextBuffer::earliest_change_since(uint64_t since) const {
    if (since >= version_) return SIZE_MAX;
    if (edit_log_.empty() || edit_log_.front().first > since + 1) return 0;
    size_t lo = SIZE_MAX;
    for (auto it = edit_log_.rbegin(); it != edit_log_.rend() && it->first > since; ++it) lo = std::min(lo, it->second);
    return lo;
}

void TextBuffer::record_change(size_t start, const std::string& removed, const std::string& inserted) {
    version_++;
    log_edit(start);
    Position s = position_of(start);
    auto advance = [](Position p, const std::string& t) {
        for (char c : t) {
            if (c == '\n') { p.line++; p.col = 0; }
            else p.col++;
        }
        return p;
    };
    changes_.push_back({start, removed.size(), inserted.size(), s, advance(s, removed), advance(s, inserted)});
}

void TextBuffer::insert(size_t offset, const std::string& text) {
    if (text.empty() || read_only_) return;
    record_change(offset, "", text);

    size_t add_start = add_buf_.size();
    add_buf_.append(text);

    Piece new_piece = {Source::Add, add_start, text.size()};

    auto [pi, local] = find_piece(offset);

    if (local == 0) {
        // Insert before this piece
        pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(pi), new_piece);
    } else if (local == pieces_[pi].length) {
        // Insert after this piece
        pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(pi) + 1, new_piece);
    } else {
        // Split the piece and insert in the middle
        Piece& p = pieces_[pi];
        Piece left = {p.source, p.start, local};
        Piece right = {p.source, p.start + local, p.length - local};

        pieces_[pi] = left;
        pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(pi) + 1, right);
        pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(pi) + 1, new_piece);
    }

    if (current_group_) current_group_->edits.push_back({offset, "", text});

    line_cache_dirty_ = true;
    modified_ = true;
}

void TextBuffer::erase(size_t offset, size_t len) {
    if (len == 0 || read_only_) return;
    std::string removed;
    removed.reserve(len);
    for (size_t i = 0; i < len; i++) removed.push_back(char_at(offset + i));
    record_change(offset, removed, "");

    size_t erase_end = offset + len;

    // Find start piece and end piece
    auto [start_pi, start_local] = find_piece(offset);
    auto [end_pi, end_local] = find_piece(erase_end);

    // Build replacement pieces
    std::vector<Piece> replacements;

    // Left remainder of start piece
    if (start_local > 0) {
        replacements.push_back({pieces_[start_pi].source, pieces_[start_pi].start, start_local});
    }

    // Right remainder of end piece
    if (end_pi < pieces_.size() && end_local < pieces_[end_pi].length) {
        replacements.push_back({pieces_[end_pi].source, pieces_[end_pi].start + end_local,
                                pieces_[end_pi].length - end_local});
    }

    // Replace the range of pieces
    auto begin_it = pieces_.begin() + static_cast<ptrdiff_t>(start_pi);
    auto end_it = (end_pi < pieces_.size())
                      ? pieces_.begin() + static_cast<ptrdiff_t>(end_pi) + 1
                      : pieces_.end();
    pieces_.erase(begin_it, end_it);
    pieces_.insert(pieces_.begin() + static_cast<ptrdiff_t>(start_pi),
                   replacements.begin(), replacements.end());

    // Ensure we always have at least one piece
    if (pieces_.empty()) {
        pieces_.push_back({Source::Original, 0, 0});
    }

    if (current_group_) current_group_->edits.push_back({offset, std::move(removed), ""});

    line_cache_dirty_ = true;
    modified_ = true;
}

void TextBuffer::replace(size_t offset, size_t length, const std::string& text) {
    erase(offset, length);
    insert(offset, text);
}

std::string TextBuffer::text() const {
    std::string result;
    size_t total = 0;
    for (auto& p : pieces_) total += p.length;
    result.reserve(total);
    for (auto& p : pieces_) {
        if (p.length > 0) {
            result.append(piece_text(p), p.length);
        }
    }
    return result;
}

std::string_view TextBuffer::chunk_at(size_t offset) const {
    auto [pi, local] = find_piece(offset);
    if (pi >= pieces_.size() || local >= pieces_[pi].length) return {};
    return {piece_text(pieces_[pi]) + local, pieces_[pi].length - local};
}

char TextBuffer::char_at(size_t offset) const {
    auto [pi, local] = find_piece(offset);
    if (pi >= pieces_.size() || local >= pieces_[pi].length) return '\0';
    return piece_text(pieces_[pi])[local];
}

size_t TextBuffer::length() const {
    size_t total = 0;
    for (auto& p : pieces_) total += p.length;
    return total;
}

void TextBuffer::rebuild_line_cache() const {
    line_offsets_.clear();
    line_offsets_.push_back(0); // Line 0 starts at offset 0

    size_t offset = 0;
    for (auto& p : pieces_) {
        const char* data = piece_text(p);
        for (size_t i = 0; i < p.length; ++i) {
            if (data[i] == '\n') {
                line_offsets_.push_back(offset + i + 1);
            }
        }
        offset += p.length;
    }

    line_cache_dirty_ = false;
}

size_t TextBuffer::line_count() const {
    if (line_cache_dirty_) rebuild_line_cache();
    return line_offsets_.size();
}

size_t TextBuffer::line_start(size_t line) const {
    if (line_cache_dirty_) rebuild_line_cache();
    if (line >= line_offsets_.size()) return length();
    return line_offsets_[line];
}

size_t TextBuffer::line_length(size_t line) const {
    if (line_cache_dirty_) rebuild_line_cache();
    if (line >= line_offsets_.size()) return 0;
    size_t start = line_offsets_[line];
    size_t end;
    if (line + 1 < line_offsets_.size()) {
        end = line_offsets_[line + 1];
    } else {
        end = length();
    }
    // Length includes the newline if present; return content length without it
    if (end > start && char_at(end - 1) == '\n') {
        return end - start - 1;
    }
    return end - start;
}

std::string TextBuffer::line_text(size_t line) const {
    if (line_cache_dirty_) rebuild_line_cache();
    if (line >= line_offsets_.size()) return "";

    size_t start = line_offsets_[line];
    size_t end;
    if (line + 1 < line_offsets_.size()) {
        end = line_offsets_[line + 1];
    } else {
        end = length();
    }

    // Don't include trailing newline
    if (end > start && char_at(end - 1) == '\n') {
        --end;
    }

    std::string result;
    result.reserve(end - start);

    size_t offset = 0;
    for (auto& p : pieces_) {
        size_t piece_start = offset;
        size_t piece_end = offset + p.length;

        if (piece_end > start && piece_start < end) {
            size_t copy_start = (start > piece_start) ? start - piece_start : 0;
            size_t copy_end = (end < piece_end) ? end - piece_start : p.length;
            result.append(piece_text(p) + copy_start, copy_end - copy_start);
        }

        offset += p.length;
        if (offset >= end) break;
    }

    return result;
}

size_t TextBuffer::offset_of(size_t line, size_t col) const {
    if (line_cache_dirty_) rebuild_line_cache();
    if (line >= line_offsets_.size()) return length();
    size_t start = line_offsets_[line];
    size_t ll = line_length(line);
    size_t clamped_col = std::min(col, ll);
    return start + clamped_col;
}

Position TextBuffer::position_of(size_t offset) const {
    if (line_cache_dirty_) rebuild_line_cache();
    if (line_offsets_.empty()) return {0, 0};

    // Binary search for line
    auto it = std::upper_bound(line_offsets_.begin(), line_offsets_.end(), offset);
    size_t line = 0;
    if (it != line_offsets_.begin()) {
        line = static_cast<size_t>(std::distance(line_offsets_.begin(), it)) - 1;
    }
    size_t col = offset - line_offsets_[line];
    return {line, col};
}

// Undo/Redo

void TextBuffer::begin_undo_group() {
    if (current_group_) return;
    undo_stack_.emplace_back();
    current_group_ = &undo_stack_.back();
    redo_stack_.clear();
}

void TextBuffer::end_undo_group() {
    if (!current_group_) return;
    if (current_group_->edits.empty()) {
        undo_stack_.pop_back();
    }
    current_group_ = nullptr;
}

std::optional<size_t> TextBuffer::undo() {
    if (undo_stack_.empty() || current_group_) return std::nullopt;
    UndoGroup group = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    size_t cursor = 0;
    for (auto it = group.edits.rbegin(); it != group.edits.rend(); ++it) {
        erase(it->offset, it->inserted.size());
        insert(it->offset, it->removed);
        cursor = it->offset + it->removed.size();
    }
    redo_stack_.push_back(std::move(group));
    modified_ = (undo_stack_.size() != save_point_);
    return cursor;
}

std::optional<size_t> TextBuffer::redo() {
    if (redo_stack_.empty() || current_group_) return std::nullopt;
    UndoGroup group = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    size_t cursor = 0;
    for (auto& e : group.edits) {
        erase(e.offset, e.removed.size());
        insert(e.offset, e.inserted);
        cursor = e.offset + e.inserted.size();
    }
    undo_stack_.push_back(std::move(group));
    modified_ = (undo_stack_.size() != save_point_);
    return cursor;
}

} // namespace oh
