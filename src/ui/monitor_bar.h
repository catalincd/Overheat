#pragma once
#include "ui/panel.h"
#include "monitor.h"

namespace oh {

// MobaXterm-style monitoring strip above the status bar: host, CPU, RAM, disk,
// network and uptime/load with mini gauges. Segments drop out by priority when
// the window is narrow. Clicking it toggles a detail card (per-core CPU, memory,
// all filesystems, top processes, OS/kernel).
class MonitorBar : public Panel {
public:
    static constexpr float HEIGHT = 24.0f;

    void set_label(const std::string& label) { label_ = label; }
    void set_snapshot(const SysMonitor::Snapshot& s) { snap_ = s; }
    const SysMonitor::Snapshot& snapshot() const { return snap_; }
    uint64_t version() const { return snap_.version; }
    void clear() { snap_ = {}; detail_ = false; }

    bool detail_open() const { return detail_; }
    void set_detail(bool on) { detail_ = on; }
    Rect detail_rect() const { return card_; }

    void render(RenderContext& ctx, Rect bounds) override;
    // The detail card, drawn over everything else; `window` is the whole window.
    void render_detail(RenderContext& ctx, Rect window);
    bool handle_mouse(const MouseEvent& e) override;  // click toggles the card

private:
    std::string label_;
    SysMonitor::Snapshot snap_;
    bool detail_ = false;
    Rect card_{};
    float detail_body(RenderContext& ctx, float x, float y, float w, bool draw, int column);
};

} // namespace oh
