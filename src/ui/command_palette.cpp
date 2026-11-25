#include "ui/command_palette.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace oh {

void CommandPalette::show(Mode mode) {
    if (!visible_) appear_.snap(0.0f);
    visible_ = true;
    mode_ = mode;
    query_.clear();
    selected_index_ = 0;
    update_filter();
}

void CommandPalette::pick(PickOptions opts) {
    pick_ = std::move(opts);
    if (!visible_) appear_.snap(0.0f);
    visible_ = true;
    mode_ = Mode::Pick;
    query_.clear();
    last_previewed_ = -1;
    update_filter();
    select(std::clamp(pick_.initial, 0, std::max(0, static_cast<int>(filtered_.size()) - 1)));
}

void CommandPalette::select(int index) {
    selected_index_ = filtered_.empty() ? 0 : std::clamp(index, 0, static_cast<int>(filtered_.size()) - 1);
    if (mode_ == Mode::Pick && pick_.on_preview && !filtered_.empty()) {
        int item = filtered_[static_cast<size_t>(selected_index_)].index;
        if (item != last_previewed_) {
            last_previewed_ = item;
            pick_.on_preview(item);
        }
    }
}

void CommandPalette::hide() {
    visible_ = false;
    query_.clear();
    filtered_.clear();
}

int CommandPalette::fuzzy_score(const std::string& pattern, const std::string& text) const {
    if (pattern.empty()) return 1; // Empty pattern matches everything

    size_t pi = 0;
    size_t ti = 0;
    int score = 0;
    int consecutive = 0;

    while (pi < pattern.size() && ti < text.size()) {
        char pc = static_cast<char>(std::tolower(static_cast<unsigned char>(pattern[pi])));
        char tc = static_cast<char>(std::tolower(static_cast<unsigned char>(text[ti])));

        if (pc == tc) {
            score += 10;

            // Bonus for consecutive matches
            consecutive++;
            if (consecutive > 1) score += consecutive * 5;

            // Bonus for matching at start of text
            if (ti == 0) score += 20;

            // Bonus for matching after separator (/, _, -, space, case change)
            if (ti > 0) {
                char prev = text[ti - 1];
                if (prev == '/' || prev == '\\' || prev == '_' || prev == '-' ||
                    prev == ' ' || prev == '.') {
                    score += 15;
                }
                // CamelCase boundary
                if (std::islower(static_cast<unsigned char>(prev)) &&
                    std::isupper(static_cast<unsigned char>(text[ti]))) {
                    score += 10;
                }
            }

            pi++;
        } else {
            consecutive = 0;
            // Penalty for skipped characters (mild)
            score -= 1;
        }
        ti++;
    }

    // All pattern characters must match
    if (pi < pattern.size()) return -1;

    return score;
}

void CommandPalette::update_filter() {
    filtered_.clear();
    first_visible_ = 0;

    if (mode_ == Mode::Pick) {
        for (size_t i = 0; i < pick_.items.size(); i++) {
            int score = fuzzy_score(query_, pick_.items[i]);
            if (score >= 0) filtered_.push_back({static_cast<int>(i), query_.empty() ? 0 : score, pick_.items[i]});
        }
        if (!query_.empty())
            std::stable_sort(filtered_.begin(), filtered_.end(),
                             [](const MatchResult& a, const MatchResult& b) { return a.score > b.score; });
        return;
    }

    if (mode_ == Mode::Commands) {
        for (size_t i = 0; i < commands_.size(); i++) {
            int score = fuzzy_score(query_, commands_[i].label);
            if (score >= 0)
                filtered_.push_back({static_cast<int>(i), score, commands_[i].label,
                                     commands_[i].keybinding, true});
        }
    } else if (mode_ == Mode::Files) {
        for (size_t i = 0; i < file_list_.size(); i++) {
            std::string filename = file_list_[i].filename().string();
            std::string full_path = root_.empty()
                ? file_list_[i].string()
                : file_list_[i].lexically_relative(root_).string();

            // Score against filename primarily, with path as tiebreaker
            int score = fuzzy_score(query_, filename);
            if (score < 0) {
                score = fuzzy_score(query_, full_path);
            }
            if (score >= 0) {
                std::string dir = fs::path(full_path).parent_path().string();
                filtered_.push_back({static_cast<int>(i), score, filename, dir, false});
            }
        }
    }

    // Sort by score descending (stable keeps the natural order among ties)
    std::stable_sort(filtered_.begin(), filtered_.end(),
              [](const MatchResult& a, const MatchResult& b) {
                  return a.score > b.score;
              });

    // Clamp selected index
    if (selected_index_ >= static_cast<int>(filtered_.size())) {
        selected_index_ = std::max(0, static_cast<int>(filtered_.size()) - 1);
    }
}

void CommandPalette::render(RenderContext& ctx, Rect bounds) {
    if (!visible_) return;
    bounds_ = bounds;
    const Theme& th = *ctx.theme;

    float char_w = ctx.font->char_width();
    float line_h = ctx.font->line_height();
    float row_h = line_h + 8.0f;
    float pad = 8.0f;

    float t = appear_.follow(1.0f, anim::FAST, 0.01f);
    ctx.r->set_alpha(t);
    float palette_w = std::min(640.0f, bounds.w - 32.0f);
    float palette_x = std::floor(bounds.x + (bounds.w - palette_w) * 0.5f);
    float palette_y = bounds.y + std::min(48.0f, bounds.h * 0.08f) - std::round((1.0f - t) * 10.0f);
    float input_h = line_h + 16.0f;
    int max_visible = std::max(1, std::min(12, static_cast<int>((bounds.h - palette_y - input_h - 3 * pad) / row_h)));
    int count = static_cast<int>(filtered_.size());
    int visible = std::min(count, max_visible);
    if (selected_index_ < first_visible_) first_visible_ = selected_index_;
    if (selected_index_ >= first_visible_ + max_visible) first_visible_ = selected_index_ - max_visible + 1;
    float list_h = visible > 0 ? visible * row_h + pad : (query_.empty() ? 0.0f : row_h + pad);
    float palette_h = pad + input_h + list_h + pad * 0.5f;
    Rect box{palette_x, palette_y, palette_w, palette_h};

    // Soft shadow: a few widening, fading rounded rects under the surface.
    for (int i = 3; i >= 1; i--) {
        Color sh = th.shadow;
        sh.a *= 0.22f;
        float g = 4.0f * i;
        ctx.r->add_rect({box.x - g, box.y - g + 6.0f, box.w + 2 * g, box.h + 2 * g}, sh, 10.0f + g);
    }
    ctx.r->add_rect(box, th.surface, 10.0f);

    Rect input_rect = {palette_x + pad, palette_y + pad, palette_w - 2 * pad, input_h};
    ctx.r->add_rect(input_rect, th.input_bg, 6.0f);

    std::string prefix = mode_ == Mode::Commands ? "> " : mode_ == Mode::Symbols ? "@ " : "";
    float text_x = input_rect.x + 10.0f;
    float baseline = input_rect.y + (input_h - line_h) * 0.5f + ctx.font->ascender();
    std::string shown = prefix + query_;
    if (mode_ == Mode::Pick && pick_.password) {
        shown.clear();
        for (unsigned char c : query_)
            if ((c & 0xC0) != 0x80) shown += "\u2022";
    }
    if (query_.empty() && mode_ == Mode::Pick && !pick_.placeholder.empty()) {
        ctx.r->add_text(text_x, baseline, pick_.placeholder, th.fg_muted, *ctx.font);
    } else if (query_.empty() && mode_ == Mode::Files) {
        ctx.r->add_text(text_x, baseline, "Search files by name", th.fg_muted, *ctx.font);
    }
    float caret_x = ctx.r->add_text(text_x, baseline, shown, th.fg, *ctx.font);
    ctx.r->add_rect({caret_x, input_rect.y + 6.0f, 2.0f, input_h - 12.0f}, th.accent);

    float list_y = input_rect.y + input_h + pad * 0.5f;
    ctx.r->push_clip({palette_x, list_y, palette_w, palette_h - (list_y - palette_y)});
    for (int vi = 0; vi < visible; vi++) {
        int i = first_visible_ + vi;
        auto& m = filtered_[static_cast<size_t>(i)];
        float y = list_y + vi * row_h;
        bool sel = i == selected_index_;
        if (sel) ctx.r->add_rect({palette_x + pad * 0.5f, y, palette_w - pad, row_h}, th.surface_hover, 6.0f);
        if (sel) ctx.r->add_rect({palette_x + pad * 0.5f, y + 6.0f, 2.0f, row_h - 12.0f}, th.accent, 1.0f);
        float by = y + (row_h - line_h) * 0.5f + ctx.font->ascender();
        float right = palette_x + palette_w - pad - 8.0f;
        float detail_w = static_cast<float>(m.detail.size()) * char_w;
        if (m.detail_right && !m.detail.empty()) {
            ctx.r->add_text(right - detail_w, by, m.detail, th.fg_muted, *ctx.font);
            right -= detail_w + 16.0f;
        }
        ctx.r->push_clip({palette_x, y, right - palette_x, row_h});
        ctx.r->set_fade(right - 24.0f, right);
        float end = ctx.r->add_text(palette_x + 18.0f, by, m.display, sel ? th.fg : th.fg, *ctx.font);
        if (!m.detail_right && !m.detail.empty())
            ctx.r->add_text(end + 12.0f, by, m.detail, th.fg_muted, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    if (count == 0 && !query_.empty()) {
        const char* msg = (mode_ == Mode::Pick && pick_.allow_free_text) ? "Press Enter to use this value"
                                                                         : "No matching results";
        ctx.r->add_text(palette_x + 18.0f, list_y + (row_h - line_h) * 0.5f + ctx.font->ascender(),
                        msg, th.fg_muted, *ctx.font);
    }
    ctx.r->pop_clip();
    ctx.r->set_alpha(1.0f);
}

bool CommandPalette::handle_key(const KeyEvent& e) {
    if (!visible_ || !e.pressed) return false;

    switch (e.keycode) {
        case SDLK_ESCAPE: {
            auto cancel = mode_ == Mode::Pick ? pick_.on_cancel : nullptr;
            hide();
            if (cancel) cancel();
            return true;
        }

        case SDLK_UP:
            select(selected_index_ - 1);
            return true;

        case SDLK_DOWN:
            select(selected_index_ + 1);
            return true;

        case SDLK_PAGEUP:
            select(selected_index_ - 10);
            return true;

        case SDLK_PAGEDOWN:
            select(selected_index_ + 10);
            return true;

        case SDLK_RETURN:
        case SDLK_RETURN2: {
            if (mode_ == Mode::Pick) {
                auto cb = pick_.on_pick;
                std::string q = query_;
                int item = filtered_.empty() ? -1 : filtered_[static_cast<size_t>(selected_index_)].index;
                if (item < 0 && !pick_.allow_free_text) return true;
                hide();
                if (cb) cb(item, q);
                return true;
            }
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(filtered_.size())) {
                auto& match = filtered_[static_cast<size_t>(selected_index_)];

                if (mode_ == Mode::Commands) {
                    if (match.index >= 0 &&
                        match.index < static_cast<int>(commands_.size())) {
                        auto fn = commands_[static_cast<size_t>(match.index)].execute;
                        hide();
                        if (fn) fn();
                    }
                } else if (mode_ == Mode::Files) {
                    if (match.index >= 0 &&
                        match.index < static_cast<int>(file_list_.size())) {
                        auto path = file_list_[static_cast<size_t>(match.index)];
                        hide();
                        if (on_file_open_) on_file_open_(path);
                    }
                }
            }
            return true;
        }

        case SDLK_BACKSPACE:
            if (!query_.empty()) {
                if (e.mods & KeyMod::Ctrl) {
                    // Delete whole word
                    while (!query_.empty() && query_.back() == ' ')
                        query_.pop_back();
                    while (!query_.empty() && query_.back() != ' ')
                        query_.pop_back();
                } else {
                    query_.pop_back();
                }
                update_filter();
                select(0);
            }
            return true;

        case SDLK_TAB:
            // Tab cycles through results (same as down)
            select(selected_index_ + ((e.mods & KeyMod::Shift) ? -1 : 1));
            return true;

        default:
            break;
    }

    return false;
}

bool CommandPalette::handle_text(const std::string& text) {
    if (!visible_) return false;

    query_ += text;
    update_filter();
    select(0);
    return true;
}

} // namespace oh
