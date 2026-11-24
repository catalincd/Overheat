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
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool wants_focus() const override { return true; }

    float gutter_width(const FontAtlas& font) const;

private:
    std::shared_ptr<Editor> editor_;
    float cursor_blink_timer_ = 0;
    bool cursor_visible_ = true;
    bool mouse_selecting_ = false;

    void render_gutter(RenderContext& ctx, Rect gutter_rect);
    void render_text(RenderContext& ctx, Rect text_rect);
    void render_cursors(RenderContext& ctx, Rect text_rect);
    void render_selections(RenderContext& ctx, Rect text_rect);
    void render_minimap(RenderContext& ctx, Rect minimap_rect);
    Position pixel_to_position(float x, float y, Rect text_rect, const FontAtlas& font) const;
};

} // namespace oh
