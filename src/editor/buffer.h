#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

// Piece table text buffer for efficient editing of large files
class TextBuffer {
public:
    TextBuffer();
    void load_string(const std::string& text);
    bool load_file(const fs::path& path);
    bool save_file(const fs::path& path);

    // Editing operations
    void insert(size_t offset, const std::string& text);
    void erase(size_t offset, size_t length);
    void replace(size_t offset, size_t length, const std::string& text);

    // Accessors
    std::string text() const;
    std::string line_text(size_t line) const;
    char char_at(size_t offset) const;
    size_t length() const;
    size_t line_count() const;

    // Position conversion
    size_t offset_of(size_t line, size_t col) const;
    Position position_of(size_t offset) const;
    size_t line_start(size_t line) const;
    size_t line_length(size_t line) const;

    // Undo/redo
    void begin_undo_group();
    void end_undo_group();
    bool undo();
    bool redo();
    bool is_modified() const { return modified_; }
    void set_unmodified() { modified_ = false; save_point_ = undo_stack_.size(); }

    const fs::path& file_path() const { return file_path_; }
    void set_file_path(const fs::path& p) { file_path_ = p; }

private:
    enum class Source { Original, Add };
    struct Piece {
        Source source;
        size_t start;
        size_t length;
    };
    struct Edit {
        size_t piece_idx;
        std::vector<Piece> old_pieces;
        std::vector<Piece> new_pieces;
        size_t offset;
    };
    struct UndoGroup {
        std::vector<Edit> edits;
    };

    std::string original_buf_;
    std::string add_buf_;
    std::vector<Piece> pieces_;
    mutable std::vector<size_t> line_offsets_;
    mutable bool line_cache_dirty_ = true;

    std::vector<UndoGroup> undo_stack_;
    std::vector<UndoGroup> redo_stack_;
    UndoGroup* current_group_ = nullptr;
    size_t save_point_ = 0;
    bool modified_ = false;
    fs::path file_path_;

    void rebuild_line_cache() const;
    const char* piece_text(const Piece& p) const;
    size_t piece_offset(size_t piece_idx) const;
    std::pair<size_t, size_t> find_piece(size_t offset) const;
    void split_piece(size_t piece_idx, size_t local_offset);
};

} // namespace oh
