#include "ui/peek_view.h"
#include "ui/file_icons.h"
#include "editor/utf8.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void PeekView::open(std::string title, std::vector<Item> items, size_t anchor_line, const Editor* host, Loader loader,
                    OpenFn on_open, std::function<void()> on_close) {
    title_ = std::move(title);
    items_ = std::move(items);
    anchor_line_ = anchor_line;
    host_ = host;
    loader_ = std::move(loader);
    on_open_ = std::move(on_open);
    on_close_ = std::move(on_close);
    editors_.clear();
    preview_.reset();
    preview_view_.set_editor(nullptr);
    preview_view_.set_minimap(false);
    rows_.clear();
    for (size_t i = 0; i < items_.size(); i++) {
        if (i == 0 || items_[i].path != items_[i - 1].path) {
            size_t n = 1;
            while (i + n < items_.size() && items_[i + n].path == items_[i].path) n++;
            rows_.push_back({true, i, n});
        }
        rows_.push_back({false, i, 0});
    }
    visible_ = !items_.empty();
    first_row_ = 0;
    select(0);
    first_row_ = 0;  // the list is not measured yet; render() keeps the selection in view
}

void PeekView::close() {
    if (!visible_) return;
    visible_ = false;
    editors_.clear();
    preview_.reset();
    preview_view_.set_editor(nullptr);
    auto cb = on_close_;
    if (cb) cb();
}

std::string PeekView::folder_of(const fs::path& p) const {
    fs::path dir = p.parent_path();
    if (root_.empty()) return dir.string();
    fs::path rel = dir.lexically_relative(root_);
    std::string r = rel.string();
    if (r.empty() || r.rfind("..", 0) == 0) return dir.string();
    return r == "." ? std::string() : r;
}

const PeekView::Item* PeekView::selected_item() const {
    if (!visible_ || selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return nullptr;
    return &items_[static_cast<size_t>(selected_)];
}

int PeekView::row_of_item(int item) const {
    for (size_t r = 0; r < rows_.size(); r++)
        if (!rows_[r].header && static_cast<int>(rows_[r].item) == item) return static_cast<int>(r);
    return 0;
}

void PeekView::select(int item) {
    if (items_.empty()) return;
    selected_ = std::clamp(item, 0, static_cast<int>(items_.size()) - 1);
    const Item& it = items_[static_cast<size_t>(selected_)];
    auto& ed = editors_[it.path.string()];
    if (!ed && loader_) ed = loader_(it.path);
    preview_ = ed;
    preview_view_.set_editor(preview_);
    if (preview_) {
        size_t lines = preview_->buffer().line_count();
        Position a{std::min(it.start.line, lines - 1), it.start.col};
        Position b{std::min(it.end.line, lines - 1), it.end.col};
        preview_->cursors().set_primary(a);
        preview_->cursors().extend_primary(b);
        preview_->set_jump_line(a.line);
        reveal_pending_ = true;
    }
    int row = row_of_item(selected_);
    if (row < first_row_ + 1) first_row_ = std::max(0, row - 1);  // keep the file header in view
    if (row >= first_row_ + visible_rows_) first_row_ = row - visible_rows_ + 1;
}

bool PeekView::handle_key(const KeyEvent& e) {
    if (!visible_ || !e.pressed) return false;
    int n = static_cast<int>(items_.size());
    switch (e.keycode) {
        case SDLK_ESCAPE: close(); return true;
        case SDLK_UP: select((selected_ - 1 + n) % n); return true;
        case SDLK_DOWN: select((selected_ + 1) % n); return true;
        case SDLK_PAGEUP: select(selected_ - std::max(1, visible_rows_ - 1)); return true;
        case SDLK_PAGEDOWN: select(selected_ + std::max(1, visible_rows_ - 1)); return true;
        case SDLK_HOME: if (e.mods & KeyMod::Ctrl) { select(0); return true; } return false;
        case SDLK_END: if (e.mods & KeyMod::Ctrl) { select(n - 1); return true; } return false;
        case SDLK_RETURN:
        case SDLK_RETURN2:
        case SDLK_KP_ENTER: {
            const Item* it = selected_item();
            if (!it) return true;
            Item copy = *it;
            auto open = on_open_;
            close();
            if (open) open(copy);
            return true;
        }
        default: return false;
    }
}

bool PeekView::handle_mouse(const MouseEvent& e) {
    if (!visible_ || !rect_.contains(e.x, e.y)) return false;
    if (close_rect_.contains(e.x, e.y)) {
        if (e.pressed && !e.motion && e.button == 1) close();
        return true;
    }
    if (list_rect_.contains(e.x, e.y)) {
        if (e.scroll_y != 0) {
            int max_first = std::max(0, static_cast<int>(rows_.size()) - visible_rows_);
            first_row_ = std::clamp(first_row_ - static_cast<int>(e.scroll_y * 3), 0, max_first);
            return true;
        }
        if (e.pressed && !e.motion && e.button == 1) {
            int r = first_row_ + static_cast<int>((e.y - list_rect_.y - 4.0f) / row_h_);
            if (r >= 0 && r < static_cast<int>(rows_.size())) {
                int item = static_cast<int>(rows_[static_cast<size_t>(r)].item);
                select(item);
                if (e.double_click && !rows_[static_cast<size_t>(r)].header) {
                    Item copy = items_[static_cast<size_t>(item)];
                    auto open = on_open_;
                    close();
                    if (open) open(copy);
                }
            }
        }
        return true;
    }
    if (preview_rect_.contains(e.x, e.y) && preview_) {
        if (e.pressed && !e.motion && e.button == 1 && e.double_click) {  // open where clicked
            preview_view_.handle_mouse(e);
            Item at = items_[static_cast<size_t>(selected_)];
            at.start = at.end = preview_->cursors().primary_pos();
            auto open = on_open_;
            close();
            if (open) open(at);
            return true;
        }
        preview_view_.handle_mouse(e);
    }
    return true;
}

void PeekView::render(RenderContext& ctx, Rect zone) {
    if (!visible_) return;
    rect_ = zone;
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    row_h_ = std::round(lh + 4.0f);
    float header_h = std::round(lh + 8.0f);
    ctx.r->push_clip(zone);
    ctx.r->add_rect(zone, th.bg);
    // Accent rules above and below, like VS Code's peek frame.
    ctx.r->add_rect({zone.x, zone.y, zone.w, 2.0f}, th.accent);
    ctx.r->add_rect({zone.x, zone.y + zone.h - 2.0f, zone.w, 2.0f}, th.accent);

    // Header: file of the selected item, its folder, and the title with the count.
    Rect head{zone.x, zone.y + 2.0f, zone.w, header_h};
    ctx.r->add_rect(head, th.surface);
    float base = head.y + (header_h - lh) * 0.5f + ctx.font->ascender();
    const Item* sel = selected_item();
    float x = head.x + 10.0f;
    if (sel) {
        std::string name = sel->path.filename().string();
        x = file_icons::draw(*ctx.r, *ctx.font, th, x, base, name, false);
        x = ctx.r->add_text(x, base, name, th.fg, *ctx.font) + cw;
        x = ctx.r->add_text(x, base, folder_of(sel->path), th.fg_muted, *ctx.font) + 2 * cw;
    }
    std::string right = title_ + " (" + std::to_string(items_.size()) + ")";
    close_rect_ = {head.x + head.w - header_h, head.y, header_h, header_h};
    float rw = static_cast<float>(utf8::display_col(right, right.size())) * cw;
    ctx.r->add_text(std::max(x, close_rect_.x - rw - 8.0f), base, right, th.fg_muted, *ctx.font);
    bool hot = close_rect_.contains(ctx.mouse_x, ctx.mouse_y);
    if (hot) ctx.r->add_rect({close_rect_.x + 3, close_rect_.y + 3, close_rect_.w - 6, close_rect_.h - 6}, th.surface_hover, 4.0f);
    ctx.r->add_char(close_rect_.x + (close_rect_.w - cw) * 0.5f, base, 0xEA76, th.fg, *ctx.font);

    // Body: preview (left) and the list (right).
    float body_y = head.y + header_h;
    float body_h = zone.y + zone.h - 2.0f - body_y;
    float list_w = std::clamp(zone.w * 0.32f, std::min(220.0f, zone.w * 0.5f), 420.0f);
    preview_rect_ = {zone.x, body_y, zone.w - list_w, body_h};
    list_rect_ = {zone.x + zone.w - list_w, body_y, list_w, body_h};
    if (preview_) {
        if (reveal_pending_) {
            float target = std::max(0.0f, (static_cast<float>(preview_->cursors().primary_pos().line) - 3.0f) * lh);
            preview_->set_scroll(0, target);
            reveal_pending_ = false;
        }
        ctx.r->push_clip(preview_rect_);
        preview_view_.render(ctx, preview_rect_);
        ctx.r->pop_clip();
    }
    ctx.r->add_rect(list_rect_, th.sidebar_bg);
    ctx.r->add_rect({list_rect_.x, list_rect_.y, 1.0f, list_rect_.h}, th.border);
    visible_rows_ = std::max(1, static_cast<int>((list_rect_.h - 8.0f) / row_h_));
    int max_first = std::max(0, static_cast<int>(rows_.size()) - visible_rows_);
    if (int row = row_of_item(selected_); row >= first_row_ + visible_rows_) first_row_ = row - visible_rows_ + 1;
    first_row_ = std::clamp(first_row_, 0, max_first);
    ctx.r->push_clip(list_rect_);
    for (int vi = 0; vi < visible_rows_ && first_row_ + vi < static_cast<int>(rows_.size()); vi++) {
        const Row& row = rows_[static_cast<size_t>(first_row_ + vi)];
        const Item& it = items_[row.item];
        float ry = list_rect_.y + 4.0f + vi * row_h_;
        float rb = ry + (row_h_ - lh) * 0.5f + ctx.font->ascender();
        float rx = list_rect_.x + 8.0f;
        float rr = list_rect_.x + list_rect_.w - 8.0f;
        if (row.header) {
            std::string name = it.path.filename().string();
            rx = file_icons::draw(*ctx.r, *ctx.font, th, rx, rb, name, false);
            rx = ctx.r->add_text(rx, rb, name, th.fg, *ctx.font) + cw;
            std::string n = std::to_string(row.count);
            float nw = static_cast<float>(n.size()) * cw + 10.0f;
            ctx.r->add_rect({rr - nw, ry + 3.0f, nw, row_h_ - 6.0f}, th.surface_hover, (row_h_ - 6.0f) * 0.5f);
            ctx.r->add_text(rr - nw + 5.0f, rb, n, th.fg, *ctx.font);
            ctx.r->push_clip({rx, ry, std::max(0.0f, rr - nw - 6.0f - rx), row_h_});
            ctx.r->add_text(rx, rb, folder_of(it.path), th.fg_muted, *ctx.font);
            ctx.r->pop_clip();
            continue;
        }
        bool is_sel = static_cast<int>(row.item) == selected_;
        if (is_sel) {
            ctx.r->add_rect({list_rect_.x + 4.0f, ry, list_rect_.w - 8.0f, row_h_}, th.tree_selection, 4.0f);
        }
        rx += 2.0f * cw;
        // Trimmed line text with the match highlighted.
        const std::string& lt = it.line_text;
        size_t lead = std::min(lt.find_first_not_of(" \t"), lt.size());
        std::string shown = lt.substr(lead);
        if (it.start.col >= lead && it.start.line == it.end.line && it.end.col > it.start.col) {
            size_t a = std::min(it.start.col - lead, shown.size()), b = std::min(it.end.col - lead, shown.size());
            float x0 = rx + utf8::display_col(shown, a) * cw, x1 = rx + utf8::display_col(shown, b) * cw;
            ctx.r->add_rect({x0, ry + 2.0f, x1 - x0, row_h_ - 4.0f}, th.find_match, 2.0f);
        }
        std::string ln = std::to_string(it.start.line + 1);
        float lnw = static_cast<float>(ln.size()) * cw;
        ctx.r->push_clip({rx, ry, std::max(0.0f, rr - lnw - 8.0f - rx), row_h_});
        ctx.r->set_fade(rr - lnw - 30.0f, rr - lnw - 8.0f);
        ctx.r->add_text(rx, rb, shown, is_sel ? th.fg : th.fg, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
        ctx.r->add_text(rr - lnw, rb, ln, th.fg_muted, *ctx.font);
    }
    ctx.r->pop_clip();
    ctx.r->pop_clip();
}

} // namespace oh
