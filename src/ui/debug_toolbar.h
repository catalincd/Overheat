#pragma once
// The floating debug toolbar at the top of the editor while a session runs:
// continue/pause, step over, step into, step out, restart, stop.
#include "ui/panel.h"
#include <string>
#include <utility>
#include <vector>

namespace oh {

class Debugger;

class DebugToolbar {
public:
    void render(RenderContext& ctx, Rect editor, const Debugger& dbg);
    // The command under (x, y) ("debug_step_over", ...), or "" outside the buttons.
    std::string hit(float x, float y) const;
    bool contains(float x, float y) const { return visible_ && rect_.contains(x, y); }
    void hide() { visible_ = false; }

private:
    Rect rect_{};
    bool visible_ = false;
    std::vector<std::pair<Rect, std::string>> buttons_;
};

} // namespace oh
