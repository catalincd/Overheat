#include "ui/pets.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace oh {

// ---------------------------------------------------------------------------
// Art. Original pixel art, side view facing right (drawn mirrored when facing left),
// bottom row on the ground. o = coat, d = darker back, c = cream (chest, tail tip),
// k = black socks / ear tips / nose, e = eye (closes on a blink), . = clear.
// ---------------------------------------------------------------------------

struct Pets::Sprite {
    int w, h;
    const char* const* rows;
};

namespace {

template <size_t N>
Pets::Sprite sprite_of(const char* const (&rows)[N]);

// Mother: 24-26 x 8-13.
const char* const M_WALK0[] = {
    ".................k..k...",
    ".................oo.oo..",
    "................oooooo..",
    "................oooeooo.",
    "..ddd...ddddddddoooooook",
    ".dooodddoooooooocccooo..",
    "oooooooooooooooccc......",
    "ccoooo.ooooooooocc......",
    "cccc....oooooooo........",
    "........oo...oo.........",
    ".......k..k.k..k........",
    "......kk..kkk..kk.......",
};
const char* const M_WALK1[] = {
    ".................k..k...",
    ".................oo.oo..",
    "................oooooo..",
    "................oooeooo.",
    "..ddd...ddddddddoooooook",
    ".dooodddoooooooocccooo..",
    "oooooooooooooooccc......",
    "ccoooo.ooooooooocc......",
    "cccc....oooooooo........",
    "........oo....oo........",
    "........kk....kk........",
    "........kkk...kkk.......",
};
const char* const M_RUN0[] = {
    "..................k.k.....",
    ".................ooooo....",
    "....dd..........ooooeoo...",
    "..ddooddddddddoooooooook..",
    ".oooooodoooooooocccoo.....",
    "cooooo.oooooooocccc.......",
    "ccoo....oooooooooc........",
    "cc.......oooooooo.........",
    ".......oo........oo.......",
    ".....kk............kk.....",
    "...kk................kk...",
};
const char* const M_RUN1[] = {
    "..................k.k.....",
    ".................ooooo....",
    ".....dd.........ooooeoo...",
    "..ddooodddddddoooooooook..",
    ".oooooodoooooooocccoo.....",
    "ccooooo.ooooooooccc.......",
    "ccc......oooooooo.........",
    "..........oooooo..........",
    "..........kkoookk.........",
    "...........kkkkk..........",
};
const char* const M_SIT0[] = {
    ".........k..k..",
    ".........oo.oo.",
    "........oooooo.",
    "........oooeooo",
    "........ooooook",
    "........ccccoo.",
    ".......occccc..",
    "......ooocccc..",
    ".....dooooccc..",
    "....ddooooocc..",
    "...dooooooock..",
    "cc.oooooooook..",
    "ccccooooookkk..",
};
const char* const M_SIT1[] = {  // tail tip flicked up
    ".........k..k..",
    ".........oo.oo.",
    "........oooooo.",
    "........oooeooo",
    "........ooooook",
    "........ccccoo.",
    ".......occccc..",
    "......ooocccc..",
    ".....dooooccc..",
    "....ddooooocc..",
    "cc.dooooooock..",
    "ccooooooooook..",
    "...oooooookkk..",
};
const char* const M_SLEEP[] = {  // curled up, tail over the nose
    "...........k.k...",
    ".....ddd...oooo..",
    "...ddooodd.ooooo.",
    "..dooooooddookko.",
    ".ooooooooooooooo.",
    ".ooooooooooocccc.",
    "coooooooooocccccc",
    ".cccccccccccccc..",
};
const char* const M_CROUCH[] = {  // pounce windup / play bow
    ".ddd.....................",
    "dooodddddd.......k..k....",
    "ooooooooooddd....oo.oo...",
    "ccooooooooooood.oooooo...",
    "cc..ooooooooooooooooeooo.",
    ".....ooooooooocccoooooook",
    "......o..k...kcc.........",
    ".....k..kk...kk.kk.......",
};

// Kits: smaller, rounder, bigger ears.
const char* const K_WALK0[] = {
    "........k..k..",
    "........oo.oo.",
    ".......oooooo.",
    ".dd....ooeoooo",
    "doodddoooooook",
    "oooooooocccoo.",
    "cc.ooooocc....",
    "...k.k..k.k...",
};
const char* const K_WALK1[] = {
    "........k..k..",
    "........oo.oo.",
    ".......oooooo.",
    ".dd....ooeoooo",
    "doodddoooooook",
    "oooooooocccoo.",
    "cc.ooooocc....",
    "....kk..kk....",
};
const char* const K_RUN0[] = {
    ".........k.k...",
    "........ooooo..",
    ".dd....oooeoo..",
    "doodddoooooook.",
    "ooooooooccco...",
    "cc..oooocc.....",
    "..kk......kk...",
    ".k..........k..",
};
const char* const K_RUN1[] = {
    ".........k.k...",
    "........ooooo..",
    ".dd....oooeoo..",
    "doodddoooooook.",
    "oooooooocccc...",
    "cc..ooooocc....",
    ".....kkkk......",
};
const char* const K_SIT0[] = {
    ".....k..k..",
    ".....oo.oo.",
    "....oooooo.",
    "....ooeoooo",
    "....ooooook",
    "....cccoo..",
    "...occcc...",
    "..oooccc...",
    "c.oooooock.",
    "ccccooookk.",
};
const char* const K_SIT1[] = {
    ".....k..k..",
    ".....oo.oo.",
    "....oooooo.",
    "....ooeoooo",
    "....ooooook",
    "....cccoo..",
    "...occcc...",
    "c.oooccc...",
    "ccoooooock.",
    "..ooooookk.",
};
const char* const K_SLEEP[] = {
    "........k.k.",
    "...ddd..ooo.",
    ".ddoooddoooo",
    "doooooookkoo",
    "oooooooocccc",
    ".cccccccccc.",
};
const char* const K_CROUCH[] = {
    ".dd...........",
    "doodd....k..k.",
    "ooooodd..oo.oo",
    "coooooodoooooo",
    "c..ooooooooeoo",
    "....oocooooook",
    "....k.kk.kk...",
};
const char* const K_DIVE[] = {  // nose-first on the way down (the mouse pounce)
    "cc...........",
    "ccdd.........",
    ".ooodd.......",
    "..ooooood.k..",
    "...ooooooook.",
    "..k.ooooooo..",
    ".k...occeooo.",
    "......cccoooo",
    ".......kk..ok",
};

const char* const BALL_ART[] = {
    ".bb.",
    "bbbb",
    "bbbb",
    ".bb.",
};
const char* const HEART_ART[] = {
    ".h.h.",
    "hhhhh",
    ".hhh.",
    "..h..",
};
const char* const Z_ART[] = {
    "zzzz",
    "...z",
    "..z.",
    ".z..",
    "zzzz",
};

template <size_t N>
Pets::Sprite sprite_of(const char* const (&rows)[N]) {
    return {static_cast<int>(std::strlen(rows[0])), static_cast<int>(N), rows};
}

enum Frame { WALK0, WALK1, RUN0, RUN1, SIT0, SIT1, SLEEP, CROUCH, DIVE, NFRAMES };

const Pets::Sprite* frames(bool kit) {
    static const Pets::Sprite mother[NFRAMES] = {
        sprite_of(M_WALK0), sprite_of(M_WALK1), sprite_of(M_RUN0), sprite_of(M_RUN1), sprite_of(M_SIT0),
        sprite_of(M_SIT1), sprite_of(M_SLEEP), sprite_of(M_CROUCH), sprite_of(M_RUN1)};
    static const Pets::Sprite kits[NFRAMES] = {
        sprite_of(K_WALK0), sprite_of(K_WALK1), sprite_of(K_RUN0), sprite_of(K_RUN1), sprite_of(K_SIT0),
        sprite_of(K_SIT1), sprite_of(K_SLEEP), sprite_of(K_CROUCH), sprite_of(K_DIVE)};
    return kit ? kits : mother;
}

int widest(bool kit) {
    int w = 0;
    for (int f = 0; f < NFRAMES; f++) w = std::max(w, frames(kit)[f].w);
    return w;
}

struct CoatColors { Color o, d, c, k, eye; };
CoatColors coat_colors(Pets::Coat c) {
    switch (c) {
        case Pets::Coat::Arctic:
            return {Color::hex(0xeef0f4), Color::hex(0xc2cad6), Color::hex(0xffffff), Color::hex(0x3b4250),
                    Color::hex(0x1c2028)};
        case Pets::Coat::Silver:
            return {Color::hex(0x9ca3ad), Color::hex(0x6c737d), Color::hex(0xe4e7eb), Color::hex(0x24272d),
                    Color::hex(0x15171a)};
        default:
            return {Color::hex(0xe8762b), Color::hex(0xb0501c), Color::hex(0xf7e8cf), Color::hex(0x3d261d),
                    Color::hex(0x1c1210)};
    }
}

constexpr float GRAVITY = 900.0f;  // px/s^2

// Draws `s` with its top-left at (x, y), mirrored when flip, merging runs of equal
// pixels into one rect. `color` maps an art char to a colour (alpha 0 = skip).
template <class Out, class F>
void blit(const Out& out, const Pets::Sprite& s, float x, float y, int scale, bool flip, F color,
          const Color* outline) {
    auto row_runs = [&](auto emit) {
        for (int j = 0; j < s.h; j++) {
            const char* row = s.rows[j];
            int i = 0;
            while (i < s.w) {
                char ch = row[flip ? s.w - 1 - i : i];
                Color c = color(ch);
                if (c.a <= 0) { i++; continue; }
                int start = i;
                while (i + 1 < s.w) {
                    Color n = color(row[flip ? s.w - 2 - i : i + 1]);
                    if (n.a != c.a || n.r != c.r || n.g != c.g || n.b != c.b) break;
                    i++;
                }
                i++;
                emit(Rect{x + static_cast<float>(start * scale), y + static_cast<float>(j * scale),
                          static_cast<float>((i - start) * scale), static_cast<float>(scale)}, c);
            }
        }
    };
    if (outline) row_runs([&](Rect r, Color) { out({r.x - 1, r.y - 1, r.w + 2, r.h + 2}, *outline); });
    row_runs([&](Rect r, Color c) { out(r, c); });
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

Pets::Coat Pets::parse_coat(const std::string& s) {
    if (s == "arctic" || s == "white") return Coat::Arctic;
    if (s == "silver") return Coat::Silver;
    return Coat::Red;
}

const char* Pets::coat_name(Coat c) {
    switch (c) {
        case Coat::Arctic: return "arctic";
        case Coat::Silver: return "silver";
        default: return "red";
    }
}

Pets::Pets() { configure(3, Coat::Red); }

void Pets::configure(int count, Coat kit_coat) {
    count = std::clamp(count, 1, 3);
    kit_coat_ = kit_coat;
    if (static_cast<int>(pets_.size()) == count) {
        for (size_t i = 1; i < pets_.size(); i++) pets_[i].coat = i == 1 ? kit_coat : Coat::Red;
        return;
    }
    pets_.assign(static_cast<size_t>(count), Pet{});
    for (size_t i = 0; i < pets_.size(); i++) {
        Pet& p = pets_[i];
        p.kit = i > 0;
        p.coat = i == 1 ? kit_coat : Coat::Red;
        p.doze = p.kit ? 1.5f + 2.0f * static_cast<float>(i) : 0.0f;  // the mother curls up first
        p.think = 0.5f + 0.7f * static_cast<float>(i);
        p.dir = i % 2 ? -1 : 1;
        p.blink = 1.5f + static_cast<float>(i);
        p.wag = 1.0f + 1.3f * static_cast<float>(i);
    }
    ball_.on = count > 1;
    placed_ = false;
    set_yard(yard_);
}

void Pets::set_yard(Rect r) {
    Rect old = yard_;
    yard_ = r;
    if (r.w <= 0 || r.h <= 0) return;
    if (placed_ && old.w > 0 && (old.w != r.w || old.x != r.x)) {
        // The column moved or resized (window, sidebar, split): keep everyone's place
        // proportionally instead of piling up at the fence.
        auto map = [&](float x) { return r.x + (x - old.x) * r.w / old.w; };
        for (auto& p : pets_) p.x = map(p.x), p.target = map(p.target);
        ball_.x = map(ball_.x);
    }
    if (!placed_) {
        placed_ = true;
        const float spots[] = {0.22f, 0.40f, 0.55f};
        for (size_t i = 0; i < pets_.size(); i++) pets_[i].x = r.x + r.w * spots[i];
        ball_.x = r.x + r.w * 0.72f;
    }
    for (auto& p : pets_) {
        p.x = std::clamp(p.x, lo(p), std::max(lo(p), hi(p)));
        p.target = std::clamp(p.target, lo(p), std::max(lo(p), hi(p)));
        p.h = std::clamp(p.h, 0.0f, std::max(0.0f, max_apex(p)));
    }
    ball_.x = std::clamp(ball_.x, r.x + 6.0f, std::max(r.x + 6.0f, r.x + r.w - 6.0f));
}

float Pets::rnd() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

float Pets::half_w(const Pet& p) const { return static_cast<float>(widest(p.kit) * SCALE) * 0.5f; }
float Pets::lo(const Pet& p) const { return yard_.x + half_w(p) + 2.0f; }
float Pets::hi(const Pet& p) const { return yard_.x + yard_.w - half_w(p) - 2.0f; }

float Pets::max_apex(const Pet& p) const {
    const Sprite* f = frames(p.kit);
    int tall = p.kit ? std::max(f[RUN0].h, f[RUN1].h) : f[RUN1].h;  // the airborne frames
    return std::max(0.0f, ground_y() - yard_.y - static_cast<float>(tall * SCALE) - 1.0f);
}

float Pets::speed(const Pet& p, bool run) const {
    if (p.kit) return run ? 82.0f : 26.0f;
    return run ? 95.0f : 30.0f;
}

float Pets::wpm() const {
    // Characters in the last 10 s, five per word.
    float n = 0;
    for (float t : keys_)
        if (clock_ - t <= 10.0f) n += 1;
    return n / 5.0f * 6.0f;
}

float Pets::energy() const {
    if (party_ > 0) return 1.0f;
    return std::clamp((wpm() - 10.0f) / 50.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void Pets::on_key(int chars) {
    chars = std::clamp(chars, 1, 20);
    for (int i = 0; i < chars; i++) keys_.push_back(clock_);
    if (keys_.size() > 600) keys_.erase(keys_.begin(), keys_.begin() + 300);
    on_input();
}

void Pets::on_input() {
    idle_ = 0;
    for (auto& p : pets_)
        if (p.st == State::Sleep) wake(p);
}

void Pets::celebrate() {
    party_ = 6.0f;
    idle_ = 0;
    for (auto& p : pets_) {
        if (p.st == State::Sleep) wake(p);
        if (p.st != State::Air) hop(p);
        p.heart = 0;
    }
}

bool Pets::click(float x, float y) {
    if (!yard_.contains(x, y)) return false;
    // Topmost first: airborne pets draw last.
    for (int pass = 0; pass < 2; pass++)
        for (int i = static_cast<int>(pets_.size()) - 1; i >= 0; i--) {
            Pet& p = pets_[static_cast<size_t>(i)];
            if ((p.st == State::Air) != (pass == 0)) continue;
            // Forgiving: a few px of slack and down to the ground, so a pet mid-hop still counts.
            Rect r = sprite_rect(p);
            Rect hit{r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, std::max(r.h + 6.0f, ground_y() - r.y + 1.0f)};
            if (!hit.contains(x, y)) continue;
            idle_ = 0;
            p.heart = 0;
            if (p.st == State::Sleep) wake(p);
            else if (p.st != State::Air && animate_) hop(p);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

void Pets::sit(Pet& p, float think) {
    p.st = State::Sit;
    p.t = 0;
    p.vx = 0;
    p.chase = -1;
    p.flee = false;
    p.think = think;
}

void Pets::wake(Pet& p) {
    // A stretch (the play-bow pose) before anything else.
    p.st = State::Crouch;
    p.stretch = true;
    p.crouch_for = 0.7f;
    p.t = 0;
    p.vx = 0;
}

void Pets::go(Pet& p, float target, bool run) {
    p.target = std::clamp(target, lo(p), std::max(lo(p), hi(p)));
    if (std::abs(p.target - p.x) < 2.0f) return sit(p, rnd(1.0f, 2.5f));
    p.dir = p.target > p.x ? 1 : -1;
    p.st = run ? State::Run : State::Walk;
    p.vx = speed(p, run) * static_cast<float>(p.dir);
    p.t = 0;
}

void Pets::launch(Pet& p, float apex, float land_x, bool pounce) {
    apex = std::clamp(apex, 0.0f, max_apex(p));
    if (apex < 2.0f) return sit(p, 1.0f);
    float v0 = std::sqrt(2.0f * GRAVITY * apex);
    float flight = 2.0f * v0 / GRAVITY;
    land_x = std::clamp(land_x, lo(p), std::max(lo(p), hi(p)));
    p.vh = v0;
    p.vx = (land_x - p.x) / flight;
    if (std::abs(p.vx) > 1.0f) p.dir = p.vx > 0 ? 1 : -1;
    p.st = State::Air;
    p.pounce = pounce;
    p.t = 0;
}

void Pets::hop(Pet& p) {
    p.chase = -1;
    p.flee = false;
    launch(p, max_apex(p) * 0.85f, p.x + static_cast<float>(p.dir) * 6.0f, false);
}

void Pets::decide(Pet& p, int i) {
    float e = energy();
    bool run = rnd() < 0.15f + 0.8f * e;
    float span = hi(p) - lo(p);
    auto somewhere = [&] {
        // A spot at least a fifth of the yard away, in a random direction.
        float d = rnd(0.2f, 0.6f) * span * (rnd() < 0.5f ? -1.0f : 1.0f);
        float t = p.x + d;
        if (t < lo(p) || t > hi(p)) t = p.x - d;
        return t;
    };
    if (!p.kit) {
        float r = rnd() * 10.0f;
        float sit_w = 4.5f - 3.5f * e, walk_w = 3.0f, run_w = 0.3f + 4.0f * e, follow_w = pets_.size() > 1 ? 1.5f : 0;
        if ((r -= sit_w) < 0) return sit(p, rnd(2.0f, 5.0f));
        if ((r -= follow_w) < 0) {  // keep an eye on a kit
            const Pet& k = pets_[1 + static_cast<size_t>(rnd() * static_cast<float>(pets_.size() - 1))];
            return go(p, k.x - static_cast<float>(k.x > p.x ? 1 : -1) * 50.0f, run);
        }
        if ((r -= walk_w) < 0 || r < run_w) return go(p, somewhere(), run);
        return hop(p);
    }
    // Kits: the ball, a sibling, a run, a sit.
    int sibling = -1;
    for (size_t j = 1; j < pets_.size(); j++)
        if (static_cast<int>(j) != i && pets_[j].st != State::Sleep && pets_[j].st != State::Air &&
            std::abs(pets_[j].x - p.x) < 260.0f)  // only a sibling close enough to catch
            sibling = static_cast<int>(j);
    float ball_w = ball_.on ? 4.0f + 3.0f * e : 0.0f;
    float chase_w = sibling >= 0 ? 1.5f + 3.0f * e : 0.0f;
    float walk_w = 2.0f, sit_w = 2.5f - 2.0f * e, hop_w = 0.3f + e;
    float r = rnd() * (ball_w + chase_w + walk_w + sit_w + hop_w);
    if ((r -= ball_w) < 0) {
        // Line up a pounce: stop a pounce-length short of the ball, on our side of it.
        float reach = 46.0f + rnd(0.0f, 14.0f);
        float side = p.x < ball_.x ? -1.0f : 1.0f;
        float t = ball_.x + side * reach;
        if (t < lo(p) || t > hi(p)) t = ball_.x - side * reach;
        go(p, t, run || std::abs(t - p.x) > 120.0f);
        p.chase = BALL;
        if (p.st == State::Sit) {  // already in place: straight to the windup
            p.chase = BALL;
            p.st = State::Crouch;
            p.stretch = false;
            p.crouch_for = rnd(0.45f, 0.9f);
            p.dir = ball_.x > p.x ? 1 : -1;
            p.t = 0;
        }
        return;
    }
    if ((r -= chase_w) < 0) {
        Pet& s = pets_[static_cast<size_t>(sibling)];
        p.chase = sibling;
        p.st = State::Run;
        p.t = 0;
        s.flee = true;
        s.chase = -1;
        // The chased kit bolts for the far side.
        float away = s.x >= p.x ? hi(s) : lo(s);
        go(s, away, true);
        s.flee = true;
        s.vx *= 0.6f;
        return;
    }
    if ((r -= walk_w) < 0) return go(p, somewhere(), run);
    if ((r -= sit_w) < 0) return sit(p, rnd(1.5f, 4.0f));
    hop(p);
}

void Pets::force(int i, const std::string& what) {
    if (i < 0 || i >= static_cast<int>(pets_.size())) return;
    Pet& p = pets_[static_cast<size_t>(i)];
    idle_ = 0;
    if (what == "sleep") {
        idle_ = sleep_after_ + 100.0f;
        return;
    }
    if (p.st == State::Sleep || p.st == State::Air) return;
    if (what == "hop") return hop(p);
    if (what == "run") return go(p, p.x < (lo(p) + hi(p)) / 2 ? hi(p) : lo(p), true);
    if (what == "pounce" && ball_.on) {
        p.chase = BALL;
        p.st = State::Crouch;
        p.stretch = false;
        p.crouch_for = 0.5f;
        p.dir = ball_.x > p.x ? 1 : -1;
        p.t = 0;
        p.vx = 0;
        return;
    }
    if (what == "chase") {
        for (size_t j = 1; j < pets_.size(); j++)
            if (static_cast<int>(j) != i) {
                p.chase = static_cast<int>(j);
                p.st = State::Run;
                pets_[j].flee = true;
                go(pets_[j], pets_[j].x >= p.x ? hi(pets_[j]) : lo(pets_[j]), true);
                pets_[j].flee = true;
                return;
            }
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void Pets::update(float dt, bool animate) {
    animate_ = animate;
    if (dt < 0) dt = 0;
    idle_ += dt;
    if (!animate) {
        // Static: everyone sits where they are, facing the middle.
        clock_ += dt;
        for (auto& p : pets_) {
            p.h = 0;
            p.heart = -1;
            if (p.st != State::Sleep || idle_ < sleep_after_) sit(p, 1.0f);
            p.wag = 3.0f;
            p.blink = 3.0f;
        }
        ball_.vx = 0;
        return;
    }
    // Long gaps (hidden window, sleep) step in slices so nothing tunnels.
    dt = std::min(dt, 0.5f);
    while (dt > 0) {
        float s = std::min(dt, 1.0f / 60.0f);
        step(s);
        dt -= s;
    }
}

void Pets::step(float dt) {
    clock_ += dt;
    if (party_ > 0) party_ -= dt;
    while (!keys_.empty() && clock_ - keys_.front() > 10.0f) keys_.erase(keys_.begin());
    if (yard_.w <= 0) return;

    for (size_t i = 0; i < pets_.size(); i++) {
        Pet& p = pets_[i];
        p.t += dt;
        if (p.heart >= 0) {
            p.heart += dt;
            if (p.heart > 1.3f) p.heart = -1;
        }
        p.blink -= dt;
        if (p.blink < -0.14f) p.blink = rnd(2.5f, 6.0f);
        p.wag -= dt;
        if (p.wag < -1.1f) p.wag = rnd(2.0f, 6.0f);

        // Long idle: curl up (from the ground; a pet in the air lands first).
        if (idle_ > sleep_after_ + p.doze && p.st != State::Sleep && p.st != State::Air) {
            p.st = State::Sleep;
            p.vx = 0;
            p.chase = -1;
            p.flee = false;
            p.t = 0;
            continue;
        }

        switch (p.st) {
        case State::Sleep:
            break;
        case State::Sit:
            p.think -= dt * (1.0f + 2.0f * energy());  // restless while you type
            if (p.think <= 0) decide(p, static_cast<int>(i));
            break;
        case State::Crouch:
            if (p.t >= p.crouch_for) {
                if (p.stretch) {
                    p.stretch = false;
                    sit(p, rnd(0.8f, 2.0f));
                } else if (p.chase == BALL) {
                    // Aim where the ball will be when we come down.
                    float apex = max_apex(p);
                    float flight = 2.0f * std::sqrt(2.0f * GRAVITY * apex) / GRAVITY;
                    float bx = ball_.x + ball_.vx * flight * 0.7f;
                    bool taken = false;  // a sibling on the landing spot: no pile-up
                    for (size_t j = 0; j < pets_.size(); j++)
                        if (j != i && pets_[j].st != State::Air &&
                            std::abs(pets_[j].x - bx) < (half_w(p) + half_w(pets_[j])) * 0.75f)
                            taken = true;
                    if (taken) sit(p, rnd(0.5f, 1.5f));
                    else launch(p, apex, bx, true);
                } else {
                    sit(p, 1.0f);
                }
            }
            break;
        case State::Walk:
        case State::Run: {
            if (p.chase >= 0) {  // chasing a sibling: steer at it, pounce over it when close
                Pet& s = pets_[static_cast<size_t>(p.chase)];
                float dx = s.x - p.x;
                p.dir = dx > 0 ? 1 : -1;
                p.vx = speed(p, true) * static_cast<float>(p.dir);
                if (s.st == State::Sleep || p.t > 4.0f) {
                    s.flee = false;
                    sit(p, 1.5f);
                    break;
                }
                // Close enough: a pounce that lands just behind the sibling (a tag, not a
                // pile-up); the caught kit stops and turns round with a heart.
                float tag = (half_w(s) + half_w(p)) * 0.95f;
                if (std::abs(dx) < tag + 34.0f && s.st != State::Air) {
                    float land = s.x - static_cast<float>(p.dir) * tag;
                    s.flee = false;
                    s.heart = 0;
                    s.dir = -p.dir;
                    s.st = State::Crouch;
                    s.stretch = true;
                    s.crouch_for = 0.9f;
                    s.t = 0;
                    s.vx = 0;
                    if (std::abs(land - p.x) > 8.0f) launch(p, max_apex(p), land, true);
                    else sit(p, rnd(1.0f, 2.0f));
                    p.chase = -1;
                    break;
                }
            }
            float old = p.x;
            p.x += p.vx * dt;
            p.stride += std::abs(p.x - old);
            bool arrived = p.chase < 0 && ((p.dir > 0 && p.x >= p.target) || (p.dir < 0 && p.x <= p.target));
            if (p.x <= lo(p) || p.x >= hi(p)) {  // the fence: turn around
                p.x = std::clamp(p.x, lo(p), std::max(lo(p), hi(p)));
                if (p.chase < 0) arrived = true;
            }
            if (arrived) {
                p.x = std::clamp(p.x, lo(p), std::max(lo(p), hi(p)));
                if (p.chase == BALL) {
                    p.dir = ball_.x > p.x ? 1 : -1;
                    p.st = State::Crouch;
                    p.stretch = false;
                    p.crouch_for = rnd(0.45f, 0.9f);
                    p.t = 0;
                    p.vx = 0;
                } else if (p.flee) {
                    p.flee = false;
                    p.dir = -p.dir;  // turn to face the chaser
                    p.st = State::Crouch;
                    p.stretch = true;
                    p.crouch_for = 0.6f;
                    p.t = 0;
                    p.vx = 0;
                } else {
                    sit(p, rnd(1.0f, 3.5f) * (1.0f - 0.6f * energy()));
                }
                break;
            }
            // Typing fast turns a stroll into a run.
            if (p.st == State::Walk && !p.flee && energy() > 0.5f) {
                p.st = State::Run;
                p.vx = speed(p, true) * static_cast<float>(p.dir);
            }
            // Someone sitting in the way: stop short of them.
            if (p.chase < 0 && !p.flee)
                for (size_t j = 0; j < pets_.size(); j++) {
                    if (j == i) continue;
                    const Pet& o = pets_[j];
                    if (o.st == State::Air) continue;
                    float dx = o.x - p.x;
                    if (dx * static_cast<float>(p.dir) <= 0) continue;  // behind us
                    if (std::abs(dx) > (half_w(o) + half_w(p)) * 0.85f) continue;
                    if (o.st == State::Walk || o.st == State::Run) continue;  // they move on
                    sit(p, rnd(0.5f, 1.5f));
                    break;
                }
            break;
        }
        case State::Air: {
            p.vh -= GRAVITY * dt;
            p.h += p.vh * dt;
            p.x += p.vx * dt;
            if (p.x < lo(p) || p.x > hi(p)) {
                p.x = std::clamp(p.x, lo(p), std::max(lo(p), hi(p)));
                p.vx = 0;
            }
            p.h = std::min(p.h, max_apex(p));
            if (p.h <= 0) {
                p.h = 0;
                p.vh = 0;
                p.vx = 0;
                bool was_ball = p.chase == BALL;
                p.chase = -1;
                if (was_ball && ball_.on && std::abs(ball_.x - p.x) < half_w(p) + 8.0f) {
                    // Got it: bat it away and settle in a play bow.
                    float away = ball_.x >= p.x ? 1.0f : -1.0f;
                    if (std::abs(ball_.x - p.x) < 2.0f) away = static_cast<float>(p.dir);
                    ball_.vx = away * rnd(70.0f, 150.0f);
                }
                if (p.pounce) {
                    p.st = State::Crouch;
                    p.stretch = true;
                    p.crouch_for = 0.25f;
                    p.t = 0;
                } else {
                    sit(p, rnd(0.6f, 2.0f));
                }
                p.pounce = false;
            }
            break;
        }
        }
    }

    // Keep ground pets from stacking: push apart whoever overlaps (sleepers stay put).
    for (size_t i = 0; i < pets_.size(); i++)
        for (size_t j = i + 1; j < pets_.size(); j++) {
            Pet& a = pets_[i];
            Pet& b = pets_[j];
            if (a.st == State::Air || b.st == State::Air) continue;
            if (a.chase == static_cast<int>(j) || b.chase == static_cast<int>(i)) continue;
            float gap = (half_w(a) + half_w(b)) * 0.75f;
            float dx = b.x - a.x;
            if (std::abs(dx) >= gap) continue;
            float push = std::min(gap - std::abs(dx), 40.0f * dt) * (dx >= 0 ? 1.0f : -1.0f);
            bool a_fixed = a.st == State::Sleep, b_fixed = b.st == State::Sleep;
            if (a_fixed && b_fixed) continue;
            if (!a_fixed) a.x = std::clamp(a.x - (b_fixed ? push : push * 0.5f), lo(a), std::max(lo(a), hi(a)));
            if (!b_fixed) b.x = std::clamp(b.x + (a_fixed ? push : push * 0.5f), lo(b), std::max(lo(b), hi(b)));
        }

    // The ball rolls, slows and bounces off the fences.
    if (ball_.on) {
        ball_.x += ball_.vx * dt;
        ball_.spin += ball_.vx * dt / 4.0f;
        ball_.vx *= std::exp(-1.4f * dt);
        if (std::abs(ball_.vx) < 4.0f) ball_.vx = 0;
        float l = yard_.x + 6.0f, r = yard_.x + yard_.w - 6.0f;
        if (ball_.x < l) ball_.x = l, ball_.vx = std::abs(ball_.vx) * 0.6f;
        if (ball_.x > r) ball_.x = std::max(l, r), ball_.vx = -std::abs(ball_.vx) * 0.6f;
    }
}

bool Pets::all_asleep() const {
    if (pets_.empty()) return true;
    for (auto& p : pets_)
        if (p.st != State::Sleep || p.heart >= 0) return false;
    return ball_.vx == 0;
}

float Pets::next_frame_in() const {
    if (pets_.empty() || yard_.w <= 0 || yard_.h <= 0 || !animate_) return -1.0f;
    if (all_asleep()) return ZZ_PERIOD;
    return 1.0f / FPS;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

const Pets::Sprite& Pets::sprite(const Pet& p) const {
    const Sprite* f = frames(p.kit);
    switch (p.st) {
    case State::Walk: return f[static_cast<int>(p.stride / 7.0f) % 2 ? WALK1 : WALK0];
    case State::Run: return f[static_cast<int>(p.stride / 12.0f) % 2 ? RUN1 : RUN0];
    case State::Crouch: return f[CROUCH];
    case State::Air: {
        if (!p.kit) return f[RUN1];  // legs tucked
        if (p.vh > 0) return f[RUN0];
        // The dive frame on the way down once it fits under the yard's top.
        float room = ground_y() - yard_.y - p.h;
        if (p.pounce && static_cast<float>(f[DIVE].h * SCALE) <= room) return f[DIVE];
        return f[RUN1];
    }
    case State::Sleep: return f[SLEEP];
    default: return f[p.wag < 0 && static_cast<int>(-p.wag / 0.18f) % 2 ? SIT1 : SIT0];
    }
}

Rect Pets::sprite_rect(const Pet& p) const {
    const Sprite& s = sprite(p);
    float w = static_cast<float>(s.w * SCALE), h = static_cast<float>(s.h * SCALE);
    float x = std::round(p.x - w * 0.5f);
    // A small butt wiggle while lining up a pounce.
    if (p.st == State::Crouch && !p.stretch && static_cast<int>(p.t * 12.0f) % 2) x += 1.0f;
    float y = std::round(ground_y() - p.h - h);
    y = std::max(y, yard_.y);
    return {x, y, w, h};
}

void Pets::draw(const RectSink& out, const Palette& pal) const {
    if (yard_.w <= 0 || yard_.h <= 0) return;
    float gy = ground_y();
    // Everything is clamped to the yard, whatever the caller's clip does.
    auto rect = [&](Rect r, Color c) {
        float x0 = std::max(r.x, yard_.x), y0 = std::max(r.y, yard_.y);
        float x1 = std::min(r.x + r.w, yard_.x + yard_.w), y1 = std::min(r.y + r.h, yard_.y + yard_.h);
        if (x1 > x0 && y1 > y0) out({x0, y0, x1 - x0, y1 - y0}, c);
    };
    rect({yard_.x, gy, yard_.w, 1.0f}, pal.ground);

    const Color outline = Color::hex(0x2a1d18, 0.55f);
    const Color* edge = pal.light ? &outline : nullptr;

    if (ball_.on) {
        Sprite b = sprite_of(BALL_ART);
        float bx = std::round(ball_.x - 4.0f), by = gy - 8.0f;
        Color base = pal.accent;
        base.a = 1.0f;
        Color shine = {std::min(1.0f, base.r * 0.5f + 0.5f), std::min(1.0f, base.g * 0.5f + 0.5f),
                       std::min(1.0f, base.b * 0.5f + 0.5f), 1.0f};
        blit(rect, b, bx, by, SCALE, false, [&](char ch) { return ch == 'b' ? base : Color{0, 0, 0, 0}; }, edge);
        // A highlight that travels round as it rolls.
        static const int spots[4][2] = {{1, 1}, {2, 1}, {2, 2}, {1, 2}};
        int k = (static_cast<int>(std::floor(ball_.spin)) % 4 + 4) % 4;
        rect({bx + static_cast<float>(spots[k][0] * SCALE), by + static_cast<float>(spots[k][1] * SCALE),
              static_cast<float>(SCALE), static_cast<float>(SCALE)}, shine);
    }

    // Ground pets first, then anyone in the air on top.
    for (int pass = 0; pass < 2; pass++)
        for (const Pet& p : pets_) {
            if ((p.st == State::Air) != (pass == 1)) continue;
            const Sprite& s = sprite(p);
            Rect r = sprite_rect(p);
            CoatColors cc = coat_colors(p.coat);
            bool blink = p.blink < 0 && animate_;
            auto color = [&](char ch) -> Color {
                switch (ch) {
                    case 'o': return cc.o;
                    case 'd': return cc.d;
                    case 'c': return cc.c;
                    case 'k': return cc.k;
                    case 'e': return blink ? cc.d : cc.eye;
                    default: return {0, 0, 0, 0};
                }
            };
            blit(rect, s, r.x, r.y, SCALE, p.dir < 0, color, edge);
        }

    // Hearts and Zz.
    for (const Pet& p : pets_) {
        Rect r = sprite_rect(p);
        float head_x = p.dir > 0 ? r.x + r.w - 6.0f : r.x - 4.0f;
        if (p.heart >= 0) {
            Sprite hs = sprite_of(HEART_ART);
            float a = std::clamp(1.4f - p.heart, 0.0f, 1.0f);
            float hy = std::max(yard_.y + 1.0f, r.y - 4.0f - p.heart * 6.0f);
            Color red = Color::hex(0xe8455a, a);
            blit(rect, hs, std::round(head_x - 5.0f), std::round(hy), SCALE, false,
                 [&](char ch) { return ch == 'h' ? red : Color{0, 0, 0, 0}; }, nullptr);
        }
        if (p.st == State::Sleep) {
            Sprite z = sprite_of(Z_ART);
            int phase = static_cast<int>(std::floor(clock_ / ZZ_PERIOD)) % 3;
            Color ink = pal.ink;
            float zx = p.dir > 0 ? r.x + r.w - 2.0f : r.x - 6.0f;
            // A z by the head that drifts up and fades while the next one starts.
            auto zcol = [&](float a) { return [&, a](char ch) { Color c = ink; c.a *= a; return ch == 'z' ? c : Color{0, 0, 0, 0}; }; };
            float step = static_cast<float>(phase);
            blit(rect, z, zx, r.y - 4.0f - 3.0f * step, SCALE, false, zcol(0.9f - 0.25f * step), nullptr);
            if (phase == 2) blit(rect, z, zx + 6.0f * static_cast<float>(p.dir), r.y + 2.0f, SCALE, false, zcol(0.9f), nullptr);
        }
    }
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

std::vector<Pets::Info> Pets::info() const {
    std::vector<Info> v;
    for (const Pet& p : pets_) {
        Rect r = sprite_rect(p);
        v.push_back({p.x, p.h, r.x, r.x + r.w, r.y, r.y + r.h, p.st, p.kit});
    }
    return v;
}

std::string Pets::describe() const {
    static const char* names[] = {"sit", "walk", "run", "crouch", "air", "sleep"};
    std::string s;
    char buf[96];
    for (size_t i = 0; i < pets_.size(); i++) {
        const Pet& p = pets_[i];
        std::snprintf(buf, sizeof(buf), "%s%s:%s@%.0f h=%.0f%s", i ? " " : "", p.kit ? "kit" : "mother",
                      names[static_cast<int>(p.st)], p.x, p.h, p.heart >= 0 ? " heart" : "");
        s += buf;
    }
    std::snprintf(buf, sizeof(buf), " ball@%.0f wpm=%.0f idle=%.1f asleep=%d", ball_.x, wpm(), idle_, all_asleep());
    return s + buf;
}

} // namespace oh
