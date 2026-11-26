#include "ui/debug_panel.h"
#include "debug/debugger.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace oh {

namespace {
constexpr uint32_t ICON_CHEVRON_RIGHT = 0xEAB6, ICON_CHEVRON_DOWN = 0xEAB4, ICON_ADD = 0xEA60,
                   ICON_CLOSE = 0xEA76, ICON_PLAY = 0xEB2C, ICON_GEAR = 0xEAF8, ICON_CHECK = 0xEAB2,
                   ICON_FRAME = 0xEB8B;

bool looks_numeric(const std::string& v) {
    if (v.empty()) return false;
    size_t i = v[0] == '-' ? 1 : 0;
    return i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]));
}
} // namespace

float DebugPanel::baseline(float y) const {
    return y + (row_h_ - ctx_->font->line_height()) * 0.5f + ctx_->font->ascender();
}

void DebugPanel::row_bg(Rect r, bool selected) {
    const Theme& th = *ctx_->theme;
    if (selected) ctx_->r->add_rect(r, th.tree_selection);
    else if (r.contains(ctx_->mouse_x, ctx_->mouse_y) && list_.contains(ctx_->mouse_x, ctx_->mouse_y))
        ctx_->r->add_rect(r, th.surface_hover);
}

void DebugPanel::checkbox(float x, float y, bool on) {
    const Theme& th = *ctx_->theme;
    float s = std::round(std::min(row_h_ - 8.0f, 14.0f));
    Rect b{x, std::round(y + (row_h_ - s) * 0.5f), s, s};
    if (on) {
        ctx_->r->add_rect(b, th.accent, 3.0f);
        const GlyphInfo* gi = ctx_->font->glyph_sized(ICON_CHECK, static_cast<uint32_t>(s));
        if (gi)
            ctx_->r->add_glyph(std::round(b.x + (b.w - gi->width) * 0.5f - gi->bearing_x),
                               std::round(b.y + (b.h - gi->height) * 0.5f + gi->bearing_y), gi, th.tab_active_bg);
    } else {
        ctx_->r->add_rect(b, th.input_bg, 3.0f, 1.0f, th.fg_muted);
    }
}

void DebugPanel::name_value(float x, float y, float right, const std::string& name, const std::string& value, bool error) {
    const Theme& th = *ctx_->theme;
    const FontAtlas& f = *ctx_->font;
    ctx_->r->push_clip({x, y, std::max(0.0f, right - x), row_h_});
    ctx_->r->set_fade(right - 24.0f, right);
    float b = baseline(y);
    float nx = ctx_->r->add_text(x, b, name, th.property, f);
    if (!value.empty() || error) {
        nx = ctx_->r->add_text(nx, b, ": ", th.fg_muted, f);
        Color vc = error ? th.fg_muted : !value.empty() && value[0] == '"' ? th.string_color
                 : looks_numeric(value) ? th.number : th.fg;
        std::string shown = value.substr(0, value.find('\n'));
        ctx_->r->add_text(nx, b, shown, vc, f);
    }
    ctx_->r->set_fade(0, 0);
    ctx_->r->pop_clip();
}

void DebugPanel::section_header(Section s, const char* title, const std::string& extra, std::function<void()> add) {
    const Theme& th = *ctx_->theme;
    Rect r{bounds_.x, y_, bounds_.w, row_h_};
    bool vis = r.y + r.h > list_.y && r.y < list_.y + list_.h;
    note(std::string("# ") + title + (extra.empty() ? "" : " | " + extra));
    if (vis) {
        ctx_->r->add_rect(r, th.sidebar_bg);
        ctx_->r->add_rect({r.x, r.y, r.w, 1.0f}, th.border);
        float b = baseline(r.y);
        ctx_->r->add_char(r.x + 6.0f, b, collapsed_[s] ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, th.fg_muted, *ctx_->font);
        float x = ctx_->r->add_text(r.x + 8.0f + ctx_->font->char_width() * 1.6f, b, title, th.fg, *ctx_->font);
        float right = r.x + r.w - 8.0f;
        if (add) {
            Rect ab{right - 20.0f, r.y + 1.0f, 20.0f, row_h_ - 2.0f};
            bool hov = ab.contains(ctx_->mouse_x, ctx_->mouse_y);
            if (hov) ctx_->r->add_rect(ab, th.surface_hover, 4.0f);
            ctx_->r->add_char(ab.x + (ab.w - ctx_->font->char_width()) * 0.5f, b, ICON_ADD, hov ? th.fg : th.fg_muted, *ctx_->font);
            hits_.push_back({ab, [add](int, bool) { add(); }});
            right -= 24.0f;
        }
        if (!extra.empty()) {
            ctx_->r->push_clip({x + 12.0f, r.y, std::max(0.0f, right - x - 12.0f), r.h});
            float w = static_cast<float>(extra.size()) * ctx_->font->char_width();
            ctx_->r->add_text(std::max(x + 12.0f, right - w), b, extra, th.fg_muted, *ctx_->font);
            ctx_->r->pop_clip();
        }
        hits_.push_back({r, [this, s](int, bool) { collapsed_[s] = !collapsed_[s]; }});
    }
    y_ += row_h_;
}

void DebugPanel::variable_rows(const DebugVariable& v, const std::string& path, int depth, int guard) {
    const Theme& th = *ctx_->theme;
    float cw = ctx_->font->char_width();
    Rect r{bounds_.x, y_, bounds_.w, row_h_};
    bool open = v.has_children() && dbg_->expanded(path);
    float x = bounds_.x + 10.0f + static_cast<float>(depth) * cw * 1.6f;
    std::string value = dbg_->display_value(v);
    note(std::string(static_cast<size_t>(depth) * 2, ' ') + v.name + " = " + value);
    if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
        row_bg(r, false);
        float b = baseline(r.y);
        if (v.has_children()) ctx_->r->add_char(x, b, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, th.fg_muted, *ctx_->font);
        name_value(x + cw * 1.6f, r.y, r.x + r.w - 6.0f, v.name, value, false);
        if (v.has_children())
            hits_.push_back({r, [this, path, open](int, bool) { dbg_->set_expanded(path, !open); }});
    }
    y_ += row_h_;
    if (!open || guard > 24) return;
    const Debugger::Children* ch = dbg_->children(v.ref);
    if (!ch || ch->loading) {
        if (r.y + 2 * r.h > list_.y && r.y + r.h < list_.y + list_.h)
            ctx_->r->add_text(x + cw * 3.2f, baseline(y_), "loading...", th.fg_muted, *ctx_->font);
        y_ += row_h_;
        return;
    }
    if (!ch->error.empty()) {
        ctx_->r->add_text(x + cw * 3.2f, baseline(y_), ch->error, th.fg_muted, *ctx_->font);
        y_ += row_h_;
    }
    for (auto& c : ch->vars) variable_rows(c, path + "/" + c.name, depth + 1, guard + 1);
    if (ch->total_indexed > static_cast<int64_t>(ch->vars.size())) {
        ctx_->r->add_text(x + cw * 3.2f, baseline(y_),
                          "... " + std::to_string(ch->total_indexed - static_cast<int64_t>(ch->vars.size())) + " more",
                          th.fg_muted, *ctx_->font);
        y_ += row_h_;
    }
}

void DebugPanel::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    ctx_ = &ctx;
    hits_.clear();
    row_text_.clear();
    const Theme& th = *ctx.theme;
    const FontAtlas& f = *ctx.font;
    float cw = f.char_width(), lh = f.line_height();
    row_h_ = std::round(lh + 4.0f);
    ctx.r->add_rect(bounds, th.sidebar_bg);
    if (!dbg_) return;

    // Title and the configuration bar.
    float title_h = 30.0f;
    ctx.r->add_text(bounds.x + 14.0f, bounds.y + (title_h - lh) * 0.5f + f.ascender(), "RUN AND DEBUG", th.fg_muted, f);
    Rect bar{bounds.x + 10.0f, bounds.y + title_h, bounds.w - 20.0f, lh + 10.0f};
    ctx.r->add_rect(bar, th.input_bg, 6.0f);
    float bb = bar.y + (bar.h - lh) * 0.5f + f.ascender();
    Rect play{bar.x, bar.y, bar.h + 4.0f, bar.h};
    bool active = dbg_->active();
    Color pc = active ? th.fg_muted : th.git_added;
    if (play.contains(ctx.mouse_x, ctx.mouse_y) && !active) ctx.r->add_rect(play, th.surface_hover, 6.0f);
    ctx.r->add_char(play.x + (play.w - cw) * 0.5f, bb, ICON_PLAY, pc, f);
    hits_.push_back({play, [this](int, bool) { if (cb_.start) cb_.start(); }, false});
    Rect gear{bar.x + bar.w - bar.h - 2.0f, bar.y, bar.h + 2.0f, bar.h};
    if (gear.contains(ctx.mouse_x, ctx.mouse_y)) ctx.r->add_rect(gear, th.surface_hover, 6.0f);
    ctx.r->add_char(gear.x + (gear.w - cw) * 0.5f, bb, ICON_GEAR, th.fg_muted, f);
    hits_.push_back({gear, [this](int, bool) { if (cb_.open_launch_file) cb_.open_launch_file(); }, false});
    Rect pick{play.x + play.w, bar.y, gear.x - play.x - play.w, bar.h};
    if (pick.contains(ctx.mouse_x, ctx.mouse_y)) ctx.r->add_rect(pick, th.surface_hover, 4.0f);
    std::string name = dbg_->selected_config_name();
    if (name.empty()) name = "No configurations";
    ctx.r->push_clip({pick.x + 4.0f, pick.y, std::max(0.0f, pick.w - cw * 2.0f - 8.0f), pick.h});
    ctx.r->add_text(pick.x + 6.0f, bb, name, th.fg, f);
    ctx.r->pop_clip();
    ctx.r->add_char(pick.x + pick.w - cw * 1.6f, bb, ICON_CHEVRON_DOWN, th.fg_muted, f);
    hits_.push_back({pick, [this](int, bool) { if (cb_.pick_config) cb_.pick_config(); }, false});
    note("config: " + name + " state: " + debug_state_name(dbg_->state()));
    float top = bar.y + bar.h + 8.0f;

    list_ = {bounds.x, top, bounds.w, bounds.y + bounds.h - top};
    ctx.r->push_clip(list_);
    y_ = list_.y - scroll_y_;
    bool stopped = dbg_->stopped();

    // ---- Variables
    section_header(Variables, "VARIABLES", "");
    if (!collapsed_[Variables]) {
        if (!stopped) {
            ctx.r->add_text(bounds.x + 22.0f, baseline(y_), dbg_->active() ? "Running..." : "Not paused", th.fg_muted, f);
            y_ += row_h_;
        }
        for (auto& sc : dbg_->scopes()) {
            DebugVariable sv;
            sv.name = sc.name;
            sv.ref = sc.ref;
            bool open = dbg_->expanded(sc.name);
            Rect r{bounds.x, y_, bounds.w, row_h_};
            note("scope " + sc.name);
            if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                row_bg(r, false);
                float b = baseline(r.y);
                ctx.r->add_char(bounds.x + 10.0f, b, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, th.fg_muted, f);
                ctx.r->add_text(bounds.x + 10.0f + cw * 1.6f, b, sc.name, th.fg, f);
                std::string path = sc.name;
                hits_.push_back({r, [this, path, open](int, bool) { dbg_->set_expanded(path, !open); }});
            }
            y_ += row_h_;
            if (!open) continue;
            const Debugger::Children* ch = dbg_->children(sc.ref);
            if (!ch || ch->loading) {
                ctx.r->add_text(bounds.x + 22.0f + cw * 1.6f, baseline(y_), "loading...", th.fg_muted, f);
                y_ += row_h_;
                continue;
            }
            for (auto& v : ch->vars) variable_rows(v, sc.name + "/" + v.name, 1, 0);
        }
    }

    // ---- Watch
    section_header(Watch, "WATCH", "", [this] { if (cb_.add_watch) cb_.add_watch(); });
    if (!collapsed_[Watch]) {
        auto& ws = dbg_->watches();
        for (size_t i = 0; i < ws.size(); i++) {
            const DebugWatch& w = ws[i];
            Rect r{bounds.x, y_, bounds.w, row_h_};
            std::string path = "watch:" + std::to_string(i) + ":" + w.expr;
            bool expandable = stopped && w.ref > 0;
            bool open = expandable && dbg_->expanded(path);
            std::string value = !stopped ? "not available" : w.value;
            if (stopped && w.value.empty() && w.ref > 0) {
                DebugVariable tmp;
                tmp.ref = w.ref;
                tmp.type = w.type;
                value = dbg_->display_value(tmp);
            }
            note("watch " + w.expr + " = " + value);
            if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                row_bg(r, false);
                float b = baseline(r.y);
                float x = bounds.x + 10.0f + cw * 1.6f;
                if (expandable) ctx.r->add_char(x - cw * 1.6f, b, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, th.fg_muted, f);
                bool hov = r.contains(ctx.mouse_x, ctx.mouse_y);
                float right = r.x + r.w - (hov ? 28.0f : 6.0f);
                name_value(x, r.y, right, w.expr, value, w.error || !stopped);
                if (hov) {
                    Rect xb{r.x + r.w - 26.0f, r.y + 1.0f, 20.0f, row_h_ - 2.0f};
                    ctx.r->add_char(xb.x + (xb.w - cw) * 0.5f, b, ICON_CLOSE, th.fg_muted, f);
                    hits_.push_back({xb, [this, i](int, bool) { dbg_->remove_watch(i); }});
                }
                hits_.push_back({r, [this, i, path, open, expandable](int button, bool dbl) {
                    if (dbl || button == 3) {
                        if (cb_.edit_watch) cb_.edit_watch(i);
                    } else if (expandable) {
                        dbg_->set_expanded(path, !open);
                    }
                }});
            }
            y_ += row_h_;
            if (open) {
                const Debugger::Children* ch = dbg_->children(w.ref);
                if (ch && !ch->loading)
                    for (auto& c : ch->vars) variable_rows(c, path + "/" + c.name, 2, 0);
            }
        }
        if (ws.empty()) {
            ctx.r->add_text(bounds.x + 22.0f, baseline(y_), "No expressions (+ to add)", th.fg_muted, f);
            y_ += row_h_;
        }
    }

    // ---- Call stack
    section_header(CallStack, "CALL STACK", stopped ? dbg_->stop_description() : dbg_->active() ? "Running" : "");
    if (!collapsed_[CallStack]) {
        auto& threads = dbg_->threads();
        bool show_threads = threads.size() > 1;
        for (auto& t : threads) {
            int depth = 0;
            if (show_threads) {
                Rect r{bounds.x, y_, bounds.w, row_h_};
                bool sel_thread = t.id == dbg_->selected_thread();
                std::string label = t.name.empty() ? "Thread " + std::to_string(t.id) : t.name;
                std::string status = stopped ? (sel_thread ? "PAUSED" : "paused") : "running";
                note("thread " + label + " " + status);
                if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                    row_bg(r, false);
                    float b = baseline(r.y);
                    ctx.r->add_char(bounds.x + 10.0f, b, t.expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, th.fg_muted, f);
                    float x = ctx.r->add_text(bounds.x + 10.0f + cw * 1.6f, b, label, th.fg, f);
                    ctx.r->add_text(x + cw, b, "#" + std::to_string(t.id), th.fg_muted, f);
                    float sw = static_cast<float>(status.size()) * cw;
                    ctx.r->add_text(r.x + r.w - sw - 10.0f, b, status, th.fg_muted, f);
                    int64_t id = t.id;
                    bool exp = t.expanded;
                    hits_.push_back({r, [this, id, exp](int, bool) { dbg_->expand_thread(id, !exp); }});
                }
                y_ += row_h_;
                if (!t.expanded) continue;
                depth = 1;
            }
            if (t.loading && t.frames.empty()) {
                ctx.r->add_text(bounds.x + 22.0f, baseline(y_), "loading...", th.fg_muted, f);
                y_ += row_h_;
            }
            for (size_t i = 0; i < t.frames.size(); i++) {
                const DebugFrame& fr = t.frames[i];
                Rect r{bounds.x, y_, bounds.w, row_h_};
                bool sel = stopped && t.id == dbg_->selected_thread() && static_cast<int>(i) == dbg_->selected_frame();
                std::string loc = fr.path.empty() ? "" : fr.path.filename().string() + ":" + std::to_string(fr.line + 1);
                note(std::string(sel ? "* " : "  ") + "frame " + fr.name + " " + loc);
                if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                    row_bg(r, sel);
                    float b = baseline(r.y);
                    float x = bounds.x + 10.0f + static_cast<float>(depth) * cw * 1.6f;
                    if (sel) ctx.r->add_char(x, b, ICON_FRAME, i == 0 ? th.warning : th.git_added, f);
                    float lw = static_cast<float>(loc.size()) * cw;
                    float right = r.x + r.w - lw - 14.0f;
                    ctx.r->push_clip({x, r.y, std::max(0.0f, right - x - 4.0f), r.h});
                    ctx.r->set_fade(right - 24.0f, right - 4.0f);
                    Color nc = fr.path.empty() || fr.hint == "subtle" ? th.fg_muted : th.fg;
                    ctx.r->add_text(x + cw * 1.6f, b, fr.name, nc, f);
                    ctx.r->set_fade(0, 0);
                    ctx.r->pop_clip();
                    ctx.r->add_text(r.x + r.w - lw - 10.0f, b, loc, th.fg_muted, f);
                    int64_t id = t.id;
                    int idx = static_cast<int>(i);
                    hits_.push_back({r, [this, id, idx](int, bool) { dbg_->select_frame(id, idx, true); }});
                }
                y_ += row_h_;
            }
        }
    }

    // ---- Breakpoints
    section_header(Breakpoints, "BREAKPOINTS", "", [this] { if (cb_.add_function_breakpoint) cb_.add_function_breakpoint(); });
    if (!collapsed_[Breakpoints]) {
        auto& bps = dbg_->breakpoints();
        for (auto& filt : dbg_->exception_filters()) {
            Rect r{bounds.x, y_, bounds.w, row_h_};
            bool on = bps.filter_on(filt.id);
            note(std::string(on ? "[x] " : "[ ] ") + filt.label);
            if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                row_bg(r, false);
                checkbox(bounds.x + 14.0f, r.y, on);
                ctx.r->add_text(bounds.x + 22.0f + cw * 2.0f, baseline(r.y), filt.label, th.fg, f);
                std::string id = filt.id;
                hits_.push_back({r, [this, id, on](int, bool) { dbg_->set_exception_filter(id, !on); }});
            }
            y_ += row_h_;
        }
        for (auto& fb : bps.functions()) {
            Rect r{bounds.x, y_, bounds.w, row_h_};
            note(std::string(fb.enabled ? "[x] " : "[ ] ") + "function " + fb.name);
            if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                row_bg(r, false);
                checkbox(bounds.x + 14.0f, r.y, fb.enabled);
                float x = ctx.r->add_text(bounds.x + 22.0f + cw * 2.0f, baseline(r.y), fb.name, th.function_name, f);
                ctx.r->add_text(x + cw, baseline(r.y), "function", th.fg_muted, f);
                std::string n = fb.name;
                bool hov = r.contains(ctx.mouse_x, ctx.mouse_y);
                if (hov) {
                    Rect xb{r.x + r.w - 26.0f, r.y + 1.0f, 20.0f, row_h_ - 2.0f};
                    ctx.r->add_char(xb.x + (xb.w - cw) * 0.5f, baseline(r.y), ICON_CLOSE, th.fg_muted, f);
                    hits_.push_back({xb, [this, n](int, bool) {
                        std::erase_if(dbg_->breakpoints().functions(), [&](auto& x) { return x.name == n; });
                        dbg_->function_breakpoints_changed();
                    }});
                }
                hits_.push_back({r, [this, n](int, bool) {
                    for (auto& x : dbg_->breakpoints().functions())
                        if (x.name == n) x.enabled = !x.enabled;
                    dbg_->function_breakpoints_changed();
                }});
            }
            y_ += row_h_;
        }
        for (auto& b : bps.source()) {
            Rect r{bounds.x, y_, bounds.w, row_h_};
            std::string label = b.path.filename().string();
            std::string line = std::to_string(b.shown_line() + 1);
            std::string detail = b.is_logpoint() ? "log: " + b.log_message
                               : !b.condition.empty() ? "if " + b.condition
                               : !b.hit_condition.empty() ? "hits " + b.hit_condition : "";
            bool unverified = dbg_->active() && b.enabled && !b.verified;
            note(std::string(b.enabled ? "[x] " : "[ ] ") + label + ":" + line +
                                (detail.empty() ? "" : " " + detail) + (unverified ? " (unverified)" : ""));
            if (r.y + r.h > list_.y && r.y < list_.y + list_.h) {
                row_bg(r, false);
                checkbox(bounds.x + 14.0f, r.y, b.enabled);
                float b0 = baseline(r.y);
                bool hov = r.contains(ctx.mouse_x, ctx.mouse_y);
                float right = r.x + r.w - (hov ? 30.0f : 8.0f);
                float lw = static_cast<float>(line.size()) * cw;
                ctx.r->add_rect({right - lw - 8.0f, r.y + 3.0f, lw + 8.0f, row_h_ - 6.0f}, th.surface_hover, (row_h_ - 6.0f) * 0.5f);
                ctx.r->add_text(right - lw - 4.0f, b0, line, th.fg_muted, f);
                float x0 = bounds.x + 22.0f + cw * 2.0f;
                ctx.r->push_clip({x0, r.y, std::max(0.0f, right - lw - 14.0f - x0), r.h});
                float x = ctx.r->add_text(x0, b0, label, unverified ? th.fg_muted : th.fg, f);
                if (!detail.empty()) x = ctx.r->add_text(x + cw, b0, detail, th.fg_muted, f);
                else x = ctx.r->add_text(x + cw, b0, b.path.parent_path().filename().string(), th.fg_muted, f);
                ctx.r->pop_clip();
                fs::path p = b.path;
                size_t ln = b.shown_line();
                if (hov) {
                    Rect xb{r.x + r.w - 26.0f, r.y + 1.0f, 20.0f, row_h_ - 2.0f};
                    ctx.r->add_char(xb.x + (xb.w - cw) * 0.5f, b0, ICON_CLOSE, th.fg_muted, f);
                    hits_.push_back({xb, [this, p, ln](int, bool) {
                        dbg_->breakpoints().remove(p, ln);
                        dbg_->breakpoints_changed(p);
                    }});
                }
                Rect cb{bounds.x + 8.0f, r.y, cw * 2.0f + 10.0f, r.h};
                hits_.push_back({cb, [this, p, ln](int, bool) {
                    if (SourceBreakpoint* sb = dbg_->breakpoints().find(p, ln)) sb->enabled = !sb->enabled;
                    dbg_->breakpoints_changed(p);
                }});
                hits_.push_back({r, [this, p, ln](int button, bool) {
                    if (button == 3) {
                        if (cb_.edit_breakpoint) cb_.edit_breakpoint(p, ln);
                    } else if (cb_.open_location) {
                        cb_.open_location(p, ln);
                    }
                }});
            }
            y_ += row_h_;
        }
    }
    content_h_ = y_ + scroll_y_ - list_.y;
    ctx.r->pop_clip();
}

bool DebugPanel::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        float max_scroll = std::max(0.0f, content_h_ - list_.h + row_h_);
        scroll_y_ = std::clamp(scroll_y_ - e.scroll_y * row_h_ * 3.0f, 0.0f, max_scroll);
        return true;
    }
    if (!e.pressed || e.motion) return true;
    // Later hits are rows; buttons inside rows were pushed first, so the first match wins.
    for (auto& h : hits_) {
        if (!h.rect.contains(e.x, e.y)) continue;
        if (h.in_list && !list_.contains(e.x, e.y)) continue;
        auto fn = h.fn;  // the callback may re-render
        fn(e.button, e.double_click);
        return true;
    }
    return true;
}

} // namespace oh
