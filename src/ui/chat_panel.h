#pragma once
#include "ui/panel.h"
#include "llm.h"
#include <functional>
#include <mutex>

namespace oh {

// Draft AI chat view: conversation, multi-line prompt box, streaming replies.
class ChatPanel : public Panel {
public:
    struct Callbacks {
        std::function<void(std::vector<ChatMessage>)> send;  // full history incl. the new user turn
        std::function<void()> stop;
        std::function<void()> pick_backend;
    };
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }
    void set_backend_name(std::string n) { backend_name_ = std::move(n); }
    void set_busy(bool b) { busy_ = b; }
    void focus_input() { input_active_ = true; }
    bool input_focused() const { return focused_ && input_active_; }
    void set_draft(std::string text) { draft_ = std::move(text); input_active_ = true; }

    // Thread-safe: called from the LLM worker via the host.
    void append_reply(const std::string& delta);
    void finish_reply(const std::string& error);
    void clear();

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_mouse(const MouseEvent& e) override;
    bool handle_text(const std::string& text) override;

private:
    Callbacks cb_;
    std::mutex mutex_;
    std::vector<ChatMessage> messages_;
    std::string draft_;
    std::string backend_name_ = "Flint";
    bool busy_ = false, input_active_ = true, follow_ = true;
    float scroll_ = 0, content_h_ = 0, view_h_ = 0;
    Rect new_btn_{}, backend_btn_{}, input_rect_{};
    void submit();
};

} // namespace oh
