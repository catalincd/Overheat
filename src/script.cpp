#include "script.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace oh {

bool ScriptRunner::load(const fs::path& path, SDL_Window* window) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "script: cannot open %s\n", path.c_str());
        return false;
    }
    for (std::string line; std::getline(in, line);) {
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        lines_.push_back(line.substr(start));
    }
    window_id_ = SDL_GetWindowID(window);
    next_ = Clock::now() + std::chrono::milliseconds(500);  // let the first frame land
    return true;
}

int ScriptRunner::ms_until_next(TimePoint now) const {
    if (!active()) return -1;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_ - now).count();
    return ms < 0 ? 0 : static_cast<int>(ms);
}

void ScriptRunner::push_mouse_button(float x, float y, bool down, int clicks) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = window_id_;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    e.button.clicks = static_cast<Uint8>(clicks);
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

void ScriptRunner::push_motion(float x, float y, bool held) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = window_id_;
    e.motion.state = held ? SDL_BUTTON_LMASK : 0;
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
}

void ScriptRunner::push_key(const std::string& combo) {
    SDL_Keymod mod = SDL_KMOD_NONE;
    std::string rest = combo;
    for (;;) {
        auto plus = rest.find('+');
        if (plus == std::string::npos || plus == rest.size() - 1) break;
        std::string m = rest.substr(0, plus);
        for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "ctrl") mod |= SDL_KMOD_LCTRL;
        else if (m == "shift") mod |= SDL_KMOD_LSHIFT;
        else if (m == "alt") mod |= SDL_KMOD_LALT;
        else if (m == "super") mod |= SDL_KMOD_LGUI;
        else break;
        rest = rest.substr(plus + 1);
    }
    SDL_Keycode key = rest.size() == 1
        ? static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(rest[0])))
        : SDL_GetKeyFromName(rest.c_str());
    if (key == SDLK_UNKNOWN) {
        std::fprintf(stderr, "script: unknown key '%s'\n", rest.c_str());
        return;
    }
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = window_id_;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    SDL_PushEvent(&e);
}

void ScriptRunner::push_text(const std::string& text) {
    text_storage_.push_back(text);
    if (text_storage_.size() > 64) text_storage_.pop_front();
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = window_id_;
    e.text.text = text_storage_.back().c_str();
    SDL_PushEvent(&e);
}

std::vector<std::string> ScriptRunner::step(TimePoint now) {
    std::vector<std::string> probes;
    while (active() && now >= next_) {
        std::istringstream ss(lines_[idx_++]);
        std::string cmd;
        ss >> cmd;
        int delay = 80;  // default spacing so each step gets its own frame
        if (cmd == "wait") {
            ss >> delay;
        } else if (cmd == "click" || cmd == "dclick") {
            float x, y;
            ss >> x >> y;
            push_motion(x, y, false);
            int clicks = cmd == "dclick" ? 2 : 1;
            push_mouse_button(x, y, true, clicks);
            push_mouse_button(x, y, false, clicks);
        } else if (cmd == "drag") {
            float x1, y1, x2, y2;
            ss >> x1 >> y1 >> x2 >> y2;
            push_mouse_button(x1, y1, true, 1);
            for (int i = 1; i <= 8; i++)
                push_motion(x1 + (x2 - x1) * i / 8.0f, y1 + (y2 - y1) * i / 8.0f, true);
            push_mouse_button(x2, y2, false, 1);
        } else if (cmd == "scroll") {
            float x, y, dy;
            ss >> x >> y >> dy;
            push_motion(x, y, false);
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = window_id_;
            e.wheel.y = dy;
            e.wheel.mouse_x = x;
            e.wheel.mouse_y = y;
            SDL_PushEvent(&e);
        } else if (cmd == "move") {  // pointer motion without a button (hover)
            float x, y;
            ss >> x >> y;
            push_motion(x, y, false);
        } else if (cmd == "mods") {  // held modifiers for later clicks: "mods ctrl", "mods none"
            std::string m;
            ss >> m;
            SDL_Keymod mod = SDL_KMOD_NONE;
            if (m.find("ctrl") != std::string::npos) mod |= SDL_KMOD_LCTRL;
            if (m.find("shift") != std::string::npos) mod |= SDL_KMOD_LSHIFT;
            if (m.find("alt") != std::string::npos) mod |= SDL_KMOD_LALT;
            SDL_SetModState(mod);
        } else if (cmd == "key") {
            std::string combo;
            ss >> combo;
            push_key(combo);
        } else if (cmd == "type") {
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            push_text(text);
        } else if (cmd == "probe") {
            std::string label;
            std::getline(ss, label);
            probes.push_back(label.empty() ? "probe" : label.substr(1));
            // A probe must observe the state before any later step's events are queued.
            next_ = now;
            return probes;
        } else if (cmd == "quit") {
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        } else {
            std::fprintf(stderr, "script: unknown command '%s'\n", cmd.c_str());
        }
        next_ = now + std::chrono::milliseconds(delay);
    }
    return probes;
}

} // namespace oh
