#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <glm/glm.hpp>

namespace oh {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct Color {
    float r, g, b, a;
    static Color hex(uint32_t c, float alpha = 1.0f) {
        return {((c >> 16) & 0xFF) / 255.0f,
                ((c >> 8) & 0xFF) / 255.0f,
                (c & 0xFF) / 255.0f, alpha};
    }
    glm::vec4 vec4() const { return {r, g, b, a}; }
};

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    Rect inset(float d) const { return {x + d, y + d, w - 2*d, h - 2*d}; }
};

// Colour tokens. Values come from a theme (see theme.h); these defaults are the
// built-in "Overheat Dark" so a Theme is always usable.
struct Theme {
    std::string name    = "Overheat Dark";
    bool dark           = true;

    // Surfaces (flat: separation comes from tone, not borders)
    Color bg            = Color::hex(0x16171a);   // editor
    Color sidebar_bg    = Color::hex(0x111214);
    Color gutter_bg     = Color::hex(0x16171a);
    Color tab_bg        = Color::hex(0x111214);   // tab strip
    Color tab_active_bg = Color::hex(0x16171a);
    Color status_bg     = Color::hex(0x111214);
    Color terminal_bg   = Color::hex(0x111214);
    Color surface       = Color::hex(0x1e1f23);   // palette, popups
    Color surface_hover = Color::hex(0x26272c);
    Color input_bg      = Color::hex(0x111214);
    Color border        = Color::hex(0x24252a);   // used sparingly
    Color shadow        = Color::hex(0x000000, 0.45f);

    // Text
    Color fg            = Color::hex(0xd6d7db);
    Color fg_muted      = Color::hex(0x8a8d96);
    Color line_number   = Color::hex(0x4d5058);
    Color line_number_active = Color::hex(0xb5b8c0);
    Color tab_inactive_fg = Color::hex(0x8a8d96);
    Color status_fg     = Color::hex(0x8a8d96);
    Color terminal_fg   = Color::hex(0xc8cad0);

    // Accent and selection
    Color accent        = Color::hex(0x5b9cff);
    Color selection     = Color::hex(0x5b9cff, 0.28f);
    Color current_line  = Color::hex(0x1b1c20);
    Color cursor_color  = Color::hex(0x5b9cff);
    Color tree_selection = Color::hex(0x5b9cff, 0.18f);
    Color scrollbar     = Color::hex(0xffffff, 0.10f);

    // Syntax
    Color comment       = Color::hex(0x6b6f78);
    Color keyword       = Color::hex(0xb392f0);
    Color string_color  = Color::hex(0x9ccf8a);
    Color number        = Color::hex(0xe6b673);
    Color function_name = Color::hex(0x7fb4ff);
    Color type_color    = Color::hex(0x6fd0c5);
    Color operator_color= Color::hex(0xa9adb7);
    Color punctuation   = Color::hex(0x8a8d96);
    Color property      = Color::hex(0xc7cbd6);
    Color preprocessor  = Color::hex(0xb392f0);

    // Diagnostics and git
    Color error         = Color::hex(0xf26d6d);
    Color warning       = Color::hex(0xe5b350);
    Color info          = Color::hex(0x5b9cff);
    Color git_added     = Color::hex(0x5cc97b);
    Color git_modified  = Color::hex(0x6aa8ff);
    Color git_deleted   = Color::hex(0xf26d6d);
    Color git_untracked = Color::hex(0x5cc97b);
    Color git_ignored   = Color::hex(0x4d5058);

    // Terminal ANSI palette (0-7 normal, 8-15 bright)
    Color ansi[16] = {
        Color::hex(0x1e1f23), Color::hex(0xf26d6d), Color::hex(0x5cc97b), Color::hex(0xe5b350),
        Color::hex(0x5b9cff), Color::hex(0xb392f0), Color::hex(0x5fc8d0), Color::hex(0xc8cad0),
        Color::hex(0x5c5f68), Color::hex(0xff8a8a), Color::hex(0x7ddc95), Color::hex(0xf2cc72),
        Color::hex(0x82b4ff), Color::hex(0xc9aaff), Color::hex(0x80dbe2), Color::hex(0xf2f3f5),
    };
};

struct GlyphInfo {
    float atlas_x, atlas_y;  // top-left texel in the atlas
    float width, height;
    float bearing_x, bearing_y;
    float advance;
};

struct Position {
    size_t line;
    size_t col;
    bool operator==(const Position& o) const { return line == o.line && col == o.col; }
    bool operator<(const Position& o) const { return line < o.line || (line == o.line && col < o.col); }
    bool operator<=(const Position& o) const { return *this == o || *this < o; }
};

struct Selection {
    Position anchor;
    Position head;
    Position min() const { return anchor < head ? anchor : head; }
    Position max() const { return anchor < head ? head : anchor; }
    bool empty() const { return anchor == head; }
};

enum class TokenType : uint8_t {
    Normal, Keyword, String, Number, Comment, Function,
    Type, Operator, Punctuation, Preprocessor, Variable,
    Property, Tag, Attribute, Builtin, Error
};

struct Token {
    uint32_t start;
    uint32_t length;
    TokenType type;
};

enum class FileStatus : uint8_t {
    Unmodified, Modified, Added, Deleted, Renamed, Untracked, Ignored
};

struct FileEntry {
    std::string name;
    fs::path path;
    bool is_dir;
    bool expanded = false;
    int depth = 0;
    bool scanned = false;  // children listed (lazily, on first expand)
    std::vector<FileEntry> children;
};

enum class KeyMod : uint32_t {
    None  = 0,
    Ctrl  = 1 << 0,
    Shift = 1 << 1,
    Alt   = 1 << 2,
    Super = 1 << 3,
};
inline KeyMod operator|(KeyMod a, KeyMod b) {
    return static_cast<KeyMod>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(KeyMod a, KeyMod b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

struct KeyEvent {
    uint32_t keycode;
    KeyMod mods;
    bool pressed;
};

struct MouseEvent {
    float x, y;
    int button;
    bool pressed;
    bool double_click;
    float scroll_x, scroll_y;
    bool motion = false;
};

using CommandFn = std::function<void()>;

// Thread-safe: wakes the event-driven main loop so the next frame is drawn.
void request_redraw();

struct Command {
    std::string name;
    std::string label;
    CommandFn execute;
    std::string keybinding;
};

} // namespace oh
