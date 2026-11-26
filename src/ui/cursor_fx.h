#pragma once
#include "types.h"
#include <array>
#include <cstdint>
#include <string>

namespace oh {

class Renderer2D;

// Neovide-style animated cursors, shared by every panel that draws a caret so the
// cursor can fly between split editors and into the terminal.
//
// Each cursor is a quad whose four corners chase their destinations on critically
// damped springs. Corners facing the direction of travel settle faster than the
// trailing ones, so a jump leaves a stretched smear that snaps in. Panels call
// report() where they used to draw their caret; when it returns true (settled, or
// animation off) they draw it themselves exactly as before, otherwise the smear is
// drawn by render() at the end of the frame. Optional particle effects (railgun,
// torpedo, pixiedust, sonic boom, ripple, wireframe) come from a fixed pool.
class CursorFx {
public:
    enum class Mode { None, Glide, Smear };
    enum class Vfx { None, Railgun, Torpedo, Pixiedust, SonicBoom, Ripple, Wireframe };

    static CursorFx& get();
    bool enabled() const;  // animations on and mode != None

    // Settings (settings.toml: cursor_animation, cursor_animation_ms, cursor_trail, cursor_vfx).
    Mode mode = Mode::Smear;
    Vfx vfx = Vfx::None;
    float length = 0.15f;        // long-jump settle scale, seconds
    float short_length = 0.04f;  // moves of <= 2 cells on one row (typing)
    float trail = 0.8f;          // 0: rigid glide .. 1: leading corners jump instantly

    static constexpr int MAX_SLOTS = 16;  // more cursors than this are drawn plainly

    // Once per frame, before panels render.
    void begin_frame();
    // A caret of `source` (any stable pointer) wants to be at `target` (screen
    // pixels, at the source's settled scroll position). `anchor` is where the
    // source's document origin will be on screen: when only it moves (scrolling),
    // the caret glides with the text instead of smearing. Slot 0 is the
    // primary cursor of whichever panel has focus; 1.. are extra cursors.
    // Returns true if the caller should draw its own caret this frame.
    bool report(int slot, const void* source, Rect target, Rect clip, float anchor_x, float anchor_y,
                Color color, float cell_w);
    // Forget the scroll anchor (the same document moved to another split group).
    void break_anchor() { slots_[0].source = nullptr; }
    // Draw in-flight cursors and particles; call after the panels.
    void render(Renderer2D& r);

    static Mode parse_mode(const std::string& s);
    static Vfx parse_vfx(const std::string& s);
    static const char* mode_name(Mode m);
    static const char* vfx_name(Vfx v);

    size_t particle_count() const { return particle_count_; }

private:
    struct Corner {
        float x = 0, y = 0, vx = 0, vy = 0, length = 0;
    };
    struct Slot {
        Corner c[4];  // TL, TR, BR, BL
        Rect target{};
        Rect clip{};  // grows to cover both panels during a cross-panel flight
        const void* source = nullptr;
        float anchor_x = 0, anchor_y = 0;
        Color color{};
        uint64_t reported = 0;  // frame of the last report
        uint64_t stepped = 0;
        bool alive = false, moving = false;
    };
    struct Particle {
        float x, y, vx, vy, age, life, size, spin, gravity;  // rings: vx/vy = start/end radius
        uint8_t kind;  // 0 dot, 1 ring outline, 2 thick ring
        Color color;
        Rect clip;
    };
    static constexpr size_t MAX_PARTICLES = 384;

    void retarget(Slot& s, Rect target, float cell_w);
    void step(Slot& s);
    void spawn(const Slot& s, Rect from, Rect to, float cell_w);
    void emit(float x, float y, float vx, float vy, float life, float size, float spin, float gravity, uint8_t kind,
              Color c, Rect clip);
    float rand01();

    std::array<Slot, MAX_SLOTS> slots_{};
    std::array<Particle, MAX_PARTICLES> particles_{};
    size_t particle_count_ = 0;
    uint64_t frame_ = 1;
    uint32_t rng_ = 0x9e3779b9u;
};

} // namespace oh
