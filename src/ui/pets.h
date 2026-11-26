#pragma once
#include "types.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oh {

// Pet foxes in a strip ("the yard") under the editor text: a mother fox and up to two
// kits that sit, walk, run, chase each other, mouse-pounce on a rolling ball and curl up
// asleep when the editor has been idle. Pure simulation plus a pixel drawer (no renderer
// dependency, so it is unit-tested); App owns the yard rect (compute_layout), feeds
// keystrokes and clicks, and asks how soon the yard wants its next frame so a still or
// sleeping yard costs no redraws.
//
// Settings: pets (false), pet_count (1-3), pet_kit_color ("red" | "arctic" | "silver"),
// pets_sleep_after (seconds, hidden; default 60).
class Pets {
public:
    enum class Coat { Red, Arctic, Silver };
    static Coat parse_coat(const std::string& s);
    static const char* coat_name(Coat c);

    static constexpr float YARD_H = 36.0f;        // strip height (logical px)
    static constexpr float GROUND_MARGIN = 5.0f;  // ground line above the yard's bottom edge,
                                                  // clear of the terminal sash's grab band
    static constexpr int SCALE = 2;               // screen px per art pixel
    static constexpr float FPS = 30.0f;           // frame cap while anything moves
    static constexpr float ZZ_PERIOD = 0.5f;      // Zz tick while everyone sleeps (2 fps)

    Pets();

    // Rebuilds the cast: 1 = the mother, 2 = plus a kit, 3 = plus two kits. The first kit
    // wears `kit_coat`, the second stays red.
    void configure(int count, Coat kit_coat);
    int count() const { return static_cast<int>(pets_.size()); }
    Coat kit_coat() const { return kit_coat_; }
    void set_sleep_after(float seconds) { sleep_after_ = seconds; }
    float sleep_after() const { return sleep_after_; }
    void set_seed(uint32_t seed) { rng_ = seed ? seed : 1; }

    // The yard rect; pets are kept inside it (positions clamp when it shrinks).
    void set_yard(Rect r);
    Rect yard() const { return yard_; }

    // Advances the simulation. `animate` false (animations = false) poses everyone
    // sitting, still. Long gaps (a hidden window) are stepped in small slices.
    void update(float dt, bool animate);

    // Input hooks: on_key for editor keystrokes (typing speed), on_input for any user
    // input (resets the idle clock and wakes sleepers), celebrate after a good build.
    void on_key(int chars = 1);
    void on_input();
    void celebrate();
    // A click at (x, y): a pet there jumps (or wakes) and shows a heart. True when one was hit.
    bool click(float x, float y);

    // Seconds after the last update() until the yard wants another frame, or a negative
    // value when it is still (animations off). While everyone sleeps this is the slow Zz tick.
    float next_frame_in() const;
    bool all_asleep() const;
    float wpm() const;
    float idle_seconds() const { return idle_; }

    struct Palette {
        Color ground;   // faint ground line
        Color ink;      // Zz
        Color accent;   // the ball
        bool light;     // light theme: outline the pets so pale coats read
    };
    // Emits screen rects through `rect`; every one lies inside the yard.
    using RectSink = std::function<void(Rect, Color)>;
    void draw(const RectSink& rect, const Palette& pal) const;

    // For tests and probes.
    enum class State { Sit, Walk, Run, Crouch, Air, Sleep };
    struct Info { float x, h, left, right, top, bottom; State state; bool kit; };
    std::vector<Info> info() const;
    std::string describe() const;
    // Scripted QA: force a behaviour on pet `i` ("pounce", "hop", "run", "chase", "sleep").
    void force(int i, const std::string& what);

    struct Sprite;  // art frame (pets.cpp)

private:
    struct Pet {
        bool kit = false;
        Coat coat = Coat::Red;
        float x = 0, h = 0, vx = 0, vh = 0;
        int dir = 1;
        State st = State::Sit;
        float t = 0, think = 1.0f;
        float stride = 0;          // distance covered, drives the leg frames
        float blink = 3.0f, wag = 2.0f;
        float heart = -1.0f;       // heart age in seconds, < 0 = none
        float target = 0;
        int chase = -1;            // pet index being chased, BALL = going for the ball
        bool flee = false;         // running from a chasing sibling
        bool pounce = false;       // airborne kind: a pounce (dives on the way down) or a hop
        bool stretch = false;      // crouch that is a waking stretch / play bow, not a pounce windup
        float crouch_for = 0.6f;
        float doze = 0;            // extra idle seconds before this one curls up
    };
    static constexpr int BALL = -2;
    struct Ball { float x = 0, vx = 0, spin = 0; bool on = false; };

    void step(float dt);
    void decide(Pet& p, int i);
    void go(Pet& p, float target, bool run);
    void launch(Pet& p, float apex, float land_x, bool pounce);
    void hop(Pet& p);
    void sit(Pet& p, float think);
    void wake(Pet& p);
    float speed(const Pet& p, bool run) const;
    float half_w(const Pet& p) const;
    float lo(const Pet& p) const;
    float hi(const Pet& p) const;
    float ground_y() const { return yard_.y + yard_.h - GROUND_MARGIN; }
    float max_apex(const Pet& p) const;
    float energy() const;
    const Sprite& sprite(const Pet& p) const;
    Rect sprite_rect(const Pet& p) const;
    float rnd();  // [0, 1)
    float rnd(float a, float b) { return a + (b - a) * rnd(); }

    std::vector<Pet> pets_;
    Ball ball_;
    Rect yard_{0, 0, 0, 0};
    bool placed_ = false;
    Coat kit_coat_ = Coat::Red;
    bool animate_ = true;
    float clock_ = 0;           // simulation time
    float idle_ = 0;            // seconds without user input
    float sleep_after_ = 60.0f;
    float party_ = 0;           // celebrate(): seconds of extra energy left
    std::vector<float> keys_;   // keystroke times (clock_) inside the WPM window
    uint32_t rng_ = 0x9e3779b9u;
};

} // namespace oh
