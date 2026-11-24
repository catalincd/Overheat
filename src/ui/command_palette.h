#pragma once
#include "ui/panel.h"
#include <functional>

namespace oh {

class CommandPalette : public Panel {
public:
    enum class Mode { Commands, Files, Symbols };

    void show(Mode mode = Mode::Commands);
    void hide();
    bool is_visible() const { return visible_; }

    void set_commands(std::vector<Command> cmds) { commands_ = std::move(cmds); }
    void set_file_list(std::vector<fs::path> files) { file_list_ = std::move(files); }

    using FileOpenCallback = std::function<void(const fs::path&)>;
    void set_on_file_open(FileOpenCallback cb) { on_file_open_ = cb; }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_text(const std::string& text) override;

private:
    bool visible_ = false;
    Mode mode_ = Mode::Commands;
    std::string query_;
    int selected_index_ = 0;

    std::vector<Command> commands_;
    std::vector<fs::path> file_list_;
    FileOpenCallback on_file_open_;

    struct MatchResult {
        int index;
        int score;
        std::string display;
    };
    std::vector<MatchResult> filtered_;

    void update_filter();
    int fuzzy_score(const std::string& pattern, const std::string& text) const;
};

} // namespace oh
