#pragma once
// Debug Console (bottom panel): program and adapter output plus a REPL that
// evaluates in the selected stack frame. Up/Down walk the input history.
#include "ui/panel.h"
#include <string>
#include <vector>

namespace oh {

class Debugger;

class DebugConsole : public Panel {
public:
    void set_debugger(Debugger* d) { dbg_ = d; }
    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool wants_focus() const override { return true; }
    void scroll_to_end() { scroll_ = 0; }

private:
    Debugger* dbg_ = nullptr;
    std::string input_;
    size_t caret_ = 0;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    float scroll_ = 0;  // lines scrolled up from the bottom
    uint64_t seen_gen_ = 0;
    float line_h_ = 18.0f;
    size_t visible_lines_ = 10;
};

} // namespace oh
