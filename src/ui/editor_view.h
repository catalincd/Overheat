#pragma once
#include "ui/panel.h"
#include "editor/editor.h"
#include "ui/find_bar.h"
#include "ui/completion_popup.h"
#include "ui/debug_marks.h"
#include "renderer/font_atlas.h"
#include "anim.h"
#include "ui/scope_highlight.h"
#include <functional>
#include <unordered_map>
#include <memory>
#include <optional>

namespace oh {

class EditorView : public Panel {
public:
    EditorView();

    void set_editor(std::shared_ptr<Editor> editor);
    std::shared_ptr<Editor> editor() const { return editor_; }

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt);
    void toggle_blame() { show_blame_ = !show_blame_; }
    void set_wrap(bool on) { wrap_ = on; wrap_version_ = UINT64_MAX; }
    bool wrap() const { return wrap_; }
    bool blame() const { return show_blame_; }
    void set_minimap(bool on) { show_minimap_ = on; }
    bool minimap() const { return show_minimap_; }
    void open_find(bool replace) { if (editor_) find_.open(*editor_, replace); reveal_cursor(); }
    bool find_has_focus() const { return find_.has_input_focus(); }
    void set_diagnostics(const std::vector<LSPDiagnostic>* d) { diagnostics_ = d; }
    CompletionPopup& completion() { return completion_; }
    void reveal_cursor();
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool wants_focus() const override { return true; }

    // Binary / large file card (file_gate_card.cpp): "Open Anyway" calls this.
    void set_on_open_anyway(std::function<void(Editor&)> fn) { on_open_anyway_ = std::move(fn); }

    float gutter_width(const FontAtlas& font) const;
    // Debugger: marks for this editor's file (nullptr = none) and gutter clicks on
    // the breakpoint lane (button 1) or anywhere in the gutter (button 3).
    void set_debug_marks(const DebugMarks* m) { debug_marks_ = m; }
    std::function<void(size_t line, int button)> on_gutter_click;
    static constexpr float DEBUG_LANE_W = 16.0f;
    float last_gutter_width() const { return cached_gutter_w_; }

    // LSP navigation (nav_glue.cpp). Screen geometry of buffer positions:
    std::optional<Position> char_at_point(float x, float y) const;  // character cell under the pointer
    Rect char_rect(Position p) const;  // screen cell of p (as last rendered)
    Rect text_area() const;
    float line_height() const { return cached_line_h_; }
    // An inline zone (the peek view) of `height` pixels below `after_line`; lines
    // after it move down. The owner draws into zone_rect() after render().
    void set_zone(std::optional<size_t> after_line, float height);
    Rect zone_rect() const { return zone_rect_; }
    // Ctrl+hover link: underline [start, end) on one line.
    void set_link(std::optional<std::pair<Position, Position>> range) { link_ = range; }
    // Inlay hints (sorted by position, byte columns): drawn inline without changing
    // buffer columns. Ignored while word wrap is on.
    void set_inlay_hints(const std::vector<LSPInlayHint>* hints) { inlays_ = hints; }

private:
    std::shared_ptr<Editor> editor_;
    float cursor_blink_timer_ = 0;
    bool cursor_visible_ = true;
    bool mouse_selecting_ = false;
    bool show_blame_ = true;
    anim::Spring smooth_x_, smooth_y_;
    float view_x_ = 0, view_y_ = 0;  // rendered (animated) scroll
    const Editor* anim_editor_ = nullptr;
    FindBar find_;
    // Shaped glyph runs per line text (ligatures); cleared when the font changes.
    std::unordered_map<std::string, std::vector<FontAtlas::Shaped>> shape_cache_;
    const FontAtlas* shape_font_ = nullptr;
    float shape_font_size_ = 0;
    bool shape_ligatures_ = true;
    uint64_t shape_generation_ = 0;
    CompletionPopup completion_;
    ScopeHighlight scope_;  // bracket depth colours, scope tints, occurrence marks
    void render_scope_highlight(RenderContext& ctx, Rect text_rect, bool& drew_pair);
    const std::vector<LSPDiagnostic>* diagnostics_ = nullptr;
    void render_diagnostics(RenderContext& ctx, Rect text_rect);
    void render_scrollbar(RenderContext& ctx, Rect text_rect);
    void render_indent_guides(RenderContext& ctx, Rect text_rect);
    Rect ruler_{};
    bool dragging_thumb_ = false;
    float drag_offset_ = 0;
    float max_scroll() const;
    static constexpr float RULER_W = 14.0f;
    static constexpr float MINIMAP_W = 96.0f;
    static constexpr float MINIMAP_LINE_H = 2.0f;
    bool show_minimap_ = true;
    bool minimap_drag_ = false;
    Rect minimap_rect_{};
    float minimap_width() const;
    size_t minimap_first_line(Rect r) const;
    bool line_has_diagnostic(size_t line) const;
    void render_find_matches(RenderContext& ctx, Rect text_rect);
    float cached_line_h_ = 20.0f;
    float cached_char_w_ = 9.0f;
    float cached_gutter_w_ = 40.0f;

    void render_gutter(RenderContext& ctx, Rect gutter_rect);
    const DebugMarks* debug_marks_ = nullptr;
    void render_debug_lines(RenderContext& ctx, Rect text_rect);   // under the text
    void render_debug_gutter(RenderContext& ctx, Rect gutter_rect);
    void render_inline_values(RenderContext& ctx, Rect text_rect);
    void render_git_marks(RenderContext& ctx, Rect gutter_rect);
    void render_blame(RenderContext& ctx, Rect text_rect);
    void render_text(RenderContext& ctx, Rect text_rect);
    void render_cursors(RenderContext& ctx, Rect text_rect);
    void render_selections(RenderContext& ctx, Rect text_rect);
    void render_minimap(RenderContext& ctx, Rect minimap_rect);
    Position position_at(float x, float y) const;
    float row_y(size_t line, Rect text_rect) const;  // top of the line's first visual row
    // Word wrap: byte offsets where each visual row of a line starts ({0} if not wrapping).
    std::vector<size_t> segments(const std::string& text) const;
    size_t first_row(size_t line) const;
    size_t total_rows() const;
    std::pair<size_t, size_t> line_at_row(size_t row) const;  // (line, segment)
    struct Pt { float x, y; };
    Pt point(const std::string& text, size_t line, size_t byte, Rect text_rect) const;
    void update_wrap_map(float text_width);
    bool wrap_ = false;
    bool wrapping() const;
    size_t wrap_cols_ = 0;
    std::vector<size_t> row_start_;  // row_start_[line] = first visual row; back() = total
    uint64_t wrap_version_ = UINT64_MAX;
    size_t wrap_map_cols_ = 0;
    float col_x(const std::string& line_text, size_t byte_col, Rect text_rect) const;
    // Line text as drawn: lines over MAX_DRAWN_LINE bytes (minified files) are cut
    // there, so shaping and per-byte work stay bounded however long the line is.
    static constexpr size_t MAX_DRAWN_LINE = 64 * 1024;
    std::string view_line(size_t line) const;
    // Virtual whitespace (indent_mode = "virtual") is drawn when not word-wrapping;
    // dcol() is the display column of a byte column either way.
    bool virt() const { return editor_ && !wrapping() && editor_->virtual_ws(); }
    size_t dcol(const std::string& text, size_t line, size_t byte) const;
    std::pair<size_t, size_t> visible_lines(Rect text_rect) const;  // [first, end)

    static constexpr float TEXT_PAD = 4.0f;

    size_t zone_line_ = SIZE_MAX;
    float zone_h_ = 0;
    Rect zone_rect_{};
    float zone_dy(size_t line) const { return line != SIZE_MAX && zone_line_ != SIZE_MAX && line > zone_line_ ? zone_h_ : 0.0f; }
    std::optional<std::pair<Position, Position>> link_;
    const std::vector<LSPInlayHint>* inlays_ = nullptr;
    std::pair<const LSPInlayHint*, const LSPInlayHint*> inlays_on(size_t line) const;
    // Cells of hints drawn before byte `byte` of `line` (inclusive: also those at it).
    size_t inlay_cells(size_t line, size_t byte, bool inclusive) const;
    void render_inlay_hints(RenderContext& ctx, Rect text_rect);

    std::function<void(Editor&)> on_open_anyway_;
    Rect gate_button_{};
    bool gated() const;
    void render_gate_card(RenderContext& ctx, Rect bounds);
    bool gate_key(const KeyEvent& e);
    bool gate_mouse(const MouseEvent& e);
};

} // namespace oh
