#pragma once
#include "ui/panel.h"
#include <functional>

namespace oh {

// VS Code-style Command Center: a slim title row with a search-box strip in the
// middle showing the open file and workspace ("app.cpp — Overheat"). Clicking the
// box opens Quick Open. Settings: command_center = false hides the row.
class CommandCenter : public Panel {
public:
    static constexpr float HEIGHT = 34.0f;

    void set_title(std::string t) { title_ = std::move(t); }
    const std::string& title() const { return title_; }
    void set_on_click(std::function<void()> cb) { on_click_ = std::move(cb); }
    Rect box() const { return box_; }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    std::string title_;
    std::function<void()> on_click_;
    Rect box_{};
};

} // namespace oh
