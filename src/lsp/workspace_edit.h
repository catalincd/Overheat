#pragma once
// LSP WorkspaceEdit / TextEdit parsing and application as pure functions, so
// rename (and later code actions) can apply edits to buffers and unopened files
// alike, and tests can check them without a server.
#include "types.h"
#include "lsp/json.h"
#include <string>
#include <vector>

namespace oh {

struct LSPTextEdit {
    Position start{}, end{};  // columns in the negotiated encoding (UTF-8 bytes or UTF-16 units)
    std::string new_text;
};

struct LSPFileEdit {
    fs::path path;
    std::vector<LSPTextEdit> edits;
};

struct LSPWorkspaceEdit {
    std::vector<LSPFileEdit> files;  // in server order, one entry per file
    int skipped_ops = 0;             // create/rename/delete file operations (not supported)
    size_t edit_count() const {
        size_t n = 0;
        for (auto& f : files) n += f.edits.size();
        return n;
    }
};

// file:// URI -> path (percent-decoded); other schemes are returned unchanged.
fs::path lsp_uri_to_path(const std::string& uri);
// Range {start:{line,character}, end:{...}} -> positions.
void lsp_parse_range(const json::Value& range, Position& start, Position& end);
// Accepts `changes` and `documentChanges` (TextDocumentEdit and file operations).
LSPWorkspaceEdit parse_workspace_edit(const json::Value& edit);
std::vector<LSPTextEdit> parse_text_edits(const json::Value& arr);

// A resolved edit in byte offsets of one text.
struct ByteEdit {
    size_t offset, length;
    std::string text;
};
// Converts LSP positions to byte offsets in `text` (columns past a line end clamp to
// it, a line past the end means the end of the text) and sorts them. Fails, with a
// message, on overlapping edits.
bool resolve_text_edits(const std::string& text, const std::vector<LSPTextEdit>& edits, bool utf16,
                        std::vector<ByteEdit>& out, std::string* error = nullptr);
// Applies edits to `text` in place (all or nothing).
bool apply_text_edits(std::string& text, const std::vector<LSPTextEdit>& edits, bool utf16,
                      std::string* error = nullptr);
// Maps a byte offset in the old text through sorted edits to the new text.
size_t map_offset_through(size_t offset, const std::vector<ByteEdit>& sorted_edits);

} // namespace oh
