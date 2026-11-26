#include "ui/completion_popup.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace oh {

namespace {
uint32_t kind_icon(int kind) {
    switch (kind) {  // LSP CompletionItemKind -> codicon
        case 2: case 3: case 4: return 0xEA8C;   // method/function/constructor: symbol-method
        case 5: case 10: return 0xEB5F;          // field/property: symbol-field
        case 6: return 0xEA88;                   // variable
        case 7: case 22: return 0xEB5B;          // class/struct
        case 8: return 0xEB61;                   // interface
        case 9: return 0xEA8B;                   // module/namespace
        case 13: case 20: return 0xEA95;         // enum / enum member
        case 14: return 0xEB62;                  // keyword
        case 21: return 0xEB5D;                  // constant
        default: return 0xEA93;                  // symbol-misc
    }
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool subsequence(const std::string& needle, const std::string& hay) {
    size_t i = 0;
    for (char c : hay)
        if (i < needle.size() && c == needle[i]) i++;
    return i == needle.size();
}
} // namespace

void CompletionPopup::show(std::vector<LSPCompletion> items, Position anchor) {
    all_ = std::move(items);
    anchor_ = anchor;
    selected_ = first_ = 0;
    visible_ = !all_.empty();
}

void CompletionPopup::refilter(const Editor& ed) {
    if (!visible_) return;
    Position cur = ed.cursors().primary_pos();
    if (cur.line != anchor_.line || cur.col < anchor_.col || ed.cursors().selections().size() != 1) {
        visible_ = false;
        return;
    }
    std::string line = ed.buffer().line_text(cur.line);
    std::string prefix = lower(line.substr(anchor_.col, cur.col - anchor_.col));
    for (unsigned char c : prefix)
        if (!(std::isalnum(c) || c == '_' || c >= 0x80)) { visible_ = false; return; }
    std::vector<std::pair<int, int>> scored;  // (rank, index)
    for (size_t i = 0; i < all_.size(); i++) {
        std::string label = lower(all_[i].insert_text.empty() ? all_[i].label : all_[i].insert_text);
        // Prefix matches first; then fuzzy matches that start on the same letter.
        if (prefix.empty() || label.rfind(prefix, 0) == 0) scored.push_back({0, static_cast<int>(i)});
        else if (!label.empty() && label[0] == prefix[0] && subsequence(prefix, label))
            scored.push_back({1, static_cast<int>(i)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });
    shown_.clear();
    for (auto& [rank, idx] : scored) shown_.push_back(idx);
    if (shown_.empty()) { visible_ = false; return; }
    selected_ = std::min(selected_, static_cast<int>(shown_.size()) - 1);
}

void CompletionPopup::accept(Editor& ed) {
    if (shown_.empty()) return;
    const LSPCompletion& c = all_[static_cast<size_t>(shown_[static_cast<size_t>(selected_)])];
    std::string text = c.insert_text.empty() ? c.label : c.insert_text;
    Position cur = ed.cursors().primary_pos();
    ed.cursors().set_primary(anchor_);
    ed.cursors().extend_primary(cur);
    ed.insert_text(text);  // replaces the typed prefix, one undo step
    visible_ = false;
}

bool CompletionPopup::handle_key(Editor& ed, const KeyEvent& e) {
    if (!visible_ || !e.pressed) return false;
    int n = static_cast<int>(shown_.size());
    switch (e.keycode) {
        case SDLK_UP: selected_ = (selected_ - 1 + n) % n; return true;
        case SDLK_DOWN: selected_ = (selected_ + 1) % n; return true;
        case SDLK_PAGEUP: selected_ = std::max(0, selected_ - MAX_ROWS); return true;
        case SDLK_PAGEDOWN: selected_ = std::min(n - 1, selected_ + MAX_ROWS); return true;
        case SDLK_RETURN: case SDLK_RETURN2: case SDLK_TAB: accept(ed); return true;
        case SDLK_ESCAPE: visible_ = false; return true;
        default: return false;
    }
}

void CompletionPopup::render(RenderContext& ctx, float x, float line_top, float line_h, Rect clip) {
    if (!visible_ || shown_.empty()) return;
    ctx.r->set_popup(true);  // stays opaque on a translucent window
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width();
    row_h_ = std::round(ctx.font->line_height() + 4.0f);
    int rows = std::min(MAX_ROWS, static_cast<int>(shown_.size()));
    if (selected_ < first_) first_ = selected_;
    if (selected_ >= first_ + rows) first_ = selected_ - rows + 1;
    float w = std::min(clip.w - 16.0f, 520.0f);
    float h = rows * row_h_ + 8.0f;
    float y = line_top + line_h + 2.0f;
    if (y + h > clip.y + clip.h && line_top - h - 2.0f > clip.y) y = line_top - h - 2.0f;  // flip above
    x = std::clamp(x - cw * 2.0f, clip.x + 4.0f, clip.x + clip.w - w - 4.0f);
    rect_ = {x, y, w, h};
    Color sh = th.shadow;
    sh.a *= 0.5f;
    ctx.r->add_rect({x - 2, y + 2, w + 4, h + 4}, sh, 8.0f);
    ctx.r->add_rect(rect_, th.surface, 6.0f);
    for (int vi = 0; vi < rows; vi++) {
        int i = first_ + vi;
        const LSPCompletion& c = all_[static_cast<size_t>(shown_[static_cast<size_t>(i)])];
        float ry = y + 4.0f + vi * row_h_;
        float base = ry + (row_h_ - ctx.font->line_height()) * 0.5f + ctx.font->ascender();
        if (i == selected_) ctx.r->add_rect({x + 4.0f, ry, w - 8.0f, row_h_}, th.surface_hover, 4.0f);
        ctx.r->add_char(x + 10.0f, base, kind_icon(c.kind), th.accent, *ctx.font);
        ctx.r->push_clip({x, ry, w - 8.0f, row_h_});
        ctx.r->set_fade(x + w - 40.0f, x + w - 10.0f);
        float end = ctx.r->add_text(x + 14.0f + 2.0f * cw, base, c.label, th.fg, *ctx.font);
        if (!c.detail.empty()) ctx.r->add_text(end + 2.0f * cw, base, c.detail, th.fg_muted, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    ctx.r->set_popup(false);
}

bool CompletionPopup::handle_mouse(Editor& ed, const MouseEvent& e) {
    if (!visible_ || !rect_.contains(e.x, e.y)) return false;
    if (e.button == 1 && e.pressed && !e.motion) {
        int i = first_ + static_cast<int>((e.y - rect_.y - 4.0f) / row_h_);
        if (i >= 0 && i < static_cast<int>(shown_.size())) {
            selected_ = i;
            accept(ed);
        }
    }
    return true;
}

} // namespace oh
