#pragma once
#include "ui/panel.h"
#include "editor/editor.h"
#include <memory>

namespace oh {

class EditorView : public Panel {
public:
    EditorView();

    void set_editor(std::shared_ptr<Editor> editor);
    std::shared_ptr<Editor> editor() const { return editor_; }

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt);
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
    float cached_line_h_ = 20.0f;
    float cached_char_w_ = 9.0f;
    float cached_gutter_w_ = 40.0f;

    void render_gutter(RenderContext& ctx, Rect gutter_rect);
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
