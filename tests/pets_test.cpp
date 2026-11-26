// Pet yard simulation: pets and everything they draw stay inside the yard, typing
// livens them up, idleness puts them to sleep and a sleeping yard stops asking for frames.
#include "ui/pets.h"
#include <cstdio>
#include <cstdlib>

using namespace oh;

static int failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
            failures++;                                               \
        }                                                             \
    } while (0)

static bool inside(Rect r, Rect yard) {
    return r.x >= yard.x - 0.01f && r.y >= yard.y - 0.01f && r.x + r.w <= yard.x + yard.w + 0.01f &&
           r.y + r.h <= yard.y + yard.h + 0.01f;
}

static void check_bounds(const Pets& pets, const char* when, float t) {
    Rect y = pets.yard();
    float ground = y.y + y.h - Pets::GROUND_MARGIN;
    for (auto& i : pets.info()) {
        CHECK(i.left >= y.x && i.right <= y.x + y.w, "%s t=%.2f: pet x [%.1f, %.1f] outside [%.1f, %.1f]", when, t,
              i.left, i.right, y.x, y.x + y.w);
        CHECK(i.top >= y.y && i.bottom <= ground + 0.01f, "%s t=%.2f: pet y [%.1f, %.1f] outside [%.1f, %.1f]", when,
              t, i.top, i.bottom, y.y, ground);
    }
    int rects = 0;
    pets.draw([&](Rect r, Color) {
        rects++;
        CHECK(inside(r, y), "%s t=%.2f: drew (%.1f,%.1f %.1fx%.1f) outside the yard", when, t, r.x, r.y, r.w, r.h);
    }, {Color::hex(0x444444), Color::hex(0x888888), Color::hex(0x5b9cff), true});
    CHECK(rects > 0, "%s: nothing drawn", when);
}

int main(int argc, char** argv) {
    uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 1;
    for (int count = 1; count <= 3; count++) {
        Pets pets;
        pets.set_seed(seed * 7919u + static_cast<uint32_t>(count));
        pets.configure(count, Pets::Coat::Arctic);
        pets.set_yard({300, 640, 700, Pets::YARD_H});
        pets.set_sleep_after(20.0f);
        const float dt = 1.0f / 30.0f;
        float t = 0;
        bool saw_air = false, saw_run = false;
        // 40 s of lively typing with clicks and forced tricks.
        for (int f = 0; f < 1200; f++, t += dt) {
            if (f % 3 == 0) pets.on_key(2);
            if (f % 97 == 0) {
                auto info = pets.info();
                auto& i = info[static_cast<size_t>(f / 97) % info.size()];
                pets.click((i.left + i.right) / 2, (i.top + i.bottom) / 2);
            }
            if (f % 151 == 0) pets.force(f / 151 % count, f % 2 ? "pounce" : "chase");
            if (f == 600) pets.set_yard({300, 640, 420, Pets::YARD_H});  // the window shrinks
            pets.update(dt, true);
            check_bounds(pets, "active", t);
            for (auto& i : pets.info()) {
                saw_air |= i.state == Pets::State::Air;
                saw_run |= i.state == Pets::State::Run;
            }
            CHECK(pets.next_frame_in() > 0 && pets.next_frame_in() <= 1.0f / 29.0f, "active yard should run at ~30 fps");
        }
        CHECK(pets.wpm() > 30, "typing should register (wpm %.0f)", pets.wpm());
        CHECK(saw_air, "count %d: nobody jumped", count);
        CHECK(saw_run, "count %d: nobody ran while typing", count);

        // Then nothing: everyone falls asleep and the yard stops asking for frames.
        for (int f = 0; f < 1200; f++, t += dt) {
            pets.update(dt, true);
            check_bounds(pets, "idle", t);
        }
        CHECK(pets.all_asleep(), "count %d: not asleep after idling: %s", count, pets.describe().c_str());
        CHECK(pets.next_frame_in() >= Pets::ZZ_PERIOD, "sleeping yard wants frames every %.3f s",
              pets.next_frame_in());
        // Big gaps (only Zz ticks or a hidden window) keep them asleep and in bounds.
        for (int f = 0; f < 20; f++) {
            pets.update(0.5f, true);
            check_bounds(pets, "zz", t += 0.5f);
        }
        CHECK(pets.all_asleep(), "woke without input");

        // Any input wakes them; animations off makes the yard static.
        pets.on_input();
        CHECK(!pets.all_asleep(), "input should wake the pets");
        pets.update(dt, true);
        CHECK(pets.next_frame_in() > 0 && pets.next_frame_in() < 0.1f, "awake yard should animate");
        pets.update(dt, false);
        CHECK(pets.next_frame_in() < 0, "animations = false: no frames");
        check_bounds(pets, "static", t);
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("pets_test: ok (seed %u)\n", seed);
    return failures ? 1 : 0;
}
