#pragma once
#include <cmath>

namespace oh::anim {

// Global switch (settings: animations = false) and a per-frame "still moving" flag
// the main loop uses to keep drawing until everything has settled.
inline bool enabled = true;
inline bool active = false;
inline float dt = 0.0f;  // seconds since the previous frame

// Time constants: ~3 tau to settle, so 40 ms ≈ 120 ms and 60 ms ≈ 180 ms.
constexpr float FAST = 0.040f;
constexpr float NORMAL = 0.060f;

// Exponentially eases toward a target; frame-rate independent.
struct Smooth {
    float value = 0, target = 0;
    bool initialized = false;

    void snap(float v) { value = target = v; initialized = true; }
    void set(float t) {
        target = t;
        if (!initialized) snap(t);
    }
    float step(float tau = NORMAL, float epsilon = 0.25f) {
        if (!enabled || tau <= 0) return value = target;
        float d = target - value;
        if (std::abs(d) <= epsilon) return value = target;
        value += d * (1.0f - std::exp(-dt / tau));
        active = true;
        return value;
    }
    // Convenience: retarget and advance in one call.
    float follow(float t, float tau = NORMAL, float epsilon = 0.25f) {
        set(t);
        return step(tau, epsilon);
    }
};

} // namespace oh::anim
