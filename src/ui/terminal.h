#pragma once
#include "ui/panel.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>

namespace oh {

// Cell colours are references resolved against the theme at draw time, so a theme
// switch recolours existing output: DEFAULT, ANSI index (0-255) or 24-bit RGB.
namespace termcolor {
constexpr uint32_t DEFAULT = 0;
constexpr uint32_t INDEXED = 1u << 24;
constexpr uint32_t RGB = 2u << 24;
inline uint32_t indexed(int i) { return INDEXED | static_cast<uint32_t>(i & 0xFF); }
inline uint32_t rgb(int r, int g, int b) {
    return RGB | (static_cast<uint32_t>(r & 0xFF) << 16) | (static_cast<uint32_t>(g & 0xFF) << 8) |
           static_cast<uint32_t>(b & 0xFF);
}
} // namespace termcolor

struct TermCell {
    uint32_t ch = ' ';
    uint32_t fg = termcolor::DEFAULT;
    uint32_t bg = termcolor::DEFAULT;
    bool bold = false;
};

class Terminal : public Panel {
public:
    Terminal();
    ~Terminal();

    // Empty argv runs $SHELL. cwd empty = inherit.
    bool spawn(const std::vector<std::string>& argv = {}, const fs::path& cwd = {});
    void kill();
    bool is_alive() const { return alive_; }

    void write(const std::string& data);
    bool handle_mouse(const MouseEvent& e) override;
    bool has_selection() const { return has_selection_; }
    std::string selection_text();
    void paste(const std::string& text);

    void render(RenderContext& ctx, Rect bounds) override;
    bool handle_key(const KeyEvent& e) override;
    bool handle_text(const std::string& text) override;
    bool wants_focus() const override { return true; }

    void update(float dt) override;

    void resize(int cols, int rows);

private:
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::atomic<bool> alive_{false};
    std::thread reader_thread_;

    std::mutex buffer_mutex_;
    std::vector<std::vector<TermCell>> screen_;
    int cols_ = 80, rows_ = 24;
    int cursor_x_ = 0, cursor_y_ = 0;
    float scroll_y_ = 0;

    // Scrollback (oldest first). dropped_ counts lines trimmed from the front, so
    // "absolute" line numbers (dropped_ + index in scrollback+screen) stay stable.
    std::deque<std::vector<TermCell>> scrollback_;
    uint64_t dropped_ = 0;

    // Mouse selection in absolute line / column coordinates.
    struct CellPos { int64_t line = 0; int col = 0; };
    CellPos sel_anchor_, sel_head_;
    bool has_selection_ = false, selecting_ = false;
    Rect inner_{};
    float cell_w_ = 9.0f, cell_h_ = 20.0f;
    CellPos cell_at(float x, float y) const;
    const std::vector<TermCell>* row_at(int64_t abs_line) const;
    bool selected(int64_t line, int col) const;
    static constexpr int MAX_SCROLLBACK = 10000;

    // VT100 parsing state
    enum class ParseState { Normal, Escape, CSI, OSC };
    ParseState parse_state_ = ParseState::Normal;
    std::string csi_params_;
    std::string osc_data_;
    uint32_t current_fg_ = termcolor::DEFAULT;
    uint32_t current_bg_ = termcolor::DEFAULT;
    bool bold_ = false;

    void process_output(const char* data, size_t len);
    void process_char(char c);
    void put_codepoint(uint32_t cp);
    bool bracketed_paste_ = false;
    int saved_x_ = 0, saved_y_ = 0;
    uint32_t saved_fg_ = termcolor::DEFAULT, saved_bg_ = termcolor::DEFAULT;
    uint32_t utf8_cp_ = 0;
    int utf8_remaining_ = 0;
    void process_csi(char final_char);
    void process_sgr(const std::vector<int>& params);
    void scroll_up();
    void clear_line(int y, int from_x = 0, int to_x = -1);
    static Color resolve(uint32_t code, const Theme& th, bool foreground);
};

} // namespace oh
