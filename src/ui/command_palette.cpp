#include "ui/command_palette.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace oh {

void CommandPalette::show(Mode mode) {
    visible_ = true;
    mode_ = mode;
    query_.clear();
    selected_index_ = 0;
    update_filter();
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

    if (mode_ == Mode::Commands) {
        for (size_t i = 0; i < commands_.size(); i++) {
            std::string display = commands_[i].label;
            if (!commands_[i].keybinding.empty()) {
                display += "  [" + commands_[i].keybinding + "]";
            }

            int score = fuzzy_score(query_, commands_[i].label);
            if (score >= 0) {
                filtered_.push_back({static_cast<int>(i), score, std::move(display)});
            }
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
                filtered_.push_back({static_cast<int>(i), score, full_path});
            }
        }
    }

    // Sort by score descending
    std::sort(filtered_.begin(), filtered_.end(),
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

    float char_w = ctx.font->char_width();
    float line_h = ctx.font->line_height();

    // Palette dimensions: centered at top, 50% of width
    float palette_w = std::min(640.0f, bounds.w - 32.0f);
    float palette_x = bounds.x + (bounds.w - palette_w) * 0.5f;
    float palette_y = bounds.y + std::min(40.0f, bounds.h * 0.08f);

    // Max visible results
    constexpr int max_visible = 12;
    int result_count = std::min(static_cast<int>(filtered_.size()), max_visible);
    float input_h = line_h + 12.0f;
    float results_h = static_cast<float>(result_count) * line_h;
    float palette_h = input_h + results_h + 8.0f;

    // Dimmed overlay behind palette
    Color dim = {0.0f, 0.0f, 0.0f, 0.3f};
    ctx.r->add_rect(bounds, dim);

    // Palette background with border
    ctx.r->add_rect({palette_x, palette_y, palette_w, palette_h},
                     ctx.theme->bg, 6.0f, 1.0f, ctx.theme->border);

    // Input field background
    Rect input_rect = {palette_x + 4.0f, palette_y + 4.0f,
                       palette_w - 8.0f, input_h - 4.0f};
    ctx.r->add_rect(input_rect, ctx.theme->tab_active_bg, 4.0f);

    // Prompt prefix based on mode
    std::string prefix;
    switch (mode_) {
        case Mode::Commands: prefix = "> "; break;
        case Mode::Files:    prefix = ""; break;
        case Mode::Symbols:  prefix = "@ "; break;
    }

    // Input text
    float input_text_x = input_rect.x + 8.0f;
    float input_text_y = input_rect.y + ctx.font->ascender() + 4.0f;
    ctx.r->add_text(input_text_x, input_text_y, prefix + query_,
                       ctx.theme->fg, *ctx.font);

    // Cursor in input field
    float cursor_x = input_text_x +
                     static_cast<float>(prefix.size() + query_.size()) * char_w;
    ctx.r->add_rect({cursor_x, input_rect.y + 4.0f, 2.0f, input_h - 12.0f},
                     ctx.theme->cursor_color);

    // Separator line
    float sep_y = palette_y + input_h;
    ctx.r->add_rect({palette_x + 4.0f, sep_y, palette_w - 8.0f, 1.0f},
                     ctx.theme->border);

    // Results
    ctx.r->push_clip({palette_x + 4.0f, sep_y, palette_w - 8.0f, palette_h - (sep_y - palette_y)});
    ctx.r->set_fade(palette_x + palette_w - 40.0f, palette_x + palette_w - 8.0f);
    float result_y = sep_y + 4.0f;
    for (int i = 0; i < result_count; i++) {
        auto& match = filtered_[static_cast<size_t>(i)];

        // Highlight selected
        if (i == selected_index_) {
            ctx.r->add_rect({palette_x + 4.0f, result_y,
                              palette_w - 8.0f, line_h},
                             ctx.theme->selection, 3.0f);
        }

        Color text_color = (i == selected_index_) ? ctx.theme->fg : ctx.theme->comment;
        ctx.r->add_text(palette_x + 16.0f,
                           result_y + ctx.font->ascender(),
                           match.display, text_color, *ctx.font);

        result_y += line_h;
    }
    ctx.r->set_fade(0, 0);
    ctx.r->pop_clip();

    // Show "no results" message if applicable
    if (filtered_.empty() && !query_.empty()) {
        ctx.r->add_text(palette_x + 16.0f,
                           sep_y + 4.0f + ctx.font->ascender(),
                           "No results found", ctx.theme->comment, *ctx.font);
    }
}

bool CommandPalette::handle_key(const KeyEvent& e) {
    if (!visible_ || !e.pressed) return false;

    switch (e.keycode) {
        case SDLK_ESCAPE:
            hide();
            return true;

        case SDLK_UP:
            if (selected_index_ > 0) selected_index_--;
            return true;

        case SDLK_DOWN:
            if (selected_index_ < static_cast<int>(filtered_.size()) - 1)
                selected_index_++;
            return true;

        case SDLK_RETURN:
        case SDLK_RETURN2: {
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
                selected_index_ = 0;
                update_filter();
            }
            return true;

        case SDLK_TAB:
            // Tab cycles through results (same as down)
            if (e.mods & KeyMod::Shift) {
                if (selected_index_ > 0) selected_index_--;
            } else {
                if (selected_index_ < static_cast<int>(filtered_.size()) - 1)
                    selected_index_++;
            }
            return true;

        default:
            break;
    }

    return false;
}

bool CommandPalette::handle_text(const std::string& text) {
    if (!visible_) return false;

    query_ += text;
    selected_index_ = 0;
    update_filter();
    return true;
}

} // namespace oh
