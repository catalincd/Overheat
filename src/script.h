#pragma once
#include "types.h"
#include <SDL3/SDL.h>
#include <deque>
#include <string>
#include <vector>

namespace oh {

// Replays a test script by injecting SDL events into the app's own queue.
//   wait MS | click X Y | dclick X Y | drag X1 Y1 X2 Y2 | scroll X Y DY
//   key COMBO (e.g. ctrl+shift+p, Down, Return) | type TEXT | probe LABEL | quit
class ScriptRunner {
public:
    bool load(const fs::path& path, SDL_Window* window);
    bool active() const { return idx_ < lines_.size(); }
    // Pushes due events. Returns the probe label when a probe step is reached.
    std::vector<std::string> step(TimePoint now);
    // Milliseconds until the next step is due (for the idle wait timeout).
    int ms_until_next(TimePoint now) const;

private:
    void push_mouse_button(float x, float y, bool down, int clicks, int button = SDL_BUTTON_LEFT);
    void push_motion(float x, float y, bool held);
    void push_key(const std::string& combo);
    void push_text(const std::string& text);

    std::vector<std::string> lines_;
    size_t idx_ = 0;
    TimePoint next_ = {};
    SDL_WindowID window_id_ = 0;
    std::deque<std::string> text_storage_;  // SDL_TextInputEvent points into this
};

} // namespace oh
