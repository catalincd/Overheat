#include "ui/file_tree.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

void FileTree::set_root(const fs::path& root) {
    root_ = root;
    refresh();
}

void FileTree::refresh() {
    root_entry_ = {};
    root_entry_.name = root_.filename().string();
    root_entry_.path = root_;
    root_entry_.is_dir = true;
    root_entry_.expanded = true;
    root_entry_.depth = 0;

    scan_directory(root_entry_);

    visible_entries_.clear();
    flatten_visible(root_entry_);
}

void FileTree::scan_directory(FileEntry& entry) {
    if (!entry.is_dir) return;

    entry.children.clear();

    std::error_code ec;
    for (auto& dir_entry : fs::directory_iterator(entry.path, ec)) {
        FileEntry child;
        child.name = dir_entry.path().filename().string();
        child.path = dir_entry.path();
        child.is_dir = dir_entry.is_directory(ec);
        child.depth = entry.depth + 1;
        child.expanded = false;

        // Skip hidden files/dirs (starting with '.')
        if (!child.name.empty() && child.name[0] == '.') continue;

        if (child.is_dir) {
            scan_directory(child);
        }

        entry.children.push_back(std::move(child));
    }

    // Sort: directories first, then alphabetical (case-insensitive)
    std::sort(entry.children.begin(), entry.children.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  // Case-insensitive comparison
                  std::string la = a.name, lb = b.name;
                  for (auto& c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  for (auto& c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  return la < lb;
              });
}

void FileTree::flatten_visible(FileEntry& entry) {
    // Don't add the root itself to the visible list
    for (auto& child : entry.children) {
        visible_entries_.push_back(&child);
        if (child.is_dir && child.expanded) {
            flatten_visible(child);
        }
    }
}

void FileTree::toggle_expand(FileEntry& entry) {
    if (!entry.is_dir) return;
    entry.expanded = !entry.expanded;

    // Rebuild visible list
    visible_entries_.clear();
    flatten_visible(root_entry_);
}

void FileTree::render(RenderContext& ctx, Rect bounds) {
    bounds_ = bounds;
    const Theme& th = *ctx.theme;
    row_h_ = std::round(ctx.font->line_height() + 4.0f);
    float char_w = ctx.font->char_width();

    ctx.r->add_rect(bounds, th.sidebar_bg);

    // Header: small muted section title with the project name.
    float hb = bounds.y + (HEADER_H - ctx.font->line_height()) * 0.5f + ctx.font->ascender();
    float hx = ctx.r->add_text(bounds.x + 14.0f, hb, "EXPLORER", th.fg_muted, *ctx.font);
    ctx.r->add_text(hx + char_w, hb, root_entry_.name, th.fg_muted, *ctx.font);

    Rect list{bounds.x, bounds.y + HEADER_H, bounds.w, bounds.h - HEADER_H};
    ctx.r->push_clip(list);
    size_t first = static_cast<size_t>(scroll_y_ / row_h_);
    size_t count = static_cast<size_t>(list.h / row_h_) + 2;
    for (size_t idx = first; idx < first + count && idx < visible_entries_.size(); idx++) {
        FileEntry* entry = visible_entries_[idx];
        float y = list.y + static_cast<float>(idx) * row_h_ - scroll_y_;
        float baseline = y + (row_h_ - ctx.font->line_height()) * 0.5f + ctx.font->ascender();

        if (static_cast<int>(idx) == selected_index_) {
            ctx.r->add_rect({bounds.x + 4.0f, y, bounds.w - 8.0f, row_h_}, th.tree_selection, 4.0f);
        }

        float x = bounds.x + 10.0f + static_cast<float>(entry->depth) * 14.0f;
        if (entry->is_dir) {
            ctx.r->add_char(x, baseline, entry->expanded ? 0xEAB4 : 0xEAB6, th.fg_muted, *ctx.font);
            ctx.r->add_char(x + char_w + 6.0f, baseline, entry->expanded ? 0xEAF7 : 0xEA83, th.accent, *ctx.font);
        } else {
            ctx.r->add_char(x + char_w + 6.0f, baseline, 0xEA7B, th.fg_muted, *ctx.font);
        }
        float name_x = x + 2.0f * char_w + 14.0f;

        Color name_color = th.fg;
        const char* badge = nullptr;
        switch (entry->git_status) {
            case FileStatus::Modified:  name_color = th.git_modified;  badge = "M"; break;
            case FileStatus::Added:     name_color = th.git_added;     badge = "A"; break;
            case FileStatus::Untracked: name_color = th.git_untracked; badge = "U"; break;
            case FileStatus::Deleted:   name_color = th.git_deleted;   badge = "D"; break;
            case FileStatus::Renamed:   name_color = th.git_modified;  badge = "R"; break;
            case FileStatus::Ignored:   name_color = th.git_ignored; break;
            default: break;
        }
        float right = bounds.x + bounds.w - 12.0f;
        if (badge) {
            ctx.r->add_text(right - char_w, baseline, badge, name_color, *ctx.font);
            right -= char_w + 8.0f;
        }
        ctx.r->push_clip({bounds.x, y, right - bounds.x, row_h_});
        ctx.r->set_fade(right - 20.0f, right);
        ctx.r->add_text(name_x, baseline, entry->name, name_color, *ctx.font);
        ctx.r->set_fade(0, 0);
        ctx.r->pop_clip();
    }
    ctx.r->pop_clip();
}

bool FileTree::handle_key(const KeyEvent& e) {
    if (!e.pressed) return false;

    switch (e.keycode) {
        case SDLK_UP:
            if (selected_index_ > 0) selected_index_--;
            return true;
        case SDLK_DOWN:
            if (selected_index_ < static_cast<int>(visible_entries_.size()) - 1)
                selected_index_++;
            return true;
        case SDLK_RETURN:
        case SDLK_RETURN2:
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(visible_entries_.size())) {
                auto* entry = visible_entries_[static_cast<size_t>(selected_index_)];
                if (entry->is_dir) {
                    toggle_expand(*entry);
                } else if (on_open_) {
                    on_open_(entry->path);
                }
            }
            return true;
        case SDLK_LEFT:
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(visible_entries_.size())) {
                auto* entry = visible_entries_[static_cast<size_t>(selected_index_)];
                if (entry->is_dir && entry->expanded) {
                    toggle_expand(*entry);
                }
            }
            return true;
        case SDLK_RIGHT:
            if (selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(visible_entries_.size())) {
                auto* entry = visible_entries_[static_cast<size_t>(selected_index_)];
                if (entry->is_dir && !entry->expanded) {
                    toggle_expand(*entry);
                }
            }
            return true;
        default:
            break;
    }
    return false;
}

bool FileTree::handle_mouse(const MouseEvent& e) {
    if (!bounds_.contains(e.x, e.y)) return false;
    float list_h = bounds_.h - HEADER_H;

    if (e.scroll_y != 0.0f) {
        float max_scroll = std::max(0.0f, static_cast<float>(visible_entries_.size()) * row_h_ - list_h);
        scroll_y_ = std::clamp(scroll_y_ - e.scroll_y * row_h_ * 3.0f, 0.0f, max_scroll);
        return true;
    }
    if (e.button != 1 || !e.pressed || e.motion) return e.button == 1;

    float rel_y = e.y - bounds_.y - HEADER_H + scroll_y_;
    if (rel_y < 0) return true;
    int clicked = static_cast<int>(rel_y / row_h_);
    if (clicked < 0 || clicked >= static_cast<int>(visible_entries_.size())) return true;
    selected_index_ = clicked;
    auto* entry = visible_entries_[static_cast<size_t>(clicked)];
    if (entry->is_dir) {
        if (!e.double_click) toggle_expand(*entry);
    } else if (on_open_) {
        on_open_(entry->path);
    }
    return true;
}

} // namespace oh
