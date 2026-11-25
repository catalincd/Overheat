#include "ui/find_bar.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>

namespace oh {

namespace {
bool is_word(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }
std::string fold(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void FindBar::open(Editor& ed, bool replace) {
    open_ = true;
    input_focus_ = true;
    replace_mode_ = replace;
    field_ = 0;
    // Seed with the selection when it is a single-line selection.
    const Selection& sel = ed.cursors().primary();
    if (!sel.empty() && sel.min().line == sel.max().line) {
        std::string line = ed.buffer().line_text(sel.min().line);
        query_ = line.substr(sel.min().col, sel.max().col - sel.min().col);
    }
    seen_version_ = UINT64_MAX;
    refresh(ed);
    pick_nearest(ed);
}

void FindBar::refresh(Editor& ed) {
    const TextBuffer& buf = ed.buffer();
    if (buf.version() == seen_version_ && query_ == seen_query_ && case_sensitive_ == seen_case_ &&
        whole_word_ == seen_word_)
        return;
    seen_version_ = buf.version();
    seen_query_ = query_;
    seen_case_ = case_sensitive_;
    seen_word_ = whole_word_;
    matches_.clear();
    if (query_.empty()) { current_ = -1; return; }
    std::string text = buf.text();
    std::string hay = case_sensitive_ ? text : fold(text);
    std::string needle = case_sensitive_ ? query_ : fold(query_);
    constexpr size_t MAX_MATCHES = 200000;
    for (size_t pos = hay.find(needle); pos != std::string::npos && matches_.size() < MAX_MATCHES;
         pos = hay.find(needle, pos + std::max<size_t>(1, needle.size()))) {
        if (whole_word_) {
            bool left = pos == 0 || !is_word(static_cast<unsigned char>(text[pos - 1]));
            size_t end = pos + needle.size();
            bool right = end >= text.size() || !is_word(static_cast<unsigned char>(text[end]));
            if (!left || !right) continue;
        }
        matches_.push_back({pos, needle.size()});
    }
    if (current_ >= static_cast<int>(matches_.size())) current_ = matches_.empty() ? -1 : 0;
}

void FindBar::pick_nearest(Editor& ed) {
    if (matches_.empty()) { current_ = -1; return; }
    Position p = ed.cursors().primary().min();
    size_t off = ed.buffer().offset_of(p.line, p.col);
    auto it = std::lower_bound(matches_.begin(), matches_.end(), off,
                               [](const Match& m, size_t o) { return m.start < o; });
    current_ = it == matches_.end() ? 0 : static_cast<int>(it - matches_.begin());
    select_current(ed);
}

void FindBar::select_current(Editor& ed) {
    if (current_ < 0 || current_ >= static_cast<int>(matches_.size())) return;
    const Match& m = matches_[static_cast<size_t>(current_)];
    Position a = ed.buffer().position_of(m.start);
    Position b = ed.buffer().position_of(m.start + m.len);
    ed.cursors().set_primary(a);
    ed.cursors().extend_primary(b);
}

void FindBar::next(Editor& ed, int dir) {
    refresh(ed);
    if (matches_.empty()) return;
    int n = static_cast<int>(matches_.size());
    current_ = current_ < 0 ? 0 : ((current_ + dir) % n + n) % n;
    select_current(ed);
}

void FindBar::replace_one(Editor& ed) {
    refresh(ed);
    if (current_ < 0) return;
    select_current(ed);
    ed.insert_text(replacement_);  // replaces the selection, one undo step
    refresh(ed);
    pick_nearest(ed);
}

void FindBar::replace_all(Editor& ed) {
    refresh(ed);
    if (matches_.empty()) return;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (auto& m : matches_) ranges.push_back({m.start, m.len});
    ed.replace_ranges(ranges, replacement_);
    refresh(ed);
}

Rect FindBar::render(RenderContext& ctx, Rect er) {
    if (!open_) return {};
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    float row_h = lh + 12.0f;
    float w = std::min(560.0f, er.w - 24.0f);
    float h = 8.0f + row_h + (replace_mode_ ? row_h + 6.0f : 0.0f) + 8.0f;
    rect_ = {er.x + er.w - w - 16.0f, er.y + 6.0f, w, h};
    Color sh = th.shadow;
    sh.a *= 0.5f;
    ctx.r->add_rect({rect_.x - 2, rect_.y + 2, rect_.w + 4, rect_.h + 4}, sh, 9.0f);
    ctx.r->add_rect(rect_, th.surface, 8.0f);

    auto base = [&](float y) { return y + (row_h - lh) * 0.5f + ctx.font->ascender(); };
    float controls_w = 26.0f * cw;
    float box_w = rect_.w - 16.0f - controls_w;
    find_box_ = {rect_.x + 8.0f, rect_.y + 8.0f, box_w, row_h};
    bool f0 = input_focus_ && field_ == 0;
    ctx.r->add_rect(find_box_, th.input_bg, 5.0f, f0 ? 1.0f : 0.0f, th.accent);
    ctx.r->push_clip(find_box_.inset(3.0f));
    if (query_.empty()) ctx.r->add_text(find_box_.x + 8.0f, base(find_box_.y), "Find", th.fg_muted, *ctx.font);
    float caret = ctx.r->add_text(find_box_.x + 8.0f, base(find_box_.y), query_, th.fg, *ctx.font);
    if (f0) ctx.r->add_rect({caret, find_box_.y + 6.0f, 2.0f, row_h - 12.0f}, th.accent);
    ctx.r->pop_clip();

    // Toggles and navigation to the right of the box.
    float x = find_box_.x + find_box_.w + 8.0f;
    auto toggle = [&](Rect& r, const char* label, bool on) {
        r = {x, find_box_.y + 3.0f, 3.0f * cw, row_h - 6.0f};
        if (on) ctx.r->add_rect(r, th.selection, 4.0f);
        ctx.r->add_text(r.x + 0.5f * cw, base(find_box_.y), label, on ? th.fg : th.fg_muted, *ctx.font);
        x += r.w + 4.0f;
    };
    toggle(case_btn_, "Aa", case_sensitive_);
    toggle(word_btn_, "ab", whole_word_);
    std::string count = query_.empty() ? "" : matches_.empty() ? "No results"
                      : std::to_string(current_ + 1) + " of " + std::to_string(matches_.size());
    Color count_c = (!query_.empty() && matches_.empty()) ? th.error : th.fg_muted;
    ctx.r->add_text(x + 4.0f, base(find_box_.y), count, count_c, *ctx.font);
    float bx = rect_.x + rect_.w - 8.0f - 3.0f * (2.0f * cw + 4.0f);
    auto button = [&](Rect& r, uint32_t glyph) {
        r = {bx, find_box_.y + 3.0f, 2.0f * cw, row_h - 6.0f};
        ctx.r->add_char(r.x + 0.5f * cw, base(find_box_.y), glyph, th.fg_muted, *ctx.font);
        bx += r.w + 4.0f;
    };
    button(prev_btn_, 0xEAA1);  // arrow-up
    button(next_btn_, 0xEA9A);  // arrow-down
    button(close_btn_, 0xEA76);

    if (replace_mode_) {
        repl_box_ = {find_box_.x, find_box_.y + row_h + 6.0f, box_w, row_h};
        bool f1 = input_focus_ && field_ == 1;
        ctx.r->add_rect(repl_box_, th.input_bg, 5.0f, f1 ? 1.0f : 0.0f, th.accent);
        ctx.r->push_clip(repl_box_.inset(3.0f));
        if (replacement_.empty()) ctx.r->add_text(repl_box_.x + 8.0f, base(repl_box_.y), "Replace", th.fg_muted, *ctx.font);
        float c2 = ctx.r->add_text(repl_box_.x + 8.0f, base(repl_box_.y), replacement_, th.fg, *ctx.font);
        if (f1) ctx.r->add_rect({c2, repl_box_.y + 6.0f, 2.0f, row_h - 12.0f}, th.accent);
        ctx.r->pop_clip();
        ctx.r->add_text(repl_box_.x + repl_box_.w + 12.0f, base(repl_box_.y), "Enter / Ctrl+Alt+Enter all",
                        th.fg_muted, *ctx.font);
    } else {
        repl_box_ = {};
    }
    return rect_;
}

bool FindBar::handle_key(Editor& ed, const KeyEvent& e) {
    if (!open_ || !e.pressed) return false;
    bool shift = e.mods & KeyMod::Shift, ctrl = e.mods & KeyMod::Ctrl, alt = e.mods & KeyMod::Alt;
    if (e.keycode == SDLK_F3) { next(ed, shift ? -1 : 1); return true; }
    if (!input_focus_) return false;
    std::string& field = field_ == 0 ? query_ : replacement_;
    switch (e.keycode) {
        case SDLK_ESCAPE: close(); return true;
        case SDLK_RETURN:
        case SDLK_RETURN2:
            if (field_ == 1 && ctrl && alt) replace_all(ed);
            else if (field_ == 1) replace_one(ed);
            else next(ed, shift ? -1 : 1);
            return true;
        case SDLK_TAB:
            if (replace_mode_) field_ = 1 - field_;
            return true;
        case SDLK_BACKSPACE:
            if (ctrl) field.clear();
            else {
                while (!field.empty() && (static_cast<unsigned char>(field.back()) & 0xC0) == 0x80) field.pop_back();
                if (!field.empty()) field.pop_back();
            }
            if (field_ == 0) { refresh(ed); pick_nearest(ed); }
            return true;
        case SDLK_C: if (alt) { case_sensitive_ = !case_sensitive_; refresh(ed); return true; } break;
        case SDLK_W: if (alt) { whole_word_ = !whole_word_; refresh(ed); return true; } break;
        case SDLK_A: if (ctrl) return true; break;  // keep select-all from reaching the editor
        default: break;
    }
    // Swallow plain keys (their text arrives via handle_text); let Ctrl/Alt chords through.
    return !ctrl && !alt;
}

bool FindBar::handle_text(Editor& ed, const std::string& text) {
    if (!has_input_focus()) return false;
    if (field_ == 0) {
        query_ += text;
        refresh(ed);
        pick_nearest(ed);
    } else {
        replacement_ += text;
    }
    return true;
}

bool FindBar::handle_mouse(Editor& ed, const MouseEvent& e) {
    if (!open_ || !rect_.contains(e.x, e.y)) return false;
    if (e.button != 1 || !e.pressed || e.motion) return true;
    input_focus_ = true;
    if (case_btn_.contains(e.x, e.y)) { case_sensitive_ = !case_sensitive_; refresh(ed); }
    else if (word_btn_.contains(e.x, e.y)) { whole_word_ = !whole_word_; refresh(ed); }
    else if (prev_btn_.contains(e.x, e.y)) next(ed, -1);
    else if (next_btn_.contains(e.x, e.y)) next(ed, 1);
    else if (close_btn_.contains(e.x, e.y)) close();
    else if (repl_box_.contains(e.x, e.y)) field_ = 1;
    else if (find_box_.contains(e.x, e.y)) field_ = 0;
    return true;
}

} // namespace oh
