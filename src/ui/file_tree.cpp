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

    float line_h = ctx.font->line_height();
    float char_w = ctx.font->char_width();

    // Background
    ctx.r->add_rect(bounds, ctx.theme->sidebar_bg);

    // Right border
    ctx.r->add_rect({bounds.x + bounds.w - 1.0f, bounds.y, 1.0f, bounds.h},
                     ctx.theme->border);

    size_t first_entry = static_cast<size_t>(scroll_y_ / line_h);
    size_t visible_count = static_cast<size_t>(bounds.h / line_h) + 2;

    for (size_t i = 0; i < visible_count && (first_entry + i) < visible_entries_.size(); i++) {
        size_t idx = first_entry + i;
        FileEntry* entry = visible_entries_[idx];

        float y = bounds.y + static_cast<float>(i) * line_h -
                  std::fmod(scroll_y_, line_h);

        if (y + line_h < bounds.y || y > bounds.y + bounds.h) continue;

        // Selection highlight
        if (static_cast<int>(idx) == selected_index_) {
            ctx.r->add_rect({bounds.x, y, bounds.w, line_h}, ctx.theme->tab_active_bg);
        }

        // Indent
        float indent = 8.0f + static_cast<float>(entry->depth) * 16.0f;
        float text_x = bounds.x + indent;

        // Directory indicator or file icon space
        if (entry->is_dir) {
            const char* indicator = entry->expanded ? "v " : "> ";
            ctx.r->add_text(text_x, y + ctx.font->ascender(),
                               indicator, ctx.theme->fg, *ctx.font);
            text_x += char_w * 2.0f;
        } else {
            text_x += char_w * 2.0f; // Same indent as dir indicator
        }

        // File/dir name with color based on git status
        Color name_color = ctx.theme->fg;
        switch (entry->git_status) {
            case FileStatus::Modified: name_color = ctx.theme->git_modified; break;
            case FileStatus::Added:
            case FileStatus::Untracked: name_color = ctx.theme->git_added; break;
            case FileStatus::Deleted: name_color = ctx.theme->git_deleted; break;
            case FileStatus::Ignored: name_color = ctx.theme->comment; break;
            default: break;
        }

        ctx.r->add_text(text_x, y + ctx.font->ascender(),
                           entry->name, name_color, *ctx.font);
    }
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
    if (!bounds_.contains(e.x, e.y) && e.scroll_y == 0.0f) return false;

    // Scroll
    if (e.scroll_y != 0.0f && bounds_.contains(e.x, e.y)) {
        float line_h = 20.0f; // Approximate
        scroll_y_ -= e.scroll_y * line_h * 3.0f;
        scroll_y_ = std::max(0.0f, scroll_y_);
        float max_scroll = std::max(0.0f,
            static_cast<float>(visible_entries_.size()) * line_h - bounds_.h);
        scroll_y_ = std::min(scroll_y_, max_scroll);
        return true;
    }

    if (e.button == 1 && e.pressed) {
        float line_h = 20.0f;
        float rel_y = e.y - bounds_.y + scroll_y_;
        int clicked = static_cast<int>(rel_y / line_h);

        if (clicked >= 0 && clicked < static_cast<int>(visible_entries_.size())) {
            selected_index_ = clicked;
            auto* entry = visible_entries_[static_cast<size_t>(clicked)];

            if (e.double_click) {
                if (entry->is_dir) {
                    toggle_expand(*entry);
                } else if (on_open_) {
                    on_open_(entry->path);
                }
            } else if (entry->is_dir) {
                toggle_expand(*entry);
            }
        }
        return true;
    }

    return false;
}

} // namespace oh
