#pragma once
#include "types.h"
#include <SDL3/SDL.h>
#include <unordered_map>
#include <string>
#include <functional>

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

    bool load(const fs::path& path);
    bool save(const fs::path& path);

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
};

} // namespace oh
