#pragma once
#include "types.h"

namespace oh {

class VulkanContext;
class Renderer2D;
class FontAtlas;

struct RenderContext {
    VulkanContext* vk;
    Renderer2D* r;
    FontAtlas* font;
    Theme* theme;
    float dt;
};

class Panel {
public:
    virtual ~Panel() = default;
    virtual void update(float dt) {}
    virtual void render(RenderContext& ctx, Rect bounds) = 0;
    virtual bool handle_key(const KeyEvent& e) { return false; }
    virtual bool handle_mouse(const MouseEvent& e) { return false; }
    virtual bool handle_text(const std::string& text) { return false; }
    virtual bool wants_focus() const { return false; }

    void set_bounds(Rect r) { bounds_ = r; }
    Rect bounds() const { return bounds_; }
    bool focused() const { return focused_; }
    void set_focused(bool f) { focused_ = f; }

protected:
    Rect bounds_ = {0, 0, 0, 0};
    bool focused_ = false;
};

} // namespace oh
