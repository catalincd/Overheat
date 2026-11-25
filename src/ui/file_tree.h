#pragma once
#include "ui/panel.h"
#include "vfs.h"
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace oh {

class FileTree : public Panel {
public:
    using OpenCallback = std::function<void(const fs::path&)>;

    void set_root(const fs::path& root, Vfs* vfs);
    // Absolute path -> status; folders inherit the status of changed descendants.
    void set_git_status(const std::unordered_map<std::string, FileStatus>& status);
    const fs::path& root() const { return root_; }
    void set_on_open(OpenCallback cb) { on_open_ = cb; }
    void refresh();

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    fs::path root_;
    Vfs* vfs_ = nullptr;
    FileEntry root_entry_;
    std::unordered_map<std::string, FileStatus> git_status_;
    void collect_expanded(const FileEntry& e, std::unordered_set<std::string>& out) const;
    void restore_expanded(FileEntry& e, const std::unordered_set<std::string>& open);
    std::vector<FileEntry*> visible_entries_;
    int selected_index_ = -1;
    float scroll_y_ = 0;
    float row_h_ = 24.0f;
    static constexpr float HEADER_H = 30.0f;
    OpenCallback on_open_;

    void scan_directory(FileEntry& entry);
    void flatten_visible(FileEntry& entry);
    void toggle_expand(FileEntry& entry);
};

} // namespace oh
