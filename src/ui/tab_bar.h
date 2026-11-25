#pragma once
#include "ui/panel.h"
#include <vector>
#include <functional>

namespace oh {

class Editor;

struct Tab {
    std::shared_ptr<Editor> editor;
    bool pinned = false;
};

class TabBar : public Panel {
public:
    using TabChangeCallback = std::function<void(int)>;

    void add_tab(std::shared_ptr<Editor> editor);
    void close_tab(int index);
    void set_active(int index);
    int active_index() const { return active_; }
    std::shared_ptr<Editor> active_editor() const;
    const std::vector<Tab>& tabs() const { return tabs_; }
    bool empty() const { return tabs_.empty(); }

    void set_on_change(TabChangeCallback cb) { on_change_ = cb; }
    void next_tab();
    void prev_tab();

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_mouse(const MouseEvent& e) override;

private:
    static constexpr float PAD = 14.0f;
    static constexpr float CLOSE_W = 16.0f;
    float char_w_ = 9.0f;
    std::string tab_title(const Tab& tab) const;
    float tab_width(const Tab& tab) const;

    std::vector<Tab> tabs_;
    int active_ = -1;
    float scroll_x_ = 0;
    TabChangeCallback on_change_;
};

} // namespace oh
