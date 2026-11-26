#pragma once
// "Run and Debug" sidebar view: launch configuration picker and start button,
// then Variables, Watch, Call Stack and Breakpoints sections.
#include "ui/panel.h"
#include <functional>
#include <string>
#include <vector>

namespace oh {

class Debugger;
struct DebugVariable;

class DebugPanel : public Panel {
public:
    struct Callbacks {
        std::function<void()> pick_config;
        std::function<void()> open_launch_file;
        std::function<void()> start;            // F5
        std::function<void()> add_watch;        // asks for an expression
        std::function<void(size_t)> edit_watch;
        std::function<void(const fs::path&, size_t line)> open_location;
        std::function<void(const fs::path&, size_t line)> edit_breakpoint;  // condition / log message
        std::function<void()> add_function_breakpoint;
    };
    void set_debugger(Debugger* d) { dbg_ = d; }
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_mouse(const MouseEvent& e) override;
    // Row texts as drawn last frame (for test probes).
    const std::vector<std::string>& last_rows() const { return row_text_; }

private:
    Debugger* dbg_ = nullptr;
    Callbacks cb_;
    struct Hit {
        Rect rect;
        std::function<void(int button, bool dbl)> fn;
        bool in_list = true;  // a row of the scrolling list (not the configuration bar)
    };
    std::vector<Hit> hits_;
    std::vector<std::string> row_text_;
    float scroll_y_ = 0, content_h_ = 0;
    float row_h_ = 22.0f;
    enum Section { Variables, Watch, CallStack, Breakpoints, SectionCount };
    bool collapsed_[SectionCount] = {};

    // Layout state while rendering.
    RenderContext* ctx_ = nullptr;
    float y_ = 0;
    Rect list_{};
    void row_bg(Rect r, bool selected);
    float baseline(float y) const;
    void section_header(Section s, const char* title, const std::string& extra,
                        std::function<void()> add = {});
    void variable_rows(const DebugVariable& v, const std::string& path, int depth, int guard);
    void name_value(float x, float y, float right, const std::string& name, const std::string& value, bool error);
    void checkbox(float x, float y, bool on);
    // Records a row for probes: its centre y on screen and its text.
    void note(const std::string& text) {
        row_text_.push_back("y=" + std::to_string(static_cast<int>(y_ + row_h_ * 0.5f)) + " " + text);
    }
};

} // namespace oh
