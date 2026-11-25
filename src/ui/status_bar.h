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
    void set_git_sync(int ahead, int behind) { ahead_ = ahead; behind_ = behind; }
    void set_diagnostic_counts(int errors, int warnings) { errors_ = errors; warnings_ = warnings; }
    // Non-empty while connected to a remote host; shown as an accent badge.
    void set_remote(const std::string& label) { remote_ = label; }
    void set_message(const std::string& msg, float duration = 3.0f);

    void render(RenderContext& ctx, Rect bounds) override;
    void update(float dt) override;

private:
    std::shared_ptr<Editor> editor_;
    std::string git_branch_;
    std::string remote_;
    int ahead_ = 0, behind_ = 0;
    int errors_ = 0, warnings_ = 0;
    std::string message_;
    float message_timer_ = 0;
};

} // namespace oh
