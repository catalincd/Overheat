#include "ui/scm_panel.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

namespace {
const char* letter(FileStatus s) {
    switch (s) {
        case FileStatus::Modified: return "M";
        case FileStatus::Added: return "A";
        case FileStatus::Deleted: return "D";
        case FileStatus::Renamed: return "R";
        case FileStatus::Untracked: return "U";
        default: return "";
    }
}
Color status_color(FileStatus s, const Theme& th) {
    switch (s) {
        case FileStatus::Added:
        case FileStatus::Untracked: return th.git_added;
        case FileStatus::Deleted: return th.git_deleted;
        default: return th.git_modified;
    }
}
} // namespace

void ScmPanel::set_state(const fs::path& workdir, const std::string& branch, std::vector<GitStatus> status) {
    workdir_ = workdir;
    branch_ = branch;
    status_ = std::move(status);
    std::erase_if(status_, [](const GitStatus& s) {
        return s.index_status == FileStatus::Unmodified && s.wt_status == FileStatus::Unmodified;
    });
    rebuild_rows();
}

void ScmPanel::rebuild_rows() {
    rows_.clear();
    bool any_staged = std::any_of(status_.begin(), status_.end(),
                                  [](auto& s) { return s.index_status != FileStatus::Unmodified; });
    if (any_staged) {
        rows_.push_back({RowKind::SectionStaged, 0});
        for (size_t i = 0; i < status_.size(); i++)
            if (status_[i].index_status != FileStatus::Unmodified) rows_.push_back({RowKind::Staged, i});
    }
    rows_.push_back({RowKind::SectionChanges, 0});
    for (size_t i = 0; i < status_.size(); i++)
        if (status_[i].wt_status != FileStatus::Unmodified) rows_.push_back({RowKind::Change, i});
}

void ScmPanel::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    hits_.clear();
    const Theme& th = *ctx.theme;
    float cw = ctx.font->char_width(), lh = ctx.font->line_height();
    row_h_ = std::round(lh + 4.0f);
    auto baseline = [&](float y, float h) { return y + (h - lh) * 0.5f + ctx.font->ascender(); };
    ctx.r->add_rect(bounds, th.sidebar_bg);

    float x0 = bounds.x + 12.0f, w = bounds.w - 24.0f;
    float y = bounds.y;
    float hx = ctx.r->add_text(bounds.x + 14.0f, baseline(y, 30.0f), "SOURCE CONTROL", th.fg_muted, *ctx.font);
    (void)hx;
    y += 30.0f;

    // Commit message box (single line; grows later if needed)
    Rect input{x0, y, w, lh + 14.0f};
    ctx.r->add_rect(input, th.input_bg, 6.0f, input_active_ && focused_ ? 1.0f : 0.0f, th.accent);
    ctx.r->push_clip(input.inset(4.0f));
    float tb = baseline(input.y, input.h);
    float tx = input.x + 8.0f;
    if (message_.empty()) {
        std::string ph = "Message (Ctrl+Enter to commit on '" + branch_ + "')";
        ctx.r->add_text(tx, tb, ph, th.fg_muted, *ctx.font);
    }
    float caret = ctx.r->add_text(tx, tb, message_, th.fg, *ctx.font);
    if (input_active_ && focused_)
        ctx.r->add_rect({caret, input.y + 6.0f, 2.0f, input.h - 12.0f}, th.accent);
    ctx.r->pop_clip();
    hits_.push_back({input, -2, false});
    y += input.h + 8.0f;

    bool any_staged = std::any_of(rows_.begin(), rows_.end(), [](auto& r) { return r.kind == RowKind::Staged; });
    Rect button{x0, y, w, lh + 10.0f};
    Color bc = th.accent;
    if (!any_staged || message_.empty()) bc.a *= 0.45f;
    ctx.r->add_rect(button, bc, 6.0f);
    std::string label = "Commit";
    float lw = (static_cast<float>(label.size()) + 2.0f) * cw;
    float bx = button.x + (button.w - lw) * 0.5f;
    ctx.r->add_char(bx, baseline(button.y, button.h), 0xEAB2, th.tab_active_bg, *ctx.font);
    ctx.r->add_text(bx + 2.0f * cw, baseline(button.y, button.h), label, th.tab_active_bg, *ctx.font);
    hits_.push_back({button, -1, false});
    y += button.h + 12.0f;

    Rect list{bounds.x, y, bounds.w, bounds.y + bounds.h - y};
    ctx.r->push_clip(list);
    float ry = list.y - scroll_y_;
    for (size_t ri = 0; ri < rows_.size(); ri++, ry += row_h_) {
        if (ry + row_h_ < list.y || ry > list.y + list.h) continue;
        const Row& row = rows_[ri];
        float rb = baseline(ry, row_h_);
        Rect rr{bounds.x + 4.0f, ry, bounds.w - 8.0f, row_h_};
        float right = bounds.x + bounds.w - 14.0f;
        if (row.kind == RowKind::SectionStaged || row.kind == RowKind::SectionChanges) {
            bool staged = row.kind == RowKind::SectionStaged;
            size_t n = std::count_if(rows_.begin(), rows_.end(), [&](auto& r) {
                return r.kind == (staged ? RowKind::Staged : RowKind::Change);
            });
            std::string title = std::string(staged ? "Staged Changes" : "Changes");
            float ex = ctx.r->add_text(bounds.x + 14.0f, rb, title, th.fg, *ctx.font);
            std::string count = std::to_string(n);
            float pill_w = static_cast<float>(count.size()) * cw + 12.0f;
            ctx.r->add_rect({ex + 8.0f, ry + 4.0f, pill_w, row_h_ - 8.0f}, th.surface_hover, (row_h_ - 8.0f) * 0.5f);
            ctx.r->add_text(ex + 14.0f, rb, count, th.fg_muted, *ctx.font);
            if (!staged && n > 0) {
                ctx.r->add_char(right - cw, rb, 0xEA60, th.fg_muted, *ctx.font);  // stage all
                hits_.push_back({{right - cw - 6.0f, ry, cw + 12.0f, row_h_}, static_cast<int>(ri), true});
            }
            continue;
        }
        const GitStatus& st = status_[row.index];
        bool staged = row.kind == RowKind::Staged;
        FileStatus fs_ = staged ? st.index_status : st.wt_status;
        Color c = status_color(fs_, th);
        // Action (+ / -) then the status letter on the right.
        ctx.r->add_text(right - cw, rb, letter(fs_), c, *ctx.font);
        float ax = right - cw - 22.0f;
        ctx.r->add_char(ax, rb, staged ? 0xEB3B : 0xEA60, th.fg_muted, *ctx.font);
        hits_.push_back({{ax - 6.0f, ry, cw + 12.0f, row_h_}, static_cast<int>(ri), true});
        hits_.push_back({rr, static_cast<int>(ri), false});

        std::string name = st.path.filename().string();
        std::string dir = st.path.parent_path().string();
        float nx = bounds.x + 26.0f;
        ctx.r->add_char(nx - 14.0f, rb, 0xEA7B, th.fg_muted, *ctx.font);
        ctx.r->push_clip({bounds.x, ry, ax - 8.0f - bounds.x, row_h_});
        ctx.r->set_fade(ax - 30.0f, ax - 8.0f);
        Color nc = fs_ == FileStatus::Deleted ? th.git_deleted : th.fg;
        float ex = ctx.r->add_text(nx + 4.0f, rb, name, nc, *ctx.font);
        if (!dir.empty()) ctx.r->add_text(ex + cw, rb, dir, th.fg_muted, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    if (status_.empty())
        ctx.r->add_text(bounds.x + 14.0f, baseline(list.y + row_h_, row_h_), "No changes", th.fg_muted, *ctx.font);
    ctx.r->pop_clip();
}

void ScmPanel::do_commit() {
    if (cb_.commit) cb_.commit(message_);
}

bool ScmPanel::handle_key(const KeyEvent& e) {
    if (!e.pressed || !input_active_) return false;
    bool ctrl = e.mods & KeyMod::Ctrl;
    switch (e.keycode) {
        case SDLK_RETURN:
        case SDLK_RETURN2:
            if (ctrl) do_commit();
            return true;
        case SDLK_BACKSPACE:
            if (ctrl) {
                while (!message_.empty() && message_.back() == ' ') message_.pop_back();
                while (!message_.empty() && message_.back() != ' ') message_.pop_back();
            } else {
                while (!message_.empty() && (static_cast<unsigned char>(message_.back()) & 0xC0) == 0x80)
                    message_.pop_back();
                if (!message_.empty()) message_.pop_back();
            }
            return true;
        case SDLK_ESCAPE:
            input_active_ = false;
            return true;
        default:
            return false;
    }
}

bool ScmPanel::handle_text(const std::string& text) {
    if (!input_active_) return false;
    message_ += text;
    return true;
}

bool ScmPanel::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    if (e.scroll_y != 0.0f) {
        float max_scroll = std::max(0.0f, static_cast<float>(rows_.size()) * row_h_ - bounds_.h * 0.5f);
        scroll_y_ = std::clamp(scroll_y_ - e.scroll_y * row_h_ * 3.0f, 0.0f, max_scroll);
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return true;
    input_active_ = false;
    for (const Hit& h : hits_) {  // actions were pushed before their rows, so they win
        if (!h.rect.contains(e.x, e.y)) continue;
        if (h.row == -2) { input_active_ = true; return true; }
        if (h.row == -1) { do_commit(); return true; }
        const Row& row = rows_[static_cast<size_t>(h.row)];
        if (row.kind == RowKind::SectionChanges) { if (h.action && cb_.stage_all) cb_.stage_all(); return true; }
        if (row.kind == RowKind::SectionStaged) return true;
        fs::path abs = workdir_ / status_[row.index].path;
        if (h.action) {
            if (row.kind == RowKind::Staged) { if (cb_.unstage) cb_.unstage(abs); }
            else if (cb_.stage) cb_.stage(abs);
        } else if (cb_.open && status_[row.index].wt_status != FileStatus::Deleted) {
            cb_.open(abs);
        }
        return true;
    }
    return true;
}

} // namespace oh
