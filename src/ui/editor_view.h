#pragma once
#include "ui/panel.h"
#include "editor/editor.h"
#include "ui/find_bar.h"
#include "ui/completion_popup.h"
#include "renderer/font_atlas.h"
#include <unordered_map>
#include <memory>

namespace oh {

class EditorView : public Panel {
public:
    EditorView();

    void set_editor(std::shared_ptr<Editor> editor);
    std::shared_ptr<Editor> editor() const { return editor_; }

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt);
    void toggle_blame() { show_blame_ = !show_blame_; }
    void open_find(bool replace) { if (editor_) find_.open(*editor_, replace); reveal_cursor(); }
    bool find_has_focus() const { return find_.has_input_focus(); }
    void set_diagnostics(const std::vector<LSPDiagnostic>* d) { diagnostics_ = d; }
    CompletionPopup& completion() { return completion_; }
    void reveal_cursor() {
        if (editor_) editor_->scroll_to_cursor(bounds_.w - cached_gutter_w_, bounds_.h, cached_char_w_, cached_line_h_);
    }
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool wants_focus() const override { return true; }

    float gutter_width(const FontAtlas& font) const;
    float last_gutter_width() const { return cached_gutter_w_; }

private:
    std::shared_ptr<Editor> editor_;
    float cursor_blink_timer_ = 0;
    bool cursor_visible_ = true;
    bool mouse_selecting_ = false;
    bool show_blame_ = true;
    FindBar find_;
    // Shaped glyph runs per line text (ligatures); cleared when the font changes.
    std::unordered_map<std::string, std::vector<FontAtlas::Shaped>> shape_cache_;
    const FontAtlas* shape_font_ = nullptr;
    float shape_font_size_ = 0;
    bool shape_ligatures_ = true;
    uint64_t shape_generation_ = 0;
    CompletionPopup completion_;
    const std::vector<LSPDiagnostic>* diagnostics_ = nullptr;
    void render_diagnostics(RenderContext& ctx, Rect text_rect);
    void render_scrollbar(RenderContext& ctx, Rect text_rect);
    Rect ruler_{};
    bool dragging_thumb_ = false;
    float drag_offset_ = 0;
    float max_scroll() const;
    static constexpr float RULER_W = 14.0f;
    bool line_has_diagnostic(size_t line) const;
    void render_find_matches(RenderContext& ctx, Rect text_rect);
    float cached_line_h_ = 20.0f;
    float cached_char_w_ = 9.0f;
    float cached_gutter_w_ = 40.0f;

    void render_gutter(RenderContext& ctx, Rect gutter_rect);
    void render_git_marks(RenderContext& ctx, Rect gutter_rect);
    void render_blame(RenderContext& ctx, Rect text_rect);
    void render_text(RenderContext& ctx, Rect text_rect);
    void render_cursors(RenderContext& ctx, Rect text_rect);
    void render_selections(RenderContext& ctx, Rect text_rect);
    void render_minimap(RenderContext& ctx, Rect minimap_rect);
    Position position_at(float x, float y) const;
    float row_y(size_t line, Rect text_rect) const;
    float col_x(const std::string& line_text, size_t byte_col, Rect text_rect) const;
    std::pair<size_t, size_t> visible_lines(Rect text_rect) const;  // [first, end)

    static constexpr float TEXT_PAD = 4.0f;
};

} // namespace oh
