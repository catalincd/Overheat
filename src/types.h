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

struct Theme {
    // High-contrast debug palette: every region and token class is unmistakable,
    // so layout and hit-testing bugs are easy to spot in screenshots.
    Color bg            = Color::hex(0x000000);
    Color fg            = Color::hex(0xffffff);
    Color sidebar_bg    = Color::hex(0x002266);
    Color tab_bg        = Color::hex(0x550000);
    Color tab_active_bg = Color::hex(0xcc0000);
    Color status_bg     = Color::hex(0x006600);
    Color selection     = Color::hex(0x0066ff, 0.65f);
    Color current_line  = Color::hex(0x262626);
    Color cursor_color  = Color::hex(0xffff00);
    Color line_number   = Color::hex(0x00ffff);
    Color comment       = Color::hex(0x999999);
    Color keyword       = Color::hex(0xff00ff);
    Color string_color  = Color::hex(0x00ff00);
    Color number        = Color::hex(0xff8800);
    Color function_name = Color::hex(0x00bbff);
    Color type_color    = Color::hex(0xffff00);
    Color operator_color= Color::hex(0xff4444);
    Color error         = Color::hex(0xff0000);
    Color warning       = Color::hex(0xffaa00);
    Color git_added     = Color::hex(0x00ff00);
    Color git_modified  = Color::hex(0x00bbff);
    Color git_deleted   = Color::hex(0xff0000);
    Color border        = Color::hex(0xffffff);
    Color scrollbar     = Color::hex(0xffffff, 0.5f);
    Color terminal_bg   = Color::hex(0x333300);
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
    FileStatus git_status = FileStatus::Unmodified;
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
