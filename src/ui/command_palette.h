#pragma once
#include "ui/panel.h"
#include <functional>
#include "anim.h"

namespace oh {

class CommandPalette : public Panel {
public:
    enum class Mode { Commands, Files, Symbols, Pick };

    // Generic chooser. on_pick gets the chosen item index, or -1 with the typed
    // text when allow_free_text is set and nothing matches. on_preview fires as the
    // highlighted item changes; on_cancel fires on Escape.
    struct PickOptions {
        std::string placeholder;
        std::vector<std::string> items;
        std::vector<std::string> details;  // optional muted text right of each item
        std::function<void(int index, const std::string& query)> on_pick;
        std::function<void(int index)> on_preview;
        std::function<void()> on_cancel;
        bool allow_free_text = false;
        bool password = false;  // mask typed text
        int initial = 0;
    };
    void pick(PickOptions opts);
    void move_selection(int delta) { select(selected_index_ + delta); }
    void accept();  // as if Enter were pressed

    void show(Mode mode = Mode::Commands);
    void hide();
    bool is_visible() const { return visible_; }

    void set_commands(std::vector<Command> cmds) { commands_ = std::move(cmds); }
    void set_file_list(std::vector<fs::path> files) { file_list_ = std::move(files); }
    void set_root(fs::path root) { root_ = std::move(root); }

    using FileOpenCallback = std::function<void(const fs::path&)>;
    void set_on_file_open(FileOpenCallback cb) { on_file_open_ = cb; }

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_text(const std::string& text) override;

private:
    fs::path root_;
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
        std::string detail;  // muted secondary text (folder, shortcut)
        bool detail_right = false;
    };
    std::vector<MatchResult> filtered_;
    PickOptions pick_;
    anim::Smooth appear_;
    int first_visible_ = 0;
    int last_previewed_ = -1;
    void select(int index);

    void update_filter();
    int fuzzy_score(const std::string& pattern, const std::string& text) const;
};

} // namespace oh
