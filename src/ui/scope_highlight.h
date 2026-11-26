#pragma once
#include "types.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oh {

class Editor;
class TextBuffer;
struct RenderContext;

// 4coder-style structure highlighting for one editor view:
//  - brackets coloured by nesting depth (strings and comments skipped),
//  - a stacked background tint for each { } scope enclosing the cursor, a bar along
//    the innermost one, and a filled box on the bracket pair at the cursor,
//  - the identifier under the cursor and its visible occurrences marked,
//  - the line of the last jump (search hit, go to definition) marked.
// Bracket nesting comes from a small lexer with checkpoints every CHECKPOINT_LINES
// lines; an edit invalidates only checkpoints after it, and per frame only the
// visible lines (plus the stretch from a checkpoint to the cursor) are scanned.
class ScopeHighlight {
public:
    // Settings (settings.toml: bracket_colors, scope_highlight, occurrence_underline).
    static inline bool bracket_colors = true;
    static inline bool scopes = true;
    static inline bool occurrences = true;

    // Text geometry, supplied by the view so wrapping and scrolling are respected.
    struct Geometry {
        Rect text;
        float line_h, char_w;
        std::function<std::pair<float, float>(size_t line, size_t byte)> point;  // top-left of a byte
        std::function<float(size_t line)> line_top;  // top of a line's first visual row
    };

    // Brings the caches up to date for visible lines [first, end).
    void update(const Editor& ed, size_t first, size_t end);
    // Draws scope tints, pair boxes, occurrence marks and the jump line (below text).
    // Returns true when it drew the bracket-pair highlight (the view then skips its own).
    bool render(RenderContext& ctx, const Editor& ed, const Geometry& g);
    // Recolours the bracket bytes of a visible line.
    void color_line(size_t line, std::vector<Color>& byte_colors, const Theme& th) const;

    // Bracket bookkeeping (public for the lexer helpers in the .cpp).
    struct Open { char ch; size_t off; };
    struct LexState {
        uint8_t mode = 0;   // code, block comment, raw / multi-line string kinds
        uint8_t aux = 0;    // quote char or long-bracket level
        int nest = 0;       // nested block comment depth (Rust)
        std::string delim;  // raw string delimiter (C++ R"x(...)x", Rust r#"..."#)
        std::vector<Open> stack;
    };
    struct Bracket {
        size_t off;
        size_t line, col;
        char ch;
        int depth;     // nesting level (0 = outermost)
        bool error;    // unmatched or mismatched
    };

private:
    static constexpr size_t CHECKPOINT_LINES = 512;
    static constexpr size_t MAX_BYTES = 32u << 20;  // larger files: no structure highlighting

    const TextBuffer* buf_ = nullptr;
    std::string lang_;
    uint64_t version_ = UINT64_MAX;
    std::vector<LexState> checkpoints_;  // [k] = state at the start of line k * CHECKPOINT_LINES

    // Visible-range results.
    size_t vis_first_ = 0, vis_end_ = 0;
    uint64_t vis_version_ = UINT64_MAX;
    std::vector<Bracket> vis_;                       // brackets on visible lines, in order
    std::vector<Open> vis_start_stack_;              // opens still unclosed at the first visible line
    std::unordered_map<size_t, size_t> pair_;        // open offset <-> close offset (both ways)

    // Enclosing brackets at the cursor, outermost first.
    size_t cur_off_ = SIZE_MAX;
    uint64_t cur_version_ = UINT64_MAX;
    std::vector<Open> cur_stack_;

    bool enabled() const;
    const LexState& state_at_line(size_t line, size_t& from_line);
    void scan(LexState& st, size_t from_line, size_t to_line, size_t stop_off,
              std::vector<Bracket>* out, bool make_checkpoints);
};

} // namespace oh
