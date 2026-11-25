#include "ui/search_panel.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void SearchPanel::set_results(std::vector<SearchHit> hits, bool truncated) {
    hits_ = std::move(hits);
    truncated_ = truncated;
    busy_ = false;
    files_.clear();
    file_hits_.clear();
    for (size_t i = 0; i < hits_.size(); i++) {
        if (files_.empty() || files_.back() != hits_[i].file) {
            files_.push_back(hits_[i].file);
            file_hits_.push_back({});
        }
        file_hits_.back().push_back(static_cast<int>(i));
    }
    collapsed_.assign(files_.size(), false);
    scroll_y_ = 0;
    rebuild_rows();
}

void SearchPanel::rebuild_rows() {
    rows_.clear();
    for (size_t f = 0; f < files_.size(); f++) {
        rows_.push_back({static_cast<int>(f), -1});
        if (!collapsed_[f])
            for (int h : file_hits_[f]) rows_.push_back({static_cast<int>(f), h});
    }
}

void SearchPanel::tick(float dt) {
    if (debounce_ < 0) return;
    debounce_ -= dt;
    if (debounce_ <= 0) {
        debounce_ = -1;
        busy_ = !query_.text.empty();
        if (cb_.run) cb_.run(query_);
    }
}

void SearchPanel::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    row_h_ = std::round(lh + 4.0f);
    auto base = [&](float y, float h) { return y + (h - lh) * 0.5f + ctx.font->ascender(); };
    ctx.r->add_rect(bounds, th.sidebar_bg);
    ctx.r->add_text(bounds.x + 14.0f, base(bounds.y, 30.0f), "SEARCH", th.fg_muted, *ctx.font);

    float toggles_w = 2.0f * (3.0f * cw + 4.0f);
    input_ = {bounds.x + 12.0f, bounds.y + 30.0f, bounds.w - 24.0f, lh + 14.0f};
    bool active = input_focused();
    ctx.r->add_rect(input_, th.input_bg, 6.0f, active ? 1.0f : 0.0f, th.accent);
    Rect text_area{input_.x + 4.0f, input_.y, input_.w - toggles_w - 10.0f, input_.h};
    ctx.r->push_clip(text_area);
    float tb = base(input_.y, input_.h);
    if (query_.text.empty()) ctx.r->add_text(input_.x + 8.0f, tb, "Search", th.fg_muted, *ctx.font);
    float caret = ctx.r->add_text(input_.x + 8.0f, tb, query_.text, th.fg, *ctx.font);
    if (active) ctx.r->add_rect({caret, input_.y + 6.0f, 2.0f, input_.h - 12.0f}, th.accent);
    ctx.r->pop_clip();
    float tx = input_.x + input_.w - toggles_w - 2.0f;
    auto toggle = [&](Rect& r, const char* label, bool on) {
        r = {tx, input_.y + 4.0f, 3.0f * cw, input_.h - 8.0f};
        if (on) ctx.r->add_rect(r, th.selection, 4.0f);
        ctx.r->add_text(r.x + 0.5f * cw, tb, label, on ? th.fg : th.fg_muted, *ctx.font);
        tx += r.w + 4.0f;
    };
    toggle(case_btn_, "Aa", query_.case_sensitive);
    toggle(word_btn_, "ab", query_.whole_word);

    float y = input_.y + input_.h + 8.0f;
    std::string summary;
    if (busy_) summary = "Searching...";
    else if (!query_.text.empty())
        summary = std::to_string(hits_.size()) + (truncated_ ? "+" : "") + " results in " +
                  std::to_string(files_.size()) + " files";
    ctx.r->add_text(bounds.x + 14.0f, base(y, row_h_), summary, th.fg_muted, *ctx.font);
    y += row_h_ + 4.0f;
    list_y_ = y;

    Rect list{bounds.x, y, bounds.w, bounds.y + bounds.h - y};
    ctx.r->push_clip(list);
    size_t first = static_cast<size_t>(scroll_y_ / row_h_);
    for (size_t ri = first; ri < rows_.size(); ri++) {
        float ry = list.y + static_cast<float>(ri) * row_h_ - scroll_y_;
        if (ry > list.y + list.h) break;
        const Row& row = rows_[ri];
        float rb = base(ry, row_h_);
        if (row.hit < 0) {
            const fs::path& f = files_[static_cast<size_t>(row.file)];
            ctx.r->add_char(bounds.x + 10.0f, rb, collapsed_[static_cast<size_t>(row.file)] ? 0xEAB6 : 0xEAB4,
                            th.fg_muted, *ctx.font);
            float ex = ctx.r->add_text(bounds.x + 14.0f + 2.0f * cw, rb, f.filename().string(), th.fg, *ctx.font);
            std::string rel = root_.empty() ? f.parent_path().string()
                                            : f.parent_path().lexically_relative(root_).string();
            if (rel == ".") rel.clear();
            ctx.r->push_clip({bounds.x, ry, bounds.w - 40.0f, row_h_});
            ctx.r->set_fade(bounds.x + bounds.w - 70.0f, bounds.x + bounds.w - 40.0f);
            ctx.r->add_text(ex + cw, rb, rel, th.fg_muted, *ctx.font);
            ctx.r->set_fade(0, 0);
            ctx.r->pop_clip();
            std::string n = std::to_string(file_hits_[static_cast<size_t>(row.file)].size());
            float pw = static_cast<float>(n.size()) * cw + 12.0f;
            float px = bounds.x + bounds.w - 14.0f - pw;
            ctx.r->add_rect({px, ry + 4.0f, pw, row_h_ - 8.0f}, th.surface_hover, (row_h_ - 8.0f) * 0.5f);
            ctx.r->add_text(px + 6.0f, rb, n, th.fg_muted, *ctx.font);
            continue;
        }
        const SearchHit& h = hits_[static_cast<size_t>(row.hit)];
        // Preview: trim indentation, keep the match in view.
        size_t lead = h.text.find_first_not_of(" \t");
        if (lead == std::string::npos) lead = 0;
        size_t start = h.col > lead + 24 ? h.col - 16 : lead;
        std::string pre = h.text.substr(start, h.col - start);
        std::string mid = h.text.substr(h.col, h.len);
        std::string post = h.text.substr(std::min(h.text.size(), h.col + h.len));
        float x = bounds.x + 34.0f;
        ctx.r->push_clip({bounds.x, ry, bounds.w - 12.0f, row_h_});
        ctx.r->set_fade(bounds.x + bounds.w - 40.0f, bounds.x + bounds.w - 12.0f);
        x = ctx.r->add_text(x, rb, pre, th.fg_muted, *ctx.font);
        float mx = x;
        x = ctx.r->add_text(x, rb, mid, th.fg, *ctx.font);
        Color hl = th.warning;
        hl.a = 0.25f;
        ctx.r->add_rect({mx, ry + 2.0f, x - mx, row_h_ - 4.0f}, hl, 2.0f);
        ctx.r->add_text(x, rb, post, th.fg_muted, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    ctx.r->pop_clip();
}

bool SearchPanel::handle_key(const KeyEvent& e) {
    if (!e.pressed || !input_active_) return false;
    bool ctrl = e.mods & KeyMod::Ctrl, alt = e.mods & KeyMod::Alt;
    switch (e.keycode) {
        case SDLK_RETURN:
        case SDLK_RETURN2:
            debounce_ = 0.0f;
            return true;
        case SDLK_BACKSPACE:
            if (ctrl) query_.text.clear();
            else {
                while (!query_.text.empty() && (static_cast<unsigned char>(query_.text.back()) & 0xC0) == 0x80)
                    query_.text.pop_back();
                if (!query_.text.empty()) query_.text.pop_back();
            }
            schedule();
            return true;
        case SDLK_C: if (alt) { query_.case_sensitive = !query_.case_sensitive; schedule(); return true; } break;
        case SDLK_W: if (alt) { query_.whole_word = !query_.whole_word; schedule(); return true; } break;
        case SDLK_ESCAPE: input_active_ = false; return true;
        default: break;
    }
    return !ctrl && !alt;
}

bool SearchPanel::handle_text(const std::string& text) {
    if (!input_active_) return false;
    query_.text += text;
    schedule();
    return true;
}

bool SearchPanel::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        float max_scroll = std::max(0.0f, static_cast<float>(rows_.size()) * row_h_ - (bounds_.h - list_y_ + bounds_.y));
        scroll_y_ = std::clamp(scroll_y_ - e.scroll_y * row_h_ * 3.0f, 0.0f, max_scroll);
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return true;
    if (case_btn_.contains(e.x, e.y)) { query_.case_sensitive = !query_.case_sensitive; schedule(); return true; }
    if (word_btn_.contains(e.x, e.y)) { query_.whole_word = !query_.whole_word; schedule(); return true; }
    if (input_.contains(e.x, e.y)) { input_active_ = true; return true; }
    input_active_ = false;
    if (e.y < list_y_) return true;
    int ri = static_cast<int>((e.y - list_y_ + scroll_y_) / row_h_);
    if (ri < 0 || ri >= static_cast<int>(rows_.size())) return true;
    const Row& row = rows_[static_cast<size_t>(ri)];
    if (row.hit < 0) {
        collapsed_[static_cast<size_t>(row.file)] = !collapsed_[static_cast<size_t>(row.file)];
        rebuild_rows();
    } else if (cb_.open) {
        cb_.open(hits_[static_cast<size_t>(row.hit)]);
    }
    return true;
}

} // namespace oh
