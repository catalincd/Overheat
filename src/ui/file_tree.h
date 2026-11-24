#pragma once
#include "ui/panel.h"
#include <functional>

namespace oh {

class FileTree : public Panel {
public:
    using OpenCallback = std::function<void(const fs::path&)>;

    void set_root(const fs::path& root);
    void set_on_open(OpenCallback cb) { on_open_ = cb; }
    void refresh();

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    fs::path root_;
    FileEntry root_entry_;
    std::vector<FileEntry*> visible_entries_;
    int selected_index_ = -1;
    float scroll_y_ = 0;
    OpenCallback on_open_;

    void scan_directory(FileEntry& entry);
    void flatten_visible(FileEntry& entry);
    void toggle_expand(FileEntry& entry);
};

} // namespace oh
