#include "ui/monitor_bar.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace oh {

namespace {

size_t cp_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Cuts to `max` code points, ending in an ellipsis when shortened.
std::string fit(const std::string& s, size_t max) {
    if (cp_count(s) <= max) return s;
    if (max == 0) return {};
    std::string out;
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80 && ++n > max - 1) break;
        out += s[i];
    }
    return out + "…";
}

Color level_color(const Theme& th, float pct) {
    return pct < 60.0f ? th.git_added : pct < 85.0f ? th.warning : th.error;
}

void gauge(Renderer2D& r, Rect g, float pct, Color fill, const Theme& th) {
    r.add_rect(g, th.surface_hover, g.h * 0.5f);
    float fw = std::round(g.w * std::clamp(pct / 100.0f, 0.0f, 1.0f));
    if (fw > 0.5f) r.add_rect({g.x, g.y, std::max(fw, g.h), g.h}, fill, g.h * 0.5f);
}

std::string pct_str(float p) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", p);
    return buf;
}

std::string load_str(const SysStats& s) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f", s.load[0], s.load[1], s.load[2]);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Strip
// ---------------------------------------------------------------------------

void MonitorBar::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    Renderer2D& r = *ctx.r;
    float cw = font.char_width();
    float baseline = bounds.y + (bounds.h - font.line_height()) * 0.5f + font.ascender();
    bool hover = bounds.contains(ctx.mouse_x, ctx.mouse_y);
    r.add_rect(bounds, hover || detail_ ? th.surface : th.status_bg);
    r.add_rect({bounds.x, bounds.y, bounds.w, 1.0f}, th.border);

    const SysStats& s = snap_.stats;
    bool live = snap_.state == SysMonitor::State::Live;
    const float GW = 40.0f, GH = 6.0f;
    float gy = std::round(bounds.y + (bounds.h - GH) * 0.5f);

    struct Seg {
        int prio;
        float w;
        std::function<void(float)> draw;
    };
    std::vector<Seg> segs;
    // label (muted) [gauge] value
    auto metric = [&](int prio, std::string label, float pct, std::string value, Color vc) {
        float w = static_cast<float>(cp_count(label)) * cw + (label.empty() ? 0 : 6.0f) +
                  (pct >= 0 ? GW + 6.0f : 0) + static_cast<float>(cp_count(value)) * cw;
        segs.push_back({prio, w, [&, label, pct, value, vc](float x) {
                            if (!label.empty()) x = r.add_text(x, baseline, label, th.status_fg, font) + 6.0f;
                            if (pct >= 0) {
                                gauge(r, {x, gy, GW, GH}, pct, level_color(th, pct), th);
                                x += GW + 6.0f;
                            }
                            r.add_text(x, baseline, value, vc, font);
                        }});
    };

    // Host with a state dot: green live, amber connecting, red lost.
    std::string host = label_;
    std::string note;
    if (snap_.state == SysMonitor::State::Connecting) note = "connecting…";
    else if (snap_.state == SysMonitor::State::Lost) note = s.valid ? "reconnecting…" : "monitor unavailable";
    Color dot = live ? th.git_added : snap_.state == SysMonitor::State::Lost ? th.error : th.warning;
    {
        float w = 14.0f + static_cast<float>(cp_count(host)) * cw + (note.empty() ? 0 : static_cast<float>(cp_count(note) + 1) * cw);
        segs.push_back({0, w, [&, host, note, dot](float x) {
                            r.add_rect({x, std::round(bounds.y + bounds.h * 0.5f - 4.0f), 8.0f, 8.0f}, dot, 4.0f);
                            x = r.add_text(x + 14.0f, baseline, host, th.fg, font);
                            if (!note.empty()) r.add_text(x + cw, baseline, note, th.status_fg, font);
                        }});
    }
    if (s.valid) {
        Color vc = live ? th.fg : th.status_fg;
        if (s.has_cpu) metric(1, "CPU", s.cpu, pct_str(s.cpu), vc);
        if (s.has_mem) {
            float p = s.mem_total ? 100.0f * static_cast<float>(s.mem_used) / static_cast<float>(s.mem_total) : 0;
            if (s.has_mem_detail) metric(2, "RAM", p, sysfmt::bytes(s.mem_used) + "/" + sysfmt::bytes(s.mem_total), vc);
            else metric(2, "RAM", -1, sysfmt::bytes(s.mem_total), vc);
        }
        int root = s.root_disk >= 0 ? s.root_disk : (s.disks.empty() ? -1 : 0);
        if (root >= 0) {
            auto& d = s.disks[static_cast<size_t>(root)];
            metric(3, fit(d.mount, 14), d.pct(), pct_str(d.pct()), vc);
        }
        if (s.folder_disk >= 0 && s.folder_disk != root) {
            auto& d = s.disks[static_cast<size_t>(s.folder_disk)];
            metric(7, fit(d.mount, 18), d.pct(), pct_str(d.pct()), vc);
        }
        if (s.has_net)
            metric(4, "", -1, "↓" + sysfmt::rate(s.rx_rate) + " ↑" + sysfmt::rate(s.tx_rate), vc);
        if (s.has_load) metric(5, "load", -1, load_str(s), vc);
        if (s.has_uptime) metric(6, "up", -1, sysfmt::duration(s.uptime), vc);
    } else if (!snap_.error.empty() && snap_.state == SysMonitor::State::Lost) {
        metric(9, "", -1, snap_.error, th.status_fg);
    }

    // Drop the least important segments until the rest fit.
    const float gap = 22.0f;
    auto total = [&] {
        float t = 0;
        for (auto& sg : segs) t += sg.w;
        return t + gap * static_cast<float>(segs.empty() ? 0 : segs.size() - 1);
    };
    float avail = bounds.w - 20.0f;
    while (segs.size() > 1 && total() > avail) {
        auto it = std::max_element(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.prio < b.prio; });
        segs.erase(it);
    }
    r.push_clip(bounds);
    float x = bounds.x + 10.0f;
    for (size_t i = 0; i < segs.size(); i++) {
        if (i) {
            r.add_rect({std::round(x + gap * 0.5f), bounds.y + 6.0f, 1.0f, bounds.h - 12.0f}, th.border);
            x += gap;
        }
        segs[i].draw(x);
        x += segs[i].w;
    }
    r.pop_clip();
}

bool MonitorBar::handle_mouse(const MouseEvent& e) {
    if (!e.pressed || e.button != 1 || !bounds_.contains(e.x, e.y)) return false;
    detail_ = !detail_;
    return true;
}

// ---------------------------------------------------------------------------
// Detail card
// ---------------------------------------------------------------------------

// Draws (or, with draw = false, only measures) one column of the card and returns
// the y below it. column 0: CPU, memory, network; 1: filesystems, processes; -1: all.
float MonitorBar::detail_body(RenderContext& ctx, float x, float y, float w, bool draw, int column) {
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    Renderer2D& r = *ctx.r;
    const SysStats& s = snap_.stats;
    float cw = font.char_width();
    float lh = std::round(font.line_height() + 6.0f);
    auto base = [&](float row_y) { return row_y + (lh - font.line_height()) * 0.5f + font.ascender(); };
    auto text = [&](float tx, float row_y, const std::string& t, Color c) {
        return draw ? r.add_text(tx, base(row_y), t, c, font) : tx + static_cast<float>(cp_count(t)) * cw;
    };
    auto rtext = [&](float right, float row_y, const std::string& t, Color c) {
        text(right - static_cast<float>(cp_count(t)) * cw, row_y, t, c);
    };
    auto bar = [&](Rect g, float pct, Color c) {
        if (draw) gauge(r, g, pct, c, th);
    };
    auto heading = [&](const std::string& title, const std::string& right) {
        text(x, y, title, th.fg);
        if (!right.empty()) rtext(x + w, y, right, th.fg_muted);
        y += lh;
    };
    auto muted_line = [&](const std::string& t) {
        text(x, y, fit(t, static_cast<size_t>(std::max(0.0f, w / cw))), th.fg_muted);
        y += lh;
    };
    const float gap = 12.0f;
    bool all = column < 0;

    if (all || column == 0) {
        // CPU: total, then a grid of per-core gauges.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%d cores", s.has_cpu ? (pct_str(s.cpu) + "  ·  ").c_str() : "",
                      s.ncpu > 0 ? s.ncpu : static_cast<int>(s.cores.size()));
        heading("CPU", buf);
        if (!s.has_cpu) {
            muted_line(s.valid ? "Usage needs /proc/stat (Linux); not available here" : "Waiting for the first sample…");
        } else {
            bar({x, y + 2.0f, w, 8.0f}, s.cpu, level_color(th, s.cpu));
            y += 16.0f;
            float cell = 150.0f;
            int cols = std::max(1, static_cast<int>((w + 24.0f) / cell));
            float cell_w = (w - 24.0f * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            for (size_t i = 0; i < s.cores.size(); i++) {
                int c = static_cast<int>(i) % cols;
                float cx = x + static_cast<float>(c) * (cell_w + 24.0f);
                float cy = y + static_cast<float>(i / static_cast<size_t>(cols)) * lh;
                std::string idx = std::to_string(i);
                text(cx, cy, idx, th.fg_muted);
                float gx = cx + 3.0f * cw;
                float gw = std::max(10.0f, cell_w - 3.0f * cw - 5.0f * cw);
                bar({gx, cy + std::round((lh - 6.0f) * 0.5f), gw, 6.0f}, s.cores[i], level_color(th, s.cores[i]));
                rtext(cx + cell_w, cy, pct_str(s.cores[i]), th.fg);
            }
            y += std::ceil(static_cast<float>(s.cores.size()) / static_cast<float>(cols)) * lh;
        }
        y += gap;

        // Memory: stacked used / buffers / cache bar, legend, swap.
        if (s.has_mem_detail) {
            heading("Memory", sysfmt::bytes(s.mem_used) + " / " + sysfmt::bytes(s.mem_total));
            float tot = static_cast<float>(std::max<uint64_t>(s.mem_total, 1));
            struct Part { const char* name; uint64_t v; Color c; };
            Color cache_c = th.type_color;
            cache_c.a *= 0.6f;
            Part parts[] = {{"Used", s.mem_used, th.accent}, {"Buffers", s.mem_buffers, th.warning},
                            {"Cache", s.mem_cached, cache_c}};
            if (draw) {
                Rect g{x, y + 2.0f, w, 10.0f};
                r.add_rect(g, th.surface_hover, 3.0f);
                float px = g.x;
                for (auto& p : parts) {
                    float pw = std::min(g.x + g.w - px, std::round(g.w * static_cast<float>(p.v) / tot));
                    if (pw >= 1.0f) r.add_rect({px, g.y, pw, g.h}, p.c, 2.0f);
                    px += std::max(0.0f, pw);
                }
            }
            y += 18.0f;
            float lx = x;
            for (auto& p : parts) {
                if (draw) r.add_rect({lx, y + std::round(lh * 0.5f - 4.0f), 8.0f, 8.0f}, p.c, 2.0f);
                lx = text(lx + 12.0f, y, std::string(p.name) + " " + sysfmt::bytes(p.v), th.fg_muted) + 14.0f;
            }
            if (lx + 12.0f * cw < x + w) text(lx, y, "Free " + sysfmt::bytes(s.mem_free), th.fg_muted);
            y += lh;
            if (s.has_swap) {
                float sp = 100.0f * static_cast<float>(s.swap_used) / static_cast<float>(s.swap_total);
                text(x, y, "Swap", th.fg_muted);
                std::string v = sysfmt::bytes(s.swap_used) + " / " + sysfmt::bytes(s.swap_total);
                float vw = static_cast<float>(cp_count(v)) * cw;
                bar({x + 6.0f * cw, y + std::round((lh - 6.0f) * 0.5f), std::max(10.0f, w - 6.0f * cw - vw - 10.0f), 6.0f},
                    sp, level_color(th, sp));
                rtext(x + w, y, v, th.fg);
            } else {
                text(x, y, "Swap", th.fg_muted);
                text(x + 6.0f * cw, y, "none", th.fg);
            }
            y += lh;
        } else {
            heading("Memory", s.has_mem ? sysfmt::bytes(s.mem_total) + " total" : "");
            muted_line(s.has_mem ? "Usage needs /proc/meminfo (Linux)" : "Not available");
        }
        y += gap;

        heading("Network", "");
        if (s.has_net) {
            float nx = text(x, y, "↓ " + sysfmt::rate(s.rx_rate), th.git_added);
            nx = text(nx + cw, y, "(" + sysfmt::bytes(s.rx_total) + ")", th.fg_muted);
            nx = text(nx + 3.0f * cw, y, "↑ " + sysfmt::rate(s.tx_rate), th.accent);
            text(nx + cw, y, "(" + sysfmt::bytes(s.tx_total) + ")", th.fg_muted);
            y += lh;
        } else {
            muted_line(s.rx_total ? "Measuring…" : "Not available");
        }
        if (all) y += gap;
    }

    if (all || column == 1) {
        // Filesystems: mount, gauge, use%, used / size. The open folder's mount in accent.
        heading("Filesystems", "");
        if (s.disks.empty()) muted_line("Not available");
        float size_w = 13.0f * cw, pct_w = 5.0f * cw, gw = 56.0f;
        float mount_chars = std::max(4.0f, (w - size_w - pct_w - gw - 16.0f) / cw);
        for (size_t i = 0; i < s.disks.size(); i++) {
            auto& d = s.disks[i];
            bool folder = static_cast<int>(i) == s.folder_disk;
            text(x, y, fit(d.mount, static_cast<size_t>(mount_chars)), folder ? th.accent : th.fg);
            float gx = x + w - size_w - pct_w - gw - 8.0f;
            bar({gx, y + std::round((lh - 6.0f) * 0.5f), gw, 6.0f}, d.pct(), level_color(th, d.pct()));
            rtext(gx + gw + pct_w, y, pct_str(d.pct()), th.fg);
            rtext(x + w, y, sysfmt::bytes(d.used) + "/" + sysfmt::bytes(d.total), th.fg_muted);
            y += lh;
        }
        y += gap;

        heading("Top processes", "CPU%   MEM%");
        if (s.procs.empty()) {
            muted_line(snap_.state == SysMonitor::State::Live && s.valid && !s.kernel.empty()
                           ? "Loading…" : "Not available");
        }
        for (auto& p : s.procs) {
            char num[48];
            std::snprintf(num, sizeof num, "%5.1f  %5.1f", static_cast<double>(p.cpu), static_cast<double>(p.mem));
            float num_w = static_cast<float>(cp_count(num)) * cw;
            text(x, y, std::to_string(p.pid), th.fg_muted);
            float cmd_x = x + 8.0f * cw;
            text(cmd_x, y, fit(p.cmd, static_cast<size_t>(std::max(4.0f, (x + w - num_w - cmd_x - 8.0f) / cw))), th.fg);
            rtext(x + w, y, num, p.cpu >= 50.0f ? th.warning : th.fg);
            y += lh;
        }
    }
    return y;
}

void MonitorBar::render_detail(RenderContext& ctx, Rect window) {
    if (!detail_) {
        card_ = {};
        return;
    }
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    Renderer2D& r = *ctx.r;
    const SysStats& s = snap_.stats;
    float cw = font.char_width();
    float lh = std::round(font.line_height() + 6.0f);
    const float pad = 16.0f;

    float w = std::min(820.0f, window.w - 16.0f);
    float inner = w - 2 * pad;
    bool two = inner >= 620.0f;
    float col_w = two ? (inner - 28.0f) * 0.5f : inner;
    float header_h = 2 * lh + 10.0f;
    float body_h = two ? std::max(detail_body(ctx, 0, 0, col_w, false, 0), detail_body(ctx, 0, 0, col_w, false, 1))
                       : detail_body(ctx, 0, 0, col_w, false, -1);
    float h = pad + header_h + body_h + pad * 0.5f;
    float bottom = bounds_.y - 6.0f;
    float y = std::max(8.0f, bottom - h);
    h = std::min(h, bottom - y);
    card_ = {std::round(bounds_.x + 8.0f), std::round(y), std::round(w), std::round(h)};

    for (int i = 3; i >= 1; i--) {
        Color sh = th.shadow;
        sh.a *= 0.22f;
        float g = 4.0f * static_cast<float>(i);
        r.add_rect({card_.x - g, card_.y - g + 6.0f, card_.w + 2 * g, card_.h + 2 * g}, sh, 10.0f + g);
    }
    r.add_rect(card_, th.surface, 10.0f, 1.0f, th.border);
    r.push_clip(card_.inset(1.0f));

    float x = card_.x + pad;
    float cy = card_.y + pad;
    auto base = [&](float row_y) { return row_y + (lh - font.line_height()) * 0.5f + font.ascender(); };
    // Header: host and live state, then OS / kernel / uptime / load / users.
    std::string title = s.user.empty() || label_.find('@') != std::string::npos ? label_ : s.user + "@" + label_;
    float tx = r.add_text(x, base(cy), title, th.fg, font);
    if (!s.host.empty() && title.find(s.host) == std::string::npos)
        r.add_text(tx + cw, base(cy), "(" + s.host + ")", th.fg_muted, font);
    std::string state = snap_.state == SysMonitor::State::Live ? "live · every 2 s"
                        : snap_.state == SysMonitor::State::Connecting ? "connecting…"
                        : (snap_.error.empty() ? std::string("disconnected") : snap_.error);
    state = fit(state, static_cast<size_t>(std::max(0.0f, inner * 0.5f / cw)));
    r.add_text(x + inner - static_cast<float>(cp_count(state)) * cw, base(cy), state,
               snap_.state == SysMonitor::State::Live ? th.git_added : th.warning, font);
    cy += lh;
    std::string info;
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!info.empty()) info += " · ";
        info += part;
    };
    add(s.os);
    add(s.kernel);
    if (s.has_uptime) add("up " + sysfmt::duration(s.uptime));
    if (s.has_load) add("load " + load_str(s));
    if (s.users >= 0) add(std::to_string(s.users) + (s.users == 1 ? " session" : " sessions"));
    r.add_text(x, base(cy), fit(info, static_cast<size_t>(inner / cw)), th.fg_muted, font);
    cy += lh;
    r.add_rect({x, cy + 4.0f, inner, 1.0f}, th.border);
    cy += 10.0f;

    if (two) {
        detail_body(ctx, x, cy, col_w, true, 0);
        r.add_rect({x + col_w + 14.0f, cy, 1.0f, body_h - 8.0f}, th.border);
        detail_body(ctx, x + col_w + 28.0f, cy, col_w, true, 1);
    } else {
        detail_body(ctx, x, cy, col_w, true, -1);
    }
    r.pop_clip();
}

} // namespace oh
