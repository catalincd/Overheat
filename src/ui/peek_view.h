#pragma once
#include "ui/panel.h"
#include "ui/editor_view.h"
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace oh {

// VS Code-style peek widget embedded below a line of the editor (Peek Definition,
// Find All References, several definitions): a read-only preview of the selected
// location on the left, the locations grouped by file on the right. Enter / double
// click opens the location, Escape closes.
class PeekView {
public:
    struct Item {
        fs::path path;
        Position start{}, end{};  // byte columns
        std::string line_text;    // the start line, for the list
    };
    // Loads a read-only editor for the preview (shared across items of one file).
    using Loader = std::function<std::shared_ptr<Editor>(const fs::path&)>;
    using OpenFn = std::function<void(const Item&)>;

    void open(std::string title, std::vector<Item> items, size_t anchor_line, const Editor* host, Loader loader,
              OpenFn on_open, std::function<void()> on_close);
    void close();
    bool visible() const { return visible_; }
    size_t anchor_line() const { return anchor_line_; }
    const Editor* host() const { return host_; }
    size_t count() const { return items_.size(); }
    int selected() const { return selected_; }
    const Item* selected_item() const;
    // Zone height for an editor line height.
    float height(float line_h) const { return std::round(line_h * (BODY_LINES + 1.0f) + 14.0f); }

    void render(RenderContext& ctx, Rect zone);
    bool handle_key(const KeyEvent& e);
    bool handle_mouse(const MouseEvent& e);  // true when inside the widget
    Rect rect() const { return rect_; }
    void set_root(fs::path root) { root_ = std::move(root); }  // folders are shown relative to it

private:
    fs::path root_;
    std::string folder_of(const fs::path& p) const;
    static constexpr float BODY_LINES = 12.0f;
    struct Row {
        bool header;
        size_t item;     // first item of the file for headers
        size_t count;    // items in the file (headers)
    };
    std::string title_;
    std::vector<Item> items_;
    std::vector<Row> rows_;
    int selected_ = 0;   // item index
    int first_row_ = 0;  // list scroll
    size_t anchor_line_ = 0;
    const Editor* host_ = nullptr;
    bool visible_ = false;
    Loader loader_;
    OpenFn on_open_;
    std::function<void()> on_close_;
    std::unordered_map<std::string, std::shared_ptr<Editor>> editors_;
    std::shared_ptr<Editor> preview_;
    EditorView preview_view_;
    bool reveal_pending_ = false;
    Rect rect_{}, list_rect_{}, preview_rect_{}, close_rect_{};
    float row_h_ = 20.0f;
    int visible_rows_ = 1;

    void select(int item);
    int row_of_item(int item) const;
};

} // namespace oh
