#include "keys.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace oh {

void KeyMap::init_defaults() {
    // File operations
    bind(SDLK_S,     KeyMod::Ctrl,                      "save");
    bind(SDLK_S,     KeyMod::Ctrl | KeyMod::Shift,      "save_as");
    bind(SDLK_N,     KeyMod::Ctrl,                      "new_file");
    bind(SDLK_O,     KeyMod::Ctrl,                      "open_file");

    // Tabs
    bind(SDLK_W,     KeyMod::Ctrl,                      "close_tab");
    bind(SDLK_TAB,   KeyMod::Ctrl,                      "quick_switch");
    bind(SDLK_TAB,   KeyMod::Ctrl | KeyMod::Shift,      "quick_switch_back");
    bind(SDLK_PAGEDOWN, KeyMod::Ctrl,                   "next_tab");
    bind(SDLK_PAGEUP,   KeyMod::Ctrl,                   "prev_tab");

    // Command palette
    bind(SDLK_P,     KeyMod::Ctrl,                      "command_palette_files");
    bind(SDLK_P,     KeyMod::Ctrl | KeyMod::Shift,      "command_palette_commands");

    // Panels
    bind(SDLK_GRAVE, KeyMod::Ctrl,                      "toggle_terminal");
    bind(SDLK_B,     KeyMod::Ctrl,                      "toggle_sidebar");
    bind(SDLK_R,     KeyMod::Ctrl,                      "workspace_switch");
    bind(SDLK_COMMA, KeyMod::Ctrl,                      "open_settings");
    bind(SDLK_I,     KeyMod::Ctrl | KeyMod::Alt,        "view_chat");
    bind(SDLK_Z,     KeyMod::Alt,                       "toggle_word_wrap");
    bind(SDLK_BACKSLASH, KeyMod::Ctrl,                  "split_editor");
    bind(SDLK_1,     KeyMod::Ctrl,                      "focus_group_1");
    bind(SDLK_2,     KeyMod::Ctrl,                      "focus_group_2");
    bind(SDLK_E,     KeyMod::Ctrl | KeyMod::Shift,      "view_explorer");
    bind(SDLK_G,     KeyMod::Ctrl | KeyMod::Shift,      "view_scm");
    bind(SDLK_F,     KeyMod::Ctrl | KeyMod::Shift,      "view_search");
    bind(SDLK_SPACE, KeyMod::Ctrl,                      "trigger_suggest");
    bind(SDLK_F12,   KeyMod::None,                      "goto_definition");

    // Undo/redo
    bind(SDLK_Z,     KeyMod::Ctrl,                      "undo");
    bind(SDLK_Z,     KeyMod::Ctrl | KeyMod::Shift,      "redo");
    bind(SDLK_Y,     KeyMod::Ctrl,                      "redo");

    // Selection
    bind(SDLK_C,     KeyMod::Ctrl,                      "copy");
    bind(SDLK_X,     KeyMod::Ctrl,                      "cut");
    bind(SDLK_V,     KeyMod::Ctrl,                      "paste");
    bind(SDLK_C,     KeyMod::Ctrl | KeyMod::Shift,      "copy");
    bind(SDLK_V,     KeyMod::Ctrl | KeyMod::Shift,      "paste");
    bind(SDLK_INSERT,KeyMod::Shift,                     "paste");
    bind(SDLK_D,     KeyMod::Ctrl,                      "select_word");
    bind(SDLK_L,     KeyMod::Ctrl,                      "select_line");
    bind(SDLK_A,     KeyMod::Ctrl,                      "select_all");

    // Editing
    bind(SDLK_SLASH, KeyMod::Ctrl,                      "toggle_comment");
    bind(SDLK_K,     KeyMod::Ctrl | KeyMod::Shift,      "delete_line");
    bind(SDLK_UP,    KeyMod::Ctrl | KeyMod::Alt,        "add_cursor_above");
    bind(SDLK_DOWN,  KeyMod::Ctrl | KeyMod::Alt,        "add_cursor_below");
    bind(SDLK_UP,    KeyMod::Alt,                       "move_line_up");
    bind(SDLK_DOWN,  KeyMod::Alt,                       "move_line_down");
    bind(SDLK_D,     KeyMod::Ctrl | KeyMod::Shift,      "duplicate_line");

    // Navigation
    bind(SDLK_G,     KeyMod::Ctrl,                      "goto_line");
    bind(SDLK_F,     KeyMod::Ctrl,                      "find");
    bind(SDLK_H,     KeyMod::Ctrl,                      "replace");

    // Indentation
    bind(SDLK_I,     KeyMod::Ctrl | KeyMod::Shift,      "reindent_selection");
    bind(SDLK_I,     KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt, "reindent_file");

    // Window
    bind(SDLK_F11,   KeyMod::None,                      "toggle_fullscreen");
    bind(SDLK_Q,     KeyMod::Ctrl,                      "quit");

    // Zoom
    bind(SDLK_PLUS,  KeyMod::Ctrl,                      "zoom_in");
    bind(SDLK_EQUALS,KeyMod::Ctrl,                      "zoom_in");
    bind(SDLK_MINUS, KeyMod::Ctrl,                      "zoom_out");
    bind(SDLK_0,     KeyMod::Ctrl,                      "zoom_reset");
    bind(SDLK_EQUALS,KeyMod::Ctrl | KeyMod::Shift,      "zoom_in");   // Ctrl++ on US layouts
    bind(SDLK_MINUS, KeyMod::Ctrl | KeyMod::Shift,      "zoom_out");
    bind(SDLK_KP_PLUS,  KeyMod::Ctrl,                   "zoom_in");
    bind(SDLK_KP_MINUS, KeyMod::Ctrl,                   "zoom_out");
    bind(SDLK_KP_0,     KeyMod::Ctrl,                   "zoom_reset");
}

void KeyMap::bind(SDL_Keycode key, KeyMod mods, const std::string& command) {
    bindings_[{key, mods}] = command;
}

std::string KeyMap::lookup(SDL_Keycode key, KeyMod mods) const {
    auto it = bindings_.find({key, mods});
    if (it != bindings_.end()) return it->second;
    return {};
}

// Helpers for serialization
static std::string keycode_to_string(SDL_Keycode k) {
    const char* name = SDL_GetKeyName(k);
    if (name && name[0]) return name;
    return std::to_string(static_cast<int>(k));
}

static SDL_Keycode string_to_keycode(const std::string& s) {
    SDL_Keycode k = SDL_GetKeyFromName(s.c_str());
    if (k != SDLK_UNKNOWN) return k;
    // Fallback: try parsing as integer
    try { return static_cast<SDL_Keycode>(std::stoi(s)); }
    catch (...) { return SDLK_UNKNOWN; }
}

static std::string mods_to_string(KeyMod m) {
    std::string s;
    if (m & KeyMod::Ctrl)  s += "Ctrl+";
    if (m & KeyMod::Shift) s += "Shift+";
    if (m & KeyMod::Alt)   s += "Alt+";
    if (m & KeyMod::Super) s += "Super+";
    return s;
}

static KeyMod parse_mods(const std::string& s) {
    KeyMod m = KeyMod::None;
    if (s.find("Ctrl") != std::string::npos)  m = m | KeyMod::Ctrl;
    if (s.find("Shift") != std::string::npos) m = m | KeyMod::Shift;
    if (s.find("Alt") != std::string::npos)   m = m | KeyMod::Alt;
    if (s.find("Super") != std::string::npos) m = m | KeyMod::Super;
    return m;
}

bool KeyMap::load(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::vector<std::pair<KeyCombo, std::string>> entries;
    std::string line;
    while (std::getline(f, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;

        // Simple JSON-like format: "key_combo": "command"
        // Find the colon separator
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key_part = line.substr(0, colon);
        std::string cmd_part = line.substr(colon + 1);

        // Strip quotes and whitespace
        auto strip = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '"' || s.front() == '\t' || s.front() == ','))
                s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\t' || s.back() == ',' || s.back() == '{' || s.back() == '}'))
                s.pop_back();
        };
        strip(key_part);
        strip(cmd_part);

        if (key_part.empty() || cmd_part.empty()) continue;

        // Parse key combo: e.g. "Ctrl+Shift+S"
        KeyMod mods = parse_mods(key_part);

        // The last segment after the last '+' is the key name
        auto last_plus = key_part.rfind('+');
        std::string key_name;
        if (last_plus != std::string::npos) {
            key_name = key_part.substr(last_plus + 1);
        } else {
            key_name = key_part;
        }

        SDL_Keycode kc = string_to_keycode(key_name);
        if (kc != SDLK_UNKNOWN) entries.push_back({{kc, mods}, cmd_part});
    }
    for (auto& [combo, cmd] : entries) {
        if (std::find(user_commands_.begin(), user_commands_.end(), cmd) != user_commands_.end()) continue;
        user_commands_.push_back(cmd);
        std::erase_if(bindings_, [&](const auto& kv) { return kv.second == cmd; });
    }
    for (auto& [combo, cmd] : entries) bindings_[combo] = cmd;
    return true;
}

void KeyMap::rebind(const std::string& command, SDL_Keycode key, KeyMod mods) {
    std::erase_if(bindings_, [&](const auto& kv) { return kv.second == command; });
    bindings_[{key, mods}] = command;  // also steals the chord from any other command
    if (std::find(user_commands_.begin(), user_commands_.end(), command) == user_commands_.end())
        user_commands_.push_back(command);
}

std::string KeyMap::chord_to_string(SDL_Keycode key, KeyMod mods) {
    return mods_to_string(mods) + keycode_to_string(key);
}

std::string KeyMap::binding_for(const std::string& command) const {
    std::string best;
    for (auto& [combo, cmd] : bindings_) {
        if (cmd != command) continue;
        std::string s = chord_to_string(combo.key, combo.mods);
        if (best.empty() || s.size() < best.size()) best = s;
    }
    return best;
}

bool KeyMap::save(const fs::path& path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    bool first = true;
    for (auto& [combo, cmd] : bindings_) {
        if (std::find(user_commands_.begin(), user_commands_.end(), cmd) == user_commands_.end()) continue;
        if (!first) f << ",\n";
        first = false;

        std::string mod_str = mods_to_string(combo.mods);
        std::string key_str = keycode_to_string(combo.key);
        f << "  \"" << mod_str << key_str << "\": \"" << cmd << "\"";
    }
    f << "\n}\n";
    return true;
}

} // namespace oh
