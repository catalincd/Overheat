#include "ui/status_bar.h"
#include "anim.h"
#include "editor/utf8.h"
#include "editor/editor.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oh {

namespace {

constexpr uint32_t SPINNER = 1;  // part icon drawn as a dot spinner, not a glyph
constexpr float PAD = 8.0f;      // inside an item, each side
constexpr float PART_GAP = 6.0f;

size_t cp_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

float luminance(Color c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
float saturation(Color c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
float distance(Color a, Color b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

struct Part {
    uint32_t icon = 0;
    std::string text;
    bool tinted = false;  // icon takes `tint` instead of the bar foreground
    Color tint{};
};

struct Item {
    std::string id;
    std::vector<Part> parts;
    bool right = false;
    int prio = 0;  // higher survives longer at narrow widths
    bool badge = false;
    float w = 0;
};

} // namespace

const std::vector<std::pair<std::string, std::string>>& StatusBar::item_names() {
    static const std::vector<std::pair<std::string, std::string>> names = {
        {"remote", "Remote Host"},      {"branch", "Git Branch"},          {"sync", "Git Sync"},
        {"problems", "Problems"},       {"tasks", "Background Tasks"},     {"position", "Cursor Position"},
        {"indent", "Indentation"},      {"encoding", "Encoding"},          {"eol", "Line Endings"},
        {"language", "Language Mode"},  {"lsp", "Language Server"},        {"bell", "Notifications"}};
    return names;
}

void StatusBar::set_message(const std::string& msg, float duration) {
    message_ = msg;
    message_timer_ = duration;
}

void StatusBar::update(float dt) {
    if (message_timer_ > 0) {
        message_timer_ -= dt;
        if (message_timer_ <= 0) {
            message_timer_ = 0;
            message_.clear();
        }
    }
}

bool StatusBar::handle_mouse(const MouseEvent& e) {
    if (!e.pressed || e.motion || !bounds_.contains(e.x, e.y) || !on_click_) return false;
    if (e.button == 3) {
        on_click_("menu");
        return true;
    }
    if (e.button != 1) return false;
    for (auto& [r, id] : hits_)
        if (r.contains(e.x, e.y)) {
            on_click_(id);
            return true;
        }
    return false;
}

void StatusBar::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    hits_.clear();
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    Renderer2D& r = *ctx.r;
    float cw = font.char_width();
    float baseline = std::round(bounds.y + (bounds.h - font.line_height()) * 0.5f + font.ascender());
    r.add_rect(bounds, th.status_bg);

    // A coloured bar (Dark+/Light+ blue) keeps every glyph in the bar foreground,
    // like VS Code; a neutral one tints error/warning icons.
    bool neutral = saturation(th.status_bg) < 0.15f;
    bool dark_bar = luminance(th.status_bg) < 0.5f;
    Color hover = dark_bar ? Color{1, 1, 1, 0.12f} : Color{0, 0, 0, 0.08f};
    Color badge_bg = distance(th.accent, th.status_bg) < 0.3f ? Color::hex(0x16825d) : th.accent;
    Color badge_fg = luminance(badge_bg) < 0.6f ? Color::hex(0xffffff) : Color::hex(0x1e1e1e);

    std::vector<Item> items;
    auto add = [&](std::string id, bool right, int prio, std::vector<Part> parts) -> Item* {
        if (hidden_.count(id)) return nullptr;
        items.push_back({std::move(id), std::move(parts), right, prio});
        return &items.back();
    };

    // ---- left ----
    {
        std::vector<Part> p;
        if (remote_.empty()) p.push_back({0xEB3A, ""});
        else p.push_back({0xEB3A, remote_});
        if (Item* it = add("remote", false, 88, std::move(p))) it->badge = true;
    }
    if (!git_branch_.empty()) {
        add("branch", false, 95, {{0xEA68, git_branch_ + (git_dirty_ ? "*" : "")}});
        if (!upstream_ && has_remote_) {
            add("sync", false, 70, {{0xEAC3, ""}});  // cloud-upload: publish branch
        } else if (upstream_) {
            std::vector<Part> p{{0xEA77, ""}};
            if (behind_ || ahead_) {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%d↓ %d↑", behind_, ahead_);
                p[0].text = buf;
            }
            add("sync", false, 70, std::move(p));
        }
    }
    add("problems", false, 90,
        {{0xEA87, std::to_string(errors_), neutral && errors_, th.error},
         {0xEA6C, std::to_string(warnings_), neutral && warnings_, th.warning}});
    if (!tasks_.empty()) {
        std::string t = tasks_.front();
        if (tasks_.size() > 1) t += " (+" + std::to_string(tasks_.size() - 1) + ")";
        add("tasks", false, 60, {{SPINNER, t}});
    }

    // ---- right (display order, left to right) ----
    if (editor_) {
        const TextBuffer& buf = editor_->buffer();
        auto pos = editor_->cursors().primary_pos();
        size_t col = utf8::display_col(buf.line_text(pos.line), pos.col);
        char tmp[96];
        std::snprintf(tmp, sizeof tmp, "Ln %zu, Col %zu", pos.line + 1, col + 1);
        std::string text = tmp;
        size_t selected = 0;
        auto& sels = editor_->cursors().selections();
        for (auto& s : sels) {
            if (s.empty()) continue;
            size_t a = buf.offset_of(s.min().line, s.min().col), b = buf.offset_of(s.max().line, s.max().col);
            selected += b - a;
        }
        if (sels.size() > 1) {
            std::snprintf(tmp, sizeof tmp, "%zu selections", sels.size());
            text = tmp;
        }
        if (selected) {
            std::snprintf(tmp, sizeof tmp, " (%zu selected)", selected);
            text += tmp;
        }
        add("position", true, 100, {{0, text}});
        auto ind = editor_->indent_style();
        add("indent", true, 45, {{0, ind.tabs ? "Tab Size: " + std::to_string(ind.size) : "Spaces: " + std::to_string(ind.size)}});
        if (format_editor_ != editor_.get() || format_version_ != buf.version()) {
            format_editor_ = editor_.get();
            format_version_ = buf.version();
            crlf_ = editor_->uses_crlf();
            bom_ = editor_->has_bom();
        }
        add("encoding", true, 40, {{0, bom_ ? "UTF-8 with BOM" : "UTF-8"}});
        add("eol", true, 50, {{0, crlf_ ? "CRLF" : "LF"}});
        std::string lang = editor_->syntax().language_id();
        add("language", true, 80, {{0, language_name(lang)}});
    }
    if (!lsp_name_.empty())
        add("lsp", true, 65, {{lsp_state_ == 0 ? SPINNER : lsp_state_ == 1 ? 0xEAB2u : 0xEABDu, lsp_name_}});
    add("bell", true, 75, {{unread_ ? 0xEB9Au : 0xEAA2u, ""}});

    // Widths, then drop the least important items until everything fits (keeping
    // a little room for a transient message).
    auto part_w = [&](const Part& p) {
        float w = static_cast<float>(cp_count(p.text)) * cw;
        if (p.icon) w += cw + (p.text.empty() ? 0.0f : 4.0f);
        return w;
    };
    for (auto& it : items) {
        it.w = 0;
        for (size_t i = 0; i < it.parts.size(); i++) it.w += part_w(it.parts[i]) + (i ? PART_GAP : 0.0f);
        it.w = std::round(it.w + 2 * PAD);
    }
    auto total = [&] {
        float t = 0;
        for (auto& it : items) t += it.w;
        return t;
    };
    float avail = bounds.w - 8.0f;
    while (!items.empty() && total() > avail) {
        auto it = std::min_element(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.prio < b.prio; });
        items.erase(it);
    }

    bool spinning = false;
    auto draw_item = [&](const Item& it, float x) {
        Rect box{x, bounds.y, it.w, bounds.h};
        bool hot = box.contains(ctx.mouse_x, ctx.mouse_y);
        Color fg = it.badge ? badge_fg : th.status_fg;
        if (it.badge) r.add_rect(box, badge_bg);
        if (hot) r.add_rect(box, hover);
        hits_.push_back({box, it.id});
        float px = x + PAD;
        for (size_t i = 0; i < it.parts.size(); i++) {
            const Part& p = it.parts[i];
            if (i) px += PART_GAP;
            if (p.icon == SPINNER) {
                // Eight dots around a circle, brightest at the leading edge.
                spinning = true;
                float cx = px + cw * 0.5f, cy = bounds.y + bounds.h * 0.5f, rad = std::min(4.5f, cw * 0.5f);
                int lead = static_cast<int>(spin_ * 12.0f) % 8;
                for (int d = 0; d < 8; d++) {
                    float a = static_cast<float>(d) * 3.14159265f / 4.0f;
                    Color c = fg;
                    c.a *= 0.25f + 0.75f * static_cast<float>((d - lead + 8) % 8) / 7.0f;
                    r.add_rect({std::round(cx + std::cos(a) * rad) - 1.25f, std::round(cy + std::sin(a) * rad) - 1.25f, 2.5f, 2.5f},
                               c, 1.25f);
                }
                px += cw + (p.text.empty() ? 0.0f : 4.0f);
            } else if (p.icon) {
                r.add_char(px, baseline, p.icon, p.tinted ? p.tint : fg, font);
                px += cw + (p.text.empty() ? 0.0f : 4.0f);
            }
            if (!p.text.empty()) px = r.add_text(px, baseline, p.text, fg, font);
        }
    };

    float lx = bounds.x;
    float rw = 0;
    for (auto& it : items)
        if (it.right) rw += it.w;
    float rx = bounds.x + bounds.w - 4.0f - rw;
    for (auto& it : items) {
        if (it.right) {
            draw_item(it, rx);
            rx += it.w;
        } else {
            draw_item(it, lx);
            lx += it.w;
        }
    }

    // Transient message in whatever room is left between the two sides.
    float right_start = bounds.x + bounds.w - 4.0f - rw;
    if (!message_.empty() && right_start - lx > 3 * cw) {
        float mx = lx + PAD;
        r.push_clip({mx, bounds.y, right_start - mx - 4.0f, bounds.h});
        r.set_fade(right_start - 28.0f, right_start - 4.0f);
        r.add_text(mx, baseline, message_, th.status_fg, font);
        r.set_fade(0, 0);
        r.pop_clip();
    }

    if (spinning) {
        spin_ += ctx.dt;
        anim::active = true;  // keep frames coming while something spins
    }
}

std::string StatusBar::language_name(const std::string& id) {
    for (auto& [lid, name] : languages())
        if (id == lid) return name;
    return id.empty() ? "Plain Text" : id;
}

const std::vector<std::pair<std::string, std::string>>& StatusBar::languages() {
    static const std::vector<std::pair<std::string, std::string>> langs = {
        {"", "Plain Text"},        {"bash", "Shell Script"},  {"c", "C"},
        {"cpp", "C++"},            {"cmake", "CMake"},        {"css", "CSS"},
        {"dockerfile", "Dockerfile"}, {"glsl", "GLSL"},       {"go", "Go"},
        {"html", "HTML"},          {"java", "Java"},          {"javascript", "JavaScript"},
        {"javascriptreact", "JavaScript JSX"}, {"json", "JSON"}, {"lua", "Lua"},
        {"make", "Makefile"},      {"markdown", "Markdown"},  {"python", "Python"},
        {"ruby", "Ruby"},          {"rust", "Rust"},          {"sql", "SQL"},
        {"toml", "TOML"},          {"typescript", "TypeScript"}, {"typescriptreact", "TypeScript JSX"},
        {"xml", "XML"},            {"yaml", "YAML"},          {"zig", "Zig"}};
    return langs;
}

} // namespace oh
