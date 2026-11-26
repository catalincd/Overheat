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
        // Symbol pickers (nav_glue.cpp): an icon glyph per item (0 = none), a callback
        // on every query change (live server queries), and no local fuzzy filtering
        // when the server already filtered the items.
        std::vector<uint32_t> icons;
        std::function<void(const std::string& query)> on_query;
        bool server_filtered = false;
    };
    void pick(PickOptions opts);
    // Replace a live picker's items (e.g. workspace symbols arriving for the query).
    void set_pick_items(std::vector<std::string> items, std::vector<std::string> details, std::vector<uint32_t> icons);
    bool picking() const { return visible_ && mode_ == Mode::Pick; }
    const std::string& query() const { return query_; }
    void move_selection(int delta) { select(selected_index_ + delta); }
    void accept();  // as if Enter were pressed

    void show(Mode mode = Mode::Commands);
    void hide();
    bool is_visible() const { return visible_; }

    void set_commands(std::vector<Command> cmds) { commands_ = std::move(cmds); }
    void set_file_list(std::vector<fs::path> files) { file_list_ = std::move(files); }
    void set_root(fs::path root) { root_ = std::move(root); }
    // Recently opened files, newest first: listed first in Quick Open.
    void set_recent(std::vector<fs::path> files) { recent_ = std::move(files); }
    // Top edge of the box (e.g. over the Command Center); < 0 = default placement.
    void set_top(float y) { top_ = y; }

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
    std::vector<fs::path> recent_;
    std::vector<fs::path> files_view_;  // Files mode: recent_ (that exist) then file_list_
    size_t recent_count_ = 0;
    float top_ = -1;
    FileOpenCallback on_file_open_;
    void build_files_view();

    struct MatchResult {
        int index;
        int score;
        std::string display;
        std::string detail;  // muted secondary text (folder, shortcut)
        bool detail_right = false;
        std::string section;  // muted label at the right of a group's first row
    };
    std::vector<MatchResult> filtered_;
    PickOptions pick_;
    anim::Smooth appear_;
    anim::Wobble wobble_;
    int first_visible_ = 0;
    int last_previewed_ = -1;
    void select(int index);

    void update_filter();
    int fuzzy_score(const std::string& pattern, const std::string& text) const;
};

} // namespace oh
