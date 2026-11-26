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

// Spring toward a target that keeps its velocity when retargeted, so repeated page
// scrolls or jumps chain smoothly (Neovide-style). `duration` is the critically
// damped settle time scale (omega = 4 / duration). zeta < 1 overshoots (wobble).
struct Spring {
    float value = 0, target = 0, velocity = 0;
    float zeta = 1.0f;
    bool initialized = false;

    void snap(float v) { value = target = v; velocity = 0; initialized = true; }
    void set(float t) {
        target = t;
        if (!initialized) snap(t);
    }
    bool moving() const { return value != target || velocity != 0; }
    float step(float duration, float epsilon = 0.25f) {
        if (!enabled || duration <= 0) { velocity = 0; return value = target; }
        float x = value - target;
        if (std::abs(x) <= epsilon && std::abs(velocity) <= epsilon * 8.0f) { velocity = 0; return value = target; }
        float w = 4.0f / duration;
        if (zeta >= 1.0f) {  // exact critically damped step: x(t) = (a + b t) e^(-w t)
            float b = velocity + w * x, e = std::exp(-w * dt);
            x = (x + b * dt) * e;
            velocity = (velocity - w * b * dt) * e;
        } else {  // underdamped: semi-implicit Euler in <= 4 ms substeps
            int n = static_cast<int>(dt / 0.004f) + 1;
            float h = dt / static_cast<float>(n);
            for (int i = 0; i < n; i++) {
                velocity += (-w * w * x - 2.0f * zeta * w * velocity) * h;
                x += velocity * h;
            }
        }
        value = target + x;
        active = true;
        return value;
    }
    float follow(float t, float duration, float epsilon = 0.25f) {
        set(t);
        return step(duration, epsilon);
    }
};

// Scroll spring settle scale (settings: scroll_animation_ms) and the opt-in
// elastic popups (settings: wobbly).
inline float scroll_duration = 0.25f;
inline bool wobbly = false;

// Elastic pop for popups (opt-in, settings: wobbly): a damped oscillation that
// starts at 1 and settles to 0 in about half a second.
struct Wobble {
    float age = 1e9f;
    void kick() { age = 0; }
    float step() {
        if (!enabled || !wobbly || age > 0.6f) return 0.0f;
        age += dt;
        active = true;
        return std::exp(-age * 8.0f) * std::cos(age * 28.0f);
    }
};

} // namespace oh::anim
