#pragma once
#include "ui/panel.h"
#include <functional>
#include <string>

namespace oh {

// Inline single-line input shown over the symbol being renamed (F2), like VS Code's
// rename widget. Enter accepts, Escape cancels; the text starts fully selected.
class RenameBox {
public:
    void open(Rect anchor, const std::string& text, std::function<void(const std::string&)> on_accept,
              std::function<void()> on_cancel = {});
    void close() { visible_ = false; }
    bool visible() const { return visible_; }
    const std::string& text() const { return text_; }
    Rect rect() const { return rect_; }
    // While visible the box owns the keyboard: every key is consumed.
    bool handle_key(const KeyEvent& e);
    bool handle_text(const std::string& text);
    bool handle_mouse(const MouseEvent& e);  // clicks outside cancel
    void set_busy(bool b) { busy_ = b; }
    void render(RenderContext& ctx, Rect bounds);

private:
    std::string text_;
    size_t caret_ = 0, anchor_sel_ = 0;  // byte offsets; selection = [min, max)
    Rect anchor_{}, rect_{};
    bool visible_ = false, busy_ = false;
    std::function<void(const std::string&)> on_accept_;
    std::function<void()> on_cancel_;
    void erase_selection();
};

} // namespace oh
