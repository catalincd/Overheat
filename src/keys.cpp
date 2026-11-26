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

    // Chords
    bind_chord(SDLK_K, KeyMod::Ctrl, SDLK_T, KeyMod::Ctrl, "select_theme");
    bind_chord(SDLK_K, KeyMod::Ctrl, SDLK_S, KeyMod::Ctrl, "keybindings");
}

void KeyMap::bind_chord(SDL_Keycode k1, KeyMod m1, SDL_Keycode k2, KeyMod m2, const std::string& command) {
    chords_[{k1, m1}][{k2, m2}] = command;
}

bool KeyMap::is_chord_prefix(SDL_Keycode key, KeyMod mods) const {
    auto it = chords_.find({key, mods});
    return it != chords_.end() && !it->second.empty();
}

std::string KeyMap::lookup_chord(SDL_Keycode k1, KeyMod m1, SDL_Keycode k2, KeyMod m2) const {
    auto it = chords_.find({k1, m1});
    if (it == chords_.end()) return {};
    auto jt = it->second.find({k2, m2});
    return jt != it->second.end() ? jt->second : std::string();
}

void KeyMap::erase_command(const std::string& command) {
    std::erase_if(bindings_, [&](const auto& kv) { return kv.second == command; });
    for (auto& [prefix, second] : chords_)
        std::erase_if(second, [&](const auto& kv) { return kv.second == command; });
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

    struct Entry { KeyCombo first, second; std::string cmd; };  // second.key unknown: no chord
    std::vector<Entry> entries;
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

        // Parse key combo: e.g. "Ctrl+Shift+S", or a chord "Ctrl+K Ctrl+T"
        auto parse_combo = [](const std::string& part) {
            // The last segment after the last '+' is the key name
            auto last_plus = part.rfind('+');
            std::string key_name = last_plus != std::string::npos ? part.substr(last_plus + 1) : part;
            return KeyCombo{string_to_keycode(key_name), parse_mods(part)};
        };
        auto space = key_part.find(' ');
        KeyCombo first = parse_combo(key_part.substr(0, space));
        KeyCombo second{SDLK_UNKNOWN, KeyMod::None};
        if (space != std::string::npos) {
            second = parse_combo(key_part.substr(key_part.find_first_not_of(' ', space)));
            if (second.key == SDLK_UNKNOWN) continue;
        }
        if (first.key != SDLK_UNKNOWN) entries.push_back({first, second, cmd_part});
    }
    for (auto& e : entries) {
        if (std::find(user_commands_.begin(), user_commands_.end(), e.cmd) != user_commands_.end()) continue;
        user_commands_.push_back(e.cmd);
        erase_command(e.cmd);
    }
    for (auto& e : entries) {
        if (e.second.key == SDLK_UNKNOWN) bindings_[e.first] = e.cmd;
        else chords_[e.first][e.second] = e.cmd;
    }
    return true;
}

void KeyMap::rebind(const std::string& command, SDL_Keycode key, KeyMod mods) {
    erase_command(command);
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
    if (!best.empty()) return best;
    for (auto& [prefix, second] : chords_)
        for (auto& [combo, cmd] : second) {
            if (cmd != command) continue;
            std::string s = chord_to_string(prefix.key, prefix.mods) + " " + chord_to_string(combo.key, combo.mods);
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
    for (auto& [prefix, second] : chords_)
        for (auto& [combo, cmd] : second) {
            if (std::find(user_commands_.begin(), user_commands_.end(), cmd) == user_commands_.end()) continue;
            if (!first) f << ",\n";
            first = false;
            f << "  \"" << chord_to_string(prefix.key, prefix.mods) << " " << chord_to_string(combo.key, combo.mods)
              << "\": \"" << cmd << "\"";
        }
    f << "\n}\n";
    return true;
}

} // namespace oh
