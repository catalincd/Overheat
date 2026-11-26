#include "ui/problems_panel.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include "ui/file_icons.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace oh {

namespace {
constexpr uint32_t ICON_ERROR = 0xEA87, ICON_WARNING = 0xEA6C, ICON_INFO = 0xEA74;
constexpr uint32_t CHEVRON_DOWN = 0xEAB4, CHEVRON_RIGHT = 0xEAB6, ICON_FILTER = 0xEAF1;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int sev_slot(int severity) { return std::clamp(severity, 1, 4) - 1; }
} // namespace

int ProblemsPanel::count(int severity) const {
    int n = 0;
    for (auto& it : items_)
        if (it.severity == severity || (severity == 3 && it.severity == 4)) n++;
    return n;
}

bool ProblemsPanel::visible_item(const Item& it) const {
    if (!show_[sev_slot(it.severity)]) return false;
    if (filter_.empty()) return true;
    std::string f = lower(filter_);
    return lower(it.message).find(f) != std::string::npos || lower(it.file).find(f) != std::string::npos ||
           lower(it.code).find(f) != std::string::npos || lower(it.source).find(f) != std::string::npos;
}

void ProblemsPanel::set_items(std::vector<Item> items) {
    // Keep the selection on the same problem when the list is rebuilt.
    std::string sel_file;
    size_t sel_line = 0;
    std::string sel_msg;
    if (selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) {
        const Row& r = rows_[static_cast<size_t>(selected_)];
        sel_file = files_[static_cast<size_t>(r.file)];
        if (r.item >= 0) sel_line = items_[static_cast<size_t>(r.item)].line, sel_msg = items_[static_cast<size_t>(r.item)].message;
    }
    items_ = std::move(items);
    // Errors first within a file, then by position.
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.file != b.file) return a.file < b.file;
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
    rebuild();
    selected_ = -1;
    for (size_t i = 0; i < rows_.size() && !sel_file.empty(); i++) {
        const Row& r = rows_[i];
        if (files_[static_cast<size_t>(r.file)] != sel_file) continue;
        if (r.item < 0 && sel_msg.empty()) { selected_ = static_cast<int>(i); break; }
        if (r.item >= 0 && r.note < 0 && items_[static_cast<size_t>(r.item)].line == sel_line &&
            items_[static_cast<size_t>(r.item)].message == sel_msg) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
}

void ProblemsPanel::rebuild() {
    files_.clear();
    file_items_.clear();
    rows_.clear();
    std::map<std::string, int> index;
    for (size_t i = 0; i < items_.size(); i++) {
        if (!visible_item(items_[i])) continue;
        auto [it, inserted] = index.try_emplace(items_[i].file, static_cast<int>(files_.size()));
        if (inserted) {
            files_.push_back(items_[i].file);
            file_items_.push_back({});
        }
        file_items_[static_cast<size_t>(it->second)].push_back(static_cast<int>(i));
    }
    for (size_t f = 0; f < files_.size(); f++) {
        rows_.push_back({static_cast<int>(f), -1, -1});
        if (std::find(collapsed_.begin(), collapsed_.end(), files_[f]) != collapsed_.end()) continue;
        for (int i : file_items_[f]) {
            rows_.push_back({static_cast<int>(f), i, -1});
            const Item& it = items_[static_cast<size_t>(i)];
            // Notes with a location (include chain, instantiation context, related spans);
            // location-less ones ("for further information visit ...") stay hidden.
            for (size_t n = 0; n < it.notes.size() && n < 12; n++)
                if (!it.notes[n].file.empty()) rows_.push_back({static_cast<int>(f), i, static_cast<int>(n)});
        }
    }
    if (selected_ >= static_cast<int>(rows_.size())) selected_ = static_cast<int>(rows_.size()) - 1;
}

void ProblemsPanel::reveal(const std::string& file, size_t line, size_t col) {
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& r = rows_[i];
        if (r.item < 0 || r.note >= 0) continue;
        const Item& it = items_[static_cast<size_t>(r.item)];
        if (it.file == file && it.line == line && it.col == col) {
            selected_ = static_cast<int>(i);
            scroll_into_view();
            return;
        }
    }
}

void ProblemsPanel::scroll_into_view() {
    if (selected_ < 0) return;
    float list_h = bounds_.y + bounds_.h - list_y_;
    float top = static_cast<float>(selected_) * row_h_;
    if (top < scroll_y_) scroll_y_ = top;
    else if (top + row_h_ > scroll_y_ + list_h) scroll_y_ = top + row_h_ - list_h;
    scroll_y_ = std::max(0.0f, scroll_y_);
}

void ProblemsPanel::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    float cw = font.char_width(), lh = font.line_height();
    row_h_ = std::round(lh + 4.0f);
    auto base = [&](float y, float h) { return std::round(y + (h - lh) * 0.5f + font.ascender()); };
    ctx.r->add_rect(bounds, th.terminal_bg);

    // ---- header: filter box, severity toggles, status ----
    float hy = bounds.y + 4.0f, hh = row_h_ + 4.0f;
    float fw = std::min(320.0f, std::max(120.0f, bounds.w * 0.35f));
    filter_box_ = {bounds.x + 10.0f, hy, fw, hh};
    bool active = focused_ && filter_active_;
    ctx.r->add_rect(filter_box_, th.input_bg, 5.0f, active ? 1.0f : 0.0f, th.accent);
    float fb = base(hy, hh);
    ctx.r->push_clip({filter_box_.x + 2.0f, hy, fw - 4.0f, hh});
    ctx.r->add_char(filter_box_.x + 8.0f, fb, ICON_FILTER, th.fg_muted, font);
    float tx = filter_box_.x + 10.0f + 2.0f * cw;
    if (filter_.empty()) ctx.r->add_text(tx, fb, fw > 230.0f ? "Filter (text, file, code)" : "Filter", th.fg_muted, font);
    float caret = ctx.r->add_text(tx, fb, filter_, th.fg, font);
    if (active) ctx.r->add_rect({caret, hy + 5.0f, 2.0f, hh - 10.0f}, th.accent);
    ctx.r->pop_clip();

    float x = filter_box_.x + fw + 10.0f;
    const uint32_t icons[3] = {ICON_ERROR, ICON_WARNING, ICON_INFO};
    const Color tones[3] = {th.error, th.warning, th.info};
    for (int s = 0; s < 3; s++) {
        std::string n = std::to_string(count(s + 1));
        float w = 3.0f * cw + static_cast<float>(n.size()) * cw;
        toggle_[s] = {x, hy, w, hh};
        bool hover = toggle_[s].contains(ctx.mouse_x, ctx.mouse_y);
        if (show_[s]) ctx.r->add_rect(toggle_[s], th.surface_hover, 5.0f);
        else if (hover) ctx.r->add_rect(toggle_[s], th.surface, 5.0f);
        ctx.r->add_char(x + 0.5f * cw, fb, icons[s], show_[s] ? tones[s] : th.fg_muted, font);
        ctx.r->add_text(x + 2.0f * cw, fb, n, show_[s] ? th.fg : th.fg_muted, font);
        x += w + 6.0f;
    }
    if (!status_.empty()) {
        Color c = status_tone_ == 1 ? th.error : status_tone_ == 2 ? th.git_added : th.fg_muted;
        float sw = static_cast<float>(status_.size()) * cw;  // ASCII status text
        float sx = std::max(x + 2.0f * cw, bounds.x + bounds.w - sw - 14.0f);
        ctx.r->push_clip({x, hy, bounds.x + bounds.w - x, hh});
        ctx.r->add_text(sx, fb, status_, c, font);
        ctx.r->pop_clip();
    }

    // ---- list ----
    list_y_ = hy + hh + 6.0f;
    Rect list{bounds.x, list_y_, bounds.w, bounds.y + bounds.h - list_y_};
    float max_scroll = std::max(0.0f, static_cast<float>(rows_.size()) * row_h_ - list.h);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
    ctx.r->push_clip(list);
    if (rows_.empty()) {
        const char* msg = items_.empty() ? "No problems have been detected in the workspace."
                                         : "No problems match the filter.";
        ctx.r->add_text(bounds.x + 14.0f, base(list.y, row_h_), msg, th.fg_muted, font);
    }
    size_t first = static_cast<size_t>(scroll_y_ / row_h_);
    for (size_t ri = first; ri < rows_.size(); ri++) {
        float ry = list.y + static_cast<float>(ri) * row_h_ - scroll_y_;
        if (ry > list.y + list.h) break;
        const Row& row = rows_[ri];
        Rect rr{bounds.x, ry, bounds.w, row_h_};
        float rb = base(ry, row_h_);
        if (static_cast<int>(ri) == selected_)
            ctx.r->add_rect(rr, focused_ ? th.tree_selection : th.surface_hover);
        else if (rr.contains(ctx.mouse_x, ctx.mouse_y))
            ctx.r->add_rect(rr, th.surface_hover);
        const std::string& file = files_[static_cast<size_t>(row.file)];
        if (row.item < 0) {
            bool collapsed = std::find(collapsed_.begin(), collapsed_.end(), file) != collapsed_.end();
            ctx.r->add_char(bounds.x + 8.0f, rb, collapsed ? CHEVRON_RIGHT : CHEVRON_DOWN, th.fg_muted, font);
            fs::path p = file;
            float fx = file_icons::draw(*ctx.r, font, th, bounds.x + 10.0f + 2.0f * cw, rb, p.filename().string(), false);
            float ex = ctx.r->add_text(fx, rb, p.filename().string(), th.fg, font);
            std::string rel = root_.empty() ? p.parent_path().string() : p.parent_path().lexically_relative(root_).string();
            if (rel == ".") rel.clear();
            if (!rel.empty() && rel.rfind("..", 0) == 0) rel = p.parent_path().string();
            ex = ctx.r->add_text(ex + cw, rb, rel, th.fg_muted, font);
            std::string n = std::to_string(file_items_[static_cast<size_t>(row.file)].size());
            float pw = static_cast<float>(n.size()) * cw + 12.0f;
            ctx.r->add_rect({ex + cw, ry + 3.0f, pw, row_h_ - 6.0f}, th.surface_hover, (row_h_ - 6.0f) * 0.5f);
            ctx.r->add_text(ex + cw + 6.0f, rb, n, th.fg_muted, font);
            continue;
        }
        const Item& it = items_[static_cast<size_t>(row.item)];
        float indent = bounds.x + 16.0f + 2.0f * cw;
        ctx.r->push_clip({bounds.x, ry, bounds.w - 8.0f, row_h_});
        ctx.r->set_fade(bounds.x + bounds.w - 40.0f, bounds.x + bounds.w - 8.0f);
        if (row.note >= 0) {
            const ProblemNote& n = it.notes[static_cast<size_t>(row.note)];
            float nx = indent + 3.0f * cw;
            nx = ctx.r->add_text(nx, rb, n.message.substr(0, n.message.find('\n')), th.fg_muted, font);
            if (!n.file.empty()) {
                std::string where = fs::path(n.file).filename().string();
                if (n.line || n.col)  // gcc's "In instantiation of" context names a file only
                    where += ":" + std::to_string(n.line + 1) + ":" + std::to_string(n.col + 1);
                ctx.r->add_text(nx + 2.0f * cw, rb, where, th.line_number, font);
            }
        } else {
            int s = sev_slot(it.severity);
            ctx.r->add_char(indent, rb, s == 0 ? ICON_ERROR : s == 1 ? ICON_WARNING : ICON_INFO,
                            s == 0 ? th.error : s == 1 ? th.warning : th.info, font);
            float mx = ctx.r->add_text(indent + 2.0f * cw, rb, it.message.substr(0, it.message.find('\n')), th.fg, font);
            std::string src = it.source;
            if (!it.code.empty()) src += (src.empty() ? "" : "(") + it.code + (it.source.empty() ? "" : ")");
            if (!src.empty()) mx = ctx.r->add_text(mx + 2.0f * cw, rb, src, th.fg_muted, font);
            ctx.r->add_text(mx + 2.0f * cw, rb, "[Ln " + std::to_string(it.line + 1) + ", Col " + std::to_string(it.col + 1) + "]",
                            th.line_number, font);
        }
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    ctx.r->pop_clip();
}

void ProblemsPanel::open_row(int ri) {
    if (ri < 0 || ri >= static_cast<int>(rows_.size())) return;
    const Row& row = rows_[static_cast<size_t>(ri)];
    if (row.item < 0) {
        const std::string& f = files_[static_cast<size_t>(row.file)];
        auto it = std::find(collapsed_.begin(), collapsed_.end(), f);
        if (it != collapsed_.end()) collapsed_.erase(it);
        else collapsed_.push_back(f);
        rebuild();
        return;
    }
    const Item& it = items_[static_cast<size_t>(row.item)];
    if (!on_open_) return;
    if (row.note >= 0) {
        const ProblemNote& n = it.notes[static_cast<size_t>(row.note)];
        if (!n.file.empty()) on_open_(n.file, n.line, n.col);
        return;
    }
    on_open_(it.file, it.line, it.col);
}

bool ProblemsPanel::handle_key(const KeyEvent& e) {
    if (!e.pressed) return false;
    bool ctrl = e.mods & KeyMod::Ctrl, alt = e.mods & KeyMod::Alt;
    int n = static_cast<int>(rows_.size());
    switch (e.keycode) {
        case SDLK_DOWN:
            if (n) selected_ = std::min(selected_ + 1, n - 1);
            filter_active_ = false;
            scroll_into_view();
            return true;
        case SDLK_UP:
            if (n) selected_ = std::max(selected_ - 1, 0);
            filter_active_ = false;
            scroll_into_view();
            return true;
        case SDLK_PAGEDOWN:
        case SDLK_PAGEUP: {
            int page = std::max(1, static_cast<int>((bounds_.y + bounds_.h - list_y_) / row_h_) - 1);
            if (n) selected_ = std::clamp(selected_ + (e.keycode == SDLK_PAGEDOWN ? page : -page), 0, n - 1);
            scroll_into_view();
            return true;
        }
        case SDLK_HOME: if (n && !filter_active_) selected_ = 0, scroll_into_view(); return true;
        case SDLK_END: if (n && !filter_active_) selected_ = n - 1, scroll_into_view(); return true;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            if (filter_active_ || selected_ < 0 || selected_ >= n) return true;
            {
                const Row& r = rows_[static_cast<size_t>(selected_)];
                const std::string& f = files_[static_cast<size_t>(r.file)];
                bool collapsed = std::find(collapsed_.begin(), collapsed_.end(), f) != collapsed_.end();
                if (e.keycode == SDLK_LEFT && r.item >= 0) {  // to the file row
                    while (selected_ > 0 && rows_[static_cast<size_t>(selected_)].item >= 0) selected_--;
                } else if ((e.keycode == SDLK_LEFT) != collapsed && r.item < 0) {
                    open_row(selected_);
                }
                scroll_into_view();
            }
            return true;
        case SDLK_RETURN:
        case SDLK_RETURN2:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (e.keycode == SDLK_SPACE && filter_active_) return false;  // typed into the filter
            if (selected_ < 0 && n) selected_ = 0;
            open_row(selected_);
            return true;
        case SDLK_BACKSPACE:
            if (filter_.empty()) return true;
            if (ctrl) filter_.clear();
            else {
                while (!filter_.empty() && (static_cast<unsigned char>(filter_.back()) & 0xC0) == 0x80) filter_.pop_back();
                if (!filter_.empty()) filter_.pop_back();
            }
            filter_active_ = true;
            rebuild();
            return true;
        case SDLK_ESCAPE:
            if (!filter_.empty()) {
                filter_.clear();
                rebuild();
                return true;
            }
            return false;  // the host gives focus back to the editor
        default:
            break;
    }
    return !ctrl && !alt;  // plain keys type into the filter (handle_text)
}

bool ProblemsPanel::handle_text(const std::string& text) {
    if (text == " " && !filter_active_) return true;  // Space opened the row
    filter_ += text;
    filter_active_ = true;
    selected_ = -1;
    rebuild();
    return true;
}

bool ProblemsPanel::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        scroll_y_ = std::max(0.0f, scroll_y_ - e.scroll_y * row_h_ * 3.0f);
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return true;
    if (filter_box_.contains(e.x, e.y)) {
        filter_active_ = true;
        return true;
    }
    for (int s = 0; s < 3; s++)
        if (toggle_[s].contains(e.x, e.y)) {
            show_[s] = !show_[s];
            if (s == 2) show_[3] = show_[2];
            rebuild();
            return true;
        }
    filter_active_ = false;
    if (e.y < list_y_) return true;
    int ri = static_cast<int>((e.y - list_y_ + scroll_y_) / row_h_);
    if (ri < 0 || ri >= static_cast<int>(rows_.size())) return true;
    selected_ = ri;
    open_row(ri);
    return true;
}

} // namespace oh
