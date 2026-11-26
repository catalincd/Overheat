#include "ui/cursor_fx.h"
#include "anim.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>

namespace oh {

namespace {

constexpr float REL[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};

void corner_dest(Rect r, int i, float& x, float& y) {
    x = r.x + (REL[i][0] + 0.5f) * r.w;
    y = r.y + (REL[i][1] + 0.5f) * r.h;
}

Rect unite(Rect a, Rect b) {
    float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    float x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

bool same_rect(Rect a, Rect b) {
    return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f && std::abs(a.w - b.w) < 0.01f &&
           std::abs(a.h - b.h) < 0.01f;
}

float cross(float ox, float oy, float ax, float ay, float bx, float by) {
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

// Convex hull of four points (monotone chain); out gets 3 or 4 points, padded to 4.
int hull4(const float in[8], float out[8]) {
    int idx[4] = {0, 1, 2, 3};
    std::sort(idx, idx + 4, [&](int a, int b) {
        return in[2 * a] < in[2 * b] || (in[2 * a] == in[2 * b] && in[2 * a + 1] < in[2 * b + 1]);
    });
    int h[8], k = 0;
    for (int pass = 0; pass < 2; pass++) {
        int start = k;
        for (int j = 0; j < 4; j++) {
            int i = pass == 0 ? idx[j] : idx[3 - j];
            while (k >= start + 2 &&
                   cross(in[2 * h[k - 2]], in[2 * h[k - 2] + 1], in[2 * h[k - 1]], in[2 * h[k - 1] + 1], in[2 * i],
                         in[2 * i + 1]) <= 0)
                k--;
            h[k++] = i;
        }
        k--;  // last point repeats as the first of the other chain
    }
    if (k < 3) return k;
    int n = std::min(k, 4);
    for (int i = 0; i < 4; i++) {
        int src = h[std::min(i, n - 1)];
        out[2 * i] = in[2 * src];
        out[2 * i + 1] = in[2 * src + 1];
    }
    return n;
}

} // namespace

CursorFx& CursorFx::get() {
    static CursorFx fx;
    return fx;
}

bool CursorFx::enabled() const {
    return anim::enabled && mode != Mode::None;
}

CursorFx::Mode CursorFx::parse_mode(const std::string& s) {
    if (s == "none" || s == "off" || s == "false") return Mode::None;
    if (s == "glide") return Mode::Glide;
    return Mode::Smear;
}

CursorFx::Vfx CursorFx::parse_vfx(const std::string& s) {
    if (s == "railgun") return Vfx::Railgun;
    if (s == "torpedo") return Vfx::Torpedo;
    if (s == "pixiedust") return Vfx::Pixiedust;
    if (s == "sonicboom" || s == "sonic_boom") return Vfx::SonicBoom;
    if (s == "ripple") return Vfx::Ripple;
    if (s == "wireframe") return Vfx::Wireframe;
    return Vfx::None;
}

const char* CursorFx::mode_name(Mode m) {
    switch (m) {
        case Mode::None: return "none";
        case Mode::Glide: return "glide";
        default: return "smear";
    }
}

const char* CursorFx::vfx_name(Vfx v) {
    switch (v) {
        case Vfx::Railgun: return "railgun";
        case Vfx::Torpedo: return "torpedo";
        case Vfx::Pixiedust: return "pixiedust";
        case Vfx::SonicBoom: return "sonicboom";
        case Vfx::Ripple: return "ripple";
        case Vfx::Wireframe: return "wireframe";
        default: return "none";
    }
}

float CursorFx::rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xffffff) / static_cast<float>(0x1000000);
}

void CursorFx::begin_frame() {
    frame_++;
    if (!anim::enabled) {
        particle_count_ = 0;
        return;
    }
    float dt = anim::dt;
    for (size_t i = 0; i < particle_count_;) {
        Particle& p = particles_[i];
        p.age += dt;
        if (p.age >= p.life) {
            particles_[i] = particles_[--particle_count_];
            continue;
        }
        if (p.kind == 0) {
            if (p.spin != 0) {  // curl: rotate the velocity
                float a = p.spin * dt, c = std::cos(a), s = std::sin(a);
                float vx = p.vx * c - p.vy * s;
                p.vy = p.vx * s + p.vy * c;
                p.vx = vx;
            }
            p.vy += p.gravity * dt;
            float drag = std::exp(-2.0f * dt);
            p.vx *= drag;
            p.vy *= drag;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
        }
        i++;
    }
    if (particle_count_) anim::active = true;
}

void CursorFx::emit(float x, float y, float vx, float vy, float life, float size, float spin, float gravity,
                    uint8_t kind, Color c, Rect clip) {
    if (particle_count_ >= MAX_PARTICLES) return;
    particles_[particle_count_++] = {x, y, vx, vy, 0.0f, life, size, spin, gravity, kind, c, clip};
}

void CursorFx::retarget(Slot& s, Rect target, float cell_w) {
    float cx0 = s.target.x + s.target.w * 0.5f, cy0 = s.target.y + s.target.h * 0.5f;
    float cx1 = target.x + target.w * 0.5f, cy1 = target.y + target.h * 0.5f;
    bool short_move = std::abs(cy1 - cy0) < 0.5f && std::abs(cx1 - cx0) <= cell_w * 2.0f + 0.5f;
    s.target = target;
    float tr = mode == Mode::Glide ? 0.0f : std::clamp(trail, 0.0f, 1.0f);
    float lead_len = length * (1.0f - tr);
    // How much each corner faces its own direction of travel, normalized to 0..1.
    float align[4];
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 4; i++) {
        float dx, dy;
        corner_dest(target, i, dx, dy);
        dx -= s.c[i].x;
        dy -= s.c[i].y;
        float len = std::sqrt(dx * dx + dy * dy);
        float rl = std::sqrt(REL[i][0] * REL[i][0] + REL[i][1] * REL[i][1]);
        align[i] = len > 1e-3f ? (dx * REL[i][0] + dy * REL[i][1]) / (len * rl) : 0.0f;
        lo = std::min(lo, align[i]);
        hi = std::max(hi, align[i]);
    }
    for (int i = 0; i < 4; i++) {
        float a = hi - lo > 1e-4f ? (align[i] - lo) / (hi - lo) : 1.0f;
        s.c[i].length = short_move ? std::min(length, short_length) : length + (lead_len - length) * a;
    }
}

void CursorFx::step(Slot& s) {
    float dt = anim::dt;
    bool moving = false;
    for (int i = 0; i < 4; i++) {
        Corner& c = s.c[i];
        float tx, ty;
        corner_dest(s.target, i, tx, ty);
        float x = c.x - tx, y = c.y - ty;
        if (c.length <= 1e-3f || (std::abs(x) < 0.1f && std::abs(y) < 0.1f && std::abs(c.vx) < 4.0f &&
                                  std::abs(c.vy) < 4.0f)) {
            c.x = tx, c.y = ty, c.vx = c.vy = 0;
            continue;
        }
        float w = 4.0f / c.length, e = std::exp(-w * dt);
        float bx = c.vx + w * x, by = c.vy + w * y;
        x = (x + bx * dt) * e;
        y = (y + by * dt) * e;
        c.vx = (c.vx - w * bx * dt) * e;
        c.vy = (c.vy - w * by * dt) * e;
        c.x = tx + x;
        c.y = ty + y;
        moving = true;
    }
    s.moving = moving;
    if (moving) anim::active = true;
}

void CursorFx::spawn(const Slot& s, Rect from, Rect to, float cell_w) {
    if (vfx == Vfx::None || vfx == Vfx::Wireframe) return;
    float fx = from.x + from.w * 0.5f, fy = from.y + from.h * 0.5f;
    float tx = to.x + to.w * 0.5f, ty = to.y + to.h * 0.5f;
    float dx = tx - fx, dy = ty - fy, d = std::sqrt(dx * dx + dy * dy);
    float ux = d > 1e-3f ? dx / d : 1.0f, uy = d > 1e-3f ? dy / d : 0.0f;
    float px = -uy, py = ux;
    Color c = s.color;
    float lh = to.h;
    (void)cell_w;
    switch (vfx) {
        case Vfx::Railgun: {
            int n = std::clamp(static_cast<int>(d / 6.0f), 3, 48);
            for (int i = 0; i < n; i++) {
                float t = (static_cast<float>(i) + rand01()) / static_cast<float>(n);
                float phase = t * d * 0.08f;
                float sp = 60.0f + 30.0f * rand01();
                emit(fx + dx * t, fy + dy * t, (px * std::sin(phase) + ux * std::cos(phase) * 0.3f) * sp,
                     (py * std::sin(phase) + uy * std::cos(phase) * 0.3f) * sp, 0.4f + 0.2f * rand01(), 3.0f, 4.0f,
                     0.0f, 0, c, s.clip);
            }
            break;
        }
        case Vfx::Torpedo: {
            int n = std::clamp(static_cast<int>(d / 7.0f), 3, 40);
            for (int i = 0; i < n; i++) {
                float t = (static_cast<float>(i) + rand01()) / static_cast<float>(n);
                float sp = 40.0f + 60.0f * rand01(), side = (rand01() - 0.5f) * 70.0f;
                emit(fx + dx * t, fy + dy * t, -ux * sp + px * side, -uy * sp + py * side, 0.45f + 0.2f * rand01(),
                     3.0f, 0.0f, 0.0f, 0, c, s.clip);
            }
            break;
        }
        case Vfx::Pixiedust: {
            int n = std::clamp(static_cast<int>(d / 10.0f), 2, 28);
            for (int i = 0; i < n; i++) {
                float t = (static_cast<float>(i) + rand01()) / static_cast<float>(n);
                emit(fx + dx * t + (rand01() - 0.5f) * 6.0f, fy + dy * t + (rand01() - 0.5f) * lh,
                     (rand01() - 0.5f) * 30.0f, -15.0f - 25.0f * rand01(), 0.7f + 0.4f * rand01(),
                     1.5f + 1.5f * rand01(), 0.0f, 90.0f, 0, c, s.clip);
            }
            break;
        }
        case Vfx::SonicBoom:
            emit(tx, ty, std::max(to.w, to.h) * 0.5f, lh * 1.6f, 0.3f, 0.0f, 0.0f, 0.0f, 2, c, s.clip);
            break;
        case Vfx::Ripple:
            emit(tx, ty, 2.0f, lh * 1.4f, 0.4f, 0.0f, 0.0f, 0.0f, 1, c, s.clip);
            break;
        default: break;
    }
}

bool CursorFx::report(int slot, const void* source, Rect target, Rect clip, float anchor_x, float anchor_y,
                      Color color, float cell_w) {
    if (slot < 0 || slot >= MAX_SLOTS) return true;
    Slot& s = slots_[static_cast<size_t>(slot)];
    bool animate = enabled();
    // Extra cursors that appear (or reappear) start where they are; the primary
    // keeps its last position so it can fly in from another panel.
    bool fresh = !s.alive || (slot > 0 && s.reported + 1 < frame_);
    if (s.stepped == frame_) return !s.moving;  // already handled this frame
    s.color = color;
    if (!animate || fresh) {
        for (int i = 0; i < 4; i++) {
            corner_dest(target, i, s.c[i].x, s.c[i].y);
            s.c[i].vx = s.c[i].vy = 0;
        }
        s.target = target;
        s.clip = clip;
        s.source = source;
        s.anchor_x = anchor_x;
        s.anchor_y = anchor_y;
        s.alive = true;
        s.moving = false;
        s.reported = s.stepped = frame_;
        return true;
    }
    bool same_source = source == s.source;
    float dax = anchor_x - s.anchor_x, day = anchor_y - s.anchor_y;
    if (!same_source) s.clip = unite(s.clip, clip);
    s.source = source;
    s.anchor_x = anchor_x;
    s.anchor_y = anchor_y;

    if (!same_rect(target, s.target)) {
        Rect scrolled = s.target;
        scrolled.x += dax;
        scrolled.y += day;
        if (same_source && same_rect(target, scrolled)) {
            // Only the view scrolled: glide rigidly on the scroll spring's timing
            // so the caret keeps pace with the text.
            s.target = target;
            for (auto& c : s.c) {
                if (anim::scroll_duration <= 0) c.x += dax, c.y += day;
                c.length = anim::scroll_duration;
            }
        } else {
            if (!s.moving) s.clip = clip;
            s.clip = unite(s.clip, clip);
            float x0 = std::min({s.c[0].x, s.c[1].x, s.c[2].x, s.c[3].x});
            float y0 = std::min({s.c[0].y, s.c[1].y, s.c[2].y, s.c[3].y});
            float x1 = std::max({s.c[0].x, s.c[1].x, s.c[2].x, s.c[3].x});
            float y1 = std::max({s.c[0].y, s.c[1].y, s.c[2].y, s.c[3].y});
            Rect from{x0, y0, x1 - x0, y1 - y0};
            retarget(s, target, cell_w);
            if (slot == 0) spawn(s, from, target, cell_w);
        }
    }
    step(s);
    s.reported = s.stepped = frame_;
    if (!s.moving) s.clip = clip;
    return !s.moving;
}

void CursorFx::render(Renderer2D& r) {
    for (auto& s : slots_) {
        if (!s.alive || !s.moving || s.reported != frame_) continue;
        float pts[8], hull[8];
        for (int i = 0; i < 4; i++) pts[2 * i] = s.c[i].x, pts[2 * i + 1] = s.c[i].y;
        if (hull4(pts, hull) < 3) continue;
        r.push_clip(s.clip);
        if (vfx == Vfx::Wireframe) {
            Color fill = s.color;
            fill.a *= 0.25f;
            r.add_poly(hull, fill);
            r.add_poly(hull, s.color, 1.5f);
        } else {
            r.add_poly(hull, s.color);
        }
        r.pop_clip();
    }
    for (size_t i = 0; i < particle_count_; i++) {
        const Particle& p = particles_[i];
        float t = p.age / p.life, fade = 1.0f - t;
        Color c = p.color;
        r.push_clip(p.clip);
        if (p.kind == 0) {
            float sz = p.size * (0.4f + 0.6f * fade);
            c.a *= fade * 0.9f;
            r.add_rect({p.x - sz * 0.5f, p.y - sz * 0.5f, sz, sz}, c, sz * 0.5f);
        } else {
            float e = 1.0f - (1.0f - t) * (1.0f - t);  // ease-out
            float rad = p.vx + (p.vy - p.vx) * e;
            float thick = p.kind == 2 ? std::max(1.0f, rad * 0.45f * fade) : 1.5f;
            c.a *= fade * (p.kind == 2 ? 0.5f : 0.8f);
            r.add_rect({p.x - rad, p.y - rad, rad * 2.0f, rad * 2.0f}, {0, 0, 0, 0}, rad, thick, c);
        }
        r.pop_clip();
    }
}

} // namespace oh
