#pragma once
#include "types.h"
#include <SDL3/SDL.h>
#include <unordered_map>
#include <string>
#include <functional>
#include <vector>

namespace oh {

struct KeyBinding {
    SDL_Keycode key;
    KeyMod mods;
    std::string command;
};

class KeyMap {
public:
    void init_defaults();
    void bind(SDL_Keycode key, KeyMod mods, const std::string& command);
    std::string lookup(SDL_Keycode key, KeyMod mods) const;

    // User file overlays the defaults: commands it mentions lose their default keys.
    bool load(const fs::path& path);
    // Writes only commands the user rebound.
    bool save(const fs::path& path);
    // Replace all bindings of `command` with one key chord (keybinding editor).
    void rebind(const std::string& command, SDL_Keycode key, KeyMod mods);
    // Display string of the command's first binding, e.g. "Ctrl+Shift+P" ("" if unbound).
    std::string binding_for(const std::string& command) const;
    static std::string chord_to_string(SDL_Keycode key, KeyMod mods);

private:
    struct KeyCombo {
        SDL_Keycode key;
        KeyMod mods;
        bool operator==(const KeyCombo& o) const { return key == o.key && static_cast<uint32_t>(mods) == static_cast<uint32_t>(o.mods); }
    };
    struct KeyComboHash {
        size_t operator()(const KeyCombo& k) const {
            return std::hash<uint64_t>()((uint64_t)k.key << 32 | (uint32_t)k.mods);
        }
    };
    std::unordered_map<KeyCombo, std::string, KeyComboHash> bindings_;
    std::vector<std::string> user_commands_;  // commands defined by the user file / editor
};

} // namespace oh
