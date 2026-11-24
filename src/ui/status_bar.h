#pragma once
#include "ui/panel.h"
#include <functional>

namespace oh {

class Editor;

class StatusBar : public Panel {
public:
    using InfoProvider = std::function<std::string()>;

    void set_editor(std::shared_ptr<Editor> e) { editor_ = e; }
    void set_git_branch(const std::string& b) { git_branch_ = b; }
    void set_message(const std::string& msg, float duration = 3.0f);

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt) override;

private:
    std::shared_ptr<Editor> editor_;
    std::string git_branch_;
    std::string message_;
    float message_timer_ = 0;
};

} // namespace oh
