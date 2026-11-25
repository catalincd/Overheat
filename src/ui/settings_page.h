#pragma once
#include "ui/panel.h"
#include "anim.h"
#include <functional>

namespace oh {

// Modal settings card. Rows are supplied by the host as closures so the page
// stays free of app knowledge.
class SettingsPage : public Panel {
public:
    struct Row {
        enum Kind { Toggle, Stepper, Choice, Button } kind;
        std::string label;
        std::string description;
        std::function<std::string()> value;     // shown on the right (Choice/Stepper)
        std::function<bool()> on;               // Toggle state
        std::function<void(int)> change;        // Toggle: 0, Stepper: -1/+1, Choice/Button: 0
    };
    void open(std::vector<Row> rows) { rows_ = std::move(rows); visible_ = true; selected_ = 0; appear_.snap(0); }
    void close() { visible_ = false; }
    bool visible() const { return visible_; }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    std::vector<Row> rows_;
    std::vector<Rect> row_rects_, minus_rects_, plus_rects_;
    Rect card_{};
    bool visible_ = false;
    int selected_ = 0;
    float scroll_ = 0;
    anim::Smooth appear_;
    void activate(int i, int delta);
};

} // namespace oh
