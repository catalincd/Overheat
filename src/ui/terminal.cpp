#include "ui/terminal.h"
#include "renderer/font_atlas.h"
#include "renderer/renderer2d.h"
#include "renderer/vulkan_context.h"

#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <SDL3/SDL.h>

namespace oh {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Terminal::Terminal() {
    screen_.resize(rows_, std::vector<TermCell>(cols_));
}

Terminal::~Terminal() {
    kill();
}

// ---------------------------------------------------------------------------
// PTY spawning
// ---------------------------------------------------------------------------

bool Terminal::spawn(const std::string& shell) {
    if (alive_) return false;

    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows_);
    ws.ws_col = static_cast<unsigned short>(cols_);

    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (pid < 0) return false;

    if (pid == 0) {
        // Child process
        const char* sh = nullptr;
        if (!shell.empty()) {
            sh = shell.c_str();
        } else {
            sh = std::getenv("SHELL");
            if (!sh) sh = "/bin/bash";
        }
        setenv("TERM", "xterm-256color", 1);
        execlp(sh, sh, nullptr);
        _exit(127);
    }

    // Parent
    child_pid_ = pid;
    alive_ = true;

    // Non-blocking reads
    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);

    reader_thread_ = std::thread([this]() {
        char buf[4096];
        while (alive_) {
            ssize_t n = ::read(master_fd_, buf, sizeof(buf));
            if (n > 0) {
                process_output(buf, static_cast<size_t>(n));
                request_redraw();
            } else if (n == 0) {
                // EOF – child closed
                alive_ = false;
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Use a short poll to avoid busy-waiting
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(master_fd_, &fds);
                    struct timeval tv{0, 10000}; // 10 ms
                    select(master_fd_ + 1, &fds, nullptr, nullptr, &tv);
                } else if (errno == EIO) {
                    // Child exited
                    alive_ = false;
                    break;
                }
            }
        }
    });

    return true;
}

void Terminal::kill() {
    if (!alive_ && master_fd_ < 0) return;

    alive_ = false;

    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGHUP);
        waitpid(child_pid_, nullptr, WNOHANG);
        child_pid_ = -1;
    }
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Write to PTY
// ---------------------------------------------------------------------------

void Terminal::write(const std::string& data) {
    if (!alive_ || master_fd_ < 0) return;
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(master_fd_, data.data() + total, data.size() - total);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Terminal::resize(int cols, int rows) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    int old_rows = rows_;
    int old_cols = cols_;
    cols_ = std::max(cols, 1);
    rows_ = std::max(rows, 1);

    // Resize screen buffer
    screen_.resize(rows_);
    for (auto& row : screen_) {
        row.resize(cols_, TermCell{});
    }
    // If we shrank, also trim old row widths (already done by resize above)
    // If we grew vertically, new rows already have default cells

    // Clamp cursor
    cursor_x_ = std::min(cursor_x_, cols_ - 1);
    cursor_y_ = std::min(cursor_y_, rows_ - 1);

    // Inform PTY of new size
    if (master_fd_ >= 0) {
        struct winsize ws{};
        ws.ws_row = static_cast<unsigned short>(rows_);
        ws.ws_col = static_cast<unsigned short>(cols_);
        ioctl(master_fd_, TIOCSWINSZ, &ws);

        if (child_pid_ > 0) {
            ::kill(child_pid_, SIGWINCH);
        }
    }
}

// ---------------------------------------------------------------------------
// Update (called each frame)
// ---------------------------------------------------------------------------

void Terminal::update(float /*dt*/) {
    // Check if child has exited
    if (child_pid_ > 0) {
        int status;
        pid_t result = waitpid(child_pid_, &status, WNOHANG);
        if (result > 0) {
            alive_ = false;
        }
    }
}

// ---------------------------------------------------------------------------
// ANSI color palette (Catppuccin Mocha)
// ---------------------------------------------------------------------------

Color Terminal::ansi_color(int idx) const {
    // Standard 0-15 (Catppuccin Mocha-inspired)
    static const Color palette[16] = {
        Color::hex(0x45475a), // 0  black   (Surface1)
        Color::hex(0xf38ba8), // 1  red
        Color::hex(0xa6e3a1), // 2  green
        Color::hex(0xf9e2af), // 3  yellow
        Color::hex(0x89b4fa), // 4  blue
        Color::hex(0xf5c2e7), // 5  magenta (pink)
        Color::hex(0x94e2d5), // 6  cyan    (teal)
        Color::hex(0xbac2de), // 7  white   (Subtext1)
        // Bright
        Color::hex(0x585b70), // 8  bright black  (Surface2)
        Color::hex(0xf38ba8), // 9  bright red
        Color::hex(0xa6e3a1), // 10 bright green
        Color::hex(0xf9e2af), // 11 bright yellow
        Color::hex(0x89b4fa), // 12 bright blue
        Color::hex(0xf5c2e7), // 13 bright magenta
        Color::hex(0x94e2d5), // 14 bright cyan
        Color::hex(0xcdd6f4), // 15 bright white (Text)
    };

    if (idx < 0) return Color::hex(0xcdd6f4);
    if (idx < 16) return palette[idx];

    // 16-231: 6x6x6 color cube
    if (idx < 232) {
        int ci = idx - 16;
        int b = ci % 6;
        ci /= 6;
        int g = ci % 6;
        int r = ci / 6;
        auto comp = [](int v) -> float { return v == 0 ? 0.0f : (55.0f + 40.0f * v) / 255.0f; };
        return {comp(r), comp(g), comp(b), 1.0f};
    }

    // 232-255: grayscale ramp
    if (idx < 256) {
        float v = (8.0f + 10.0f * (idx - 232)) / 255.0f;
        return {v, v, v, 1.0f};
    }

    return Color::hex(0xcdd6f4);
}

// ---------------------------------------------------------------------------
// Screen manipulation helpers
// ---------------------------------------------------------------------------

void Terminal::scroll_up() {
    // Top line goes to scrollback
    scrollback_.push_back(std::move(screen_[0]));
    if (static_cast<int>(scrollback_.size()) > MAX_SCROLLBACK) {
        scrollback_.erase(scrollback_.begin());
    }
    // Shift everything up
    for (int i = 0; i < rows_ - 1; ++i) {
        screen_[i] = std::move(screen_[i + 1]);
    }
    // Clear last line
    screen_[rows_ - 1] = std::vector<TermCell>(cols_);
}

void Terminal::clear_line(int y, int from_x, int to_x) {
    if (y < 0 || y >= rows_) return;
    if (to_x < 0) to_x = cols_;
    to_x = std::min(to_x, cols_);
    from_x = std::max(from_x, 0);
    for (int x = from_x; x < to_x; ++x) {
        screen_[y][x] = TermCell{};
    }
}

// ---------------------------------------------------------------------------
// Output processing (called from reader thread)
// ---------------------------------------------------------------------------

void Terminal::process_output(const char* data, size_t len) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (size_t i = 0; i < len; ++i) {
        process_char(data[i]);
    }
}

void Terminal::put_codepoint(uint32_t cp) {
    utf8_remaining_ = 0;
    if (cursor_x_ >= cols_) {
        cursor_x_ = 0;
        if (cursor_y_ < rows_ - 1) cursor_y_++;
        else scroll_up();
    }
    auto& cell = screen_[cursor_y_][cursor_x_];
    cell.ch = cp;
    cell.fg = current_fg_;
    cell.bg = current_bg_;
    cell.bold = bold_;
    cursor_x_++;
}

void Terminal::process_char(char c) {
    unsigned char uc = static_cast<unsigned char>(c);

    switch (parse_state_) {
    case ParseState::Normal:
        if (uc == 0x1B) {
            parse_state_ = ParseState::Escape;
        } else if (uc == '\r') {
            cursor_x_ = 0;
        } else if (uc == '\n') {
            if (cursor_y_ < rows_ - 1) {
                cursor_y_++;
            } else {
                scroll_up();
            }
        } else if (uc == '\b') {
            if (cursor_x_ > 0) cursor_x_--;
        } else if (uc == '\t') {
            cursor_x_ = std::min(((cursor_x_ / 8) + 1) * 8, cols_ - 1);
        } else if (uc == '\a') {
            // Bell – ignore
        } else if (uc >= 0x80) {
            if ((uc & 0xC0) == 0x80) {
                if (utf8_remaining_ > 0) {
                    utf8_cp_ = (utf8_cp_ << 6) | (uc & 0x3F);
                    if (--utf8_remaining_ == 0) put_codepoint(utf8_cp_);
                }
            } else {
                utf8_remaining_ = (uc >= 0xF0) ? 3 : (uc >= 0xE0) ? 2 : 1;
                utf8_cp_ = uc & (0x3F >> utf8_remaining_);
            }
        } else if (uc >= 0x20) {
            put_codepoint(uc);
        }
        break;

    case ParseState::Escape:
        if (uc == '[') {
            parse_state_ = ParseState::CSI;
            csi_params_.clear();
        } else if (uc == ']') {
            parse_state_ = ParseState::OSC;
            osc_data_.clear();
        } else if (uc == 'M') {
            // Reverse index – move cursor up, scroll down if at top
            if (cursor_y_ > 0) {
                cursor_y_--;
            } else {
                // Scroll down: insert line at top
                if (rows_ > 1) {
                    for (int i = rows_ - 1; i > 0; --i) {
                        screen_[i] = std::move(screen_[i - 1]);
                    }
                    screen_[0] = std::vector<TermCell>(cols_);
                }
            }
            parse_state_ = ParseState::Normal;
        } else if (uc == 'c') {
            // Reset terminal
            current_fg_ = Color::hex(0xcdd6f4);
            current_bg_ = {0, 0, 0, 0};
            bold_ = false;
            cursor_x_ = 0;
            cursor_y_ = 0;
            for (int y = 0; y < rows_; ++y)
                clear_line(y);
            parse_state_ = ParseState::Normal;
        } else if (uc == '7') {
            // Save cursor (we save position only)
            // Using csi_params_ as temp storage is hacky; keep it simple
            parse_state_ = ParseState::Normal;
        } else if (uc == '8') {
            // Restore cursor
            parse_state_ = ParseState::Normal;
        } else {
            // Unknown escape, ignore
            parse_state_ = ParseState::Normal;
        }
        break;

    case ParseState::CSI:
        if ((uc >= '0' && uc <= '9') || uc == ';' || uc == '?' || uc == '>' || uc == '!') {
            csi_params_ += static_cast<char>(uc);
        } else {
            // Final character
            process_csi(static_cast<char>(uc));
            parse_state_ = ParseState::Normal;
        }
        break;

    case ParseState::OSC:
        if (uc == 0x07) {
            // BEL terminates OSC
            // Handle title change: osc_data_ might be "0;title" or "2;title"
            parse_state_ = ParseState::Normal;
        } else if (uc == 0x1B) {
            // Might be ST (ESC \) – peek would be needed; just end OSC
            parse_state_ = ParseState::Normal;
        } else {
            osc_data_ += static_cast<char>(uc);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// CSI sequence handler
// ---------------------------------------------------------------------------

static std::vector<int> parse_csi_params(const std::string& s) {
    std::vector<int> params;
    // Strip leading '?' or '>' for private modes
    size_t start = 0;
    while (start < s.size() && (s[start] == '?' || s[start] == '>' || s[start] == '!'))
        start++;

    std::string num;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == ';') {
            params.push_back(num.empty() ? 0 : std::stoi(num));
            num.clear();
        } else if (s[i] >= '0' && s[i] <= '9') {
            num += s[i];
        }
    }
    params.push_back(num.empty() ? 0 : std::stoi(num));
    return params;
}

void Terminal::process_csi(char final_char) {
    auto params = parse_csi_params(csi_params_);
    bool is_private = (!csi_params_.empty() && csi_params_[0] == '?');

    int p0 = params.empty() ? 0 : params[0];

    switch (final_char) {
    case 'A': // Cursor up
        cursor_y_ = std::max(0, cursor_y_ - std::max(p0, 1));
        break;
    case 'B': // Cursor down
        cursor_y_ = std::min(rows_ - 1, cursor_y_ + std::max(p0, 1));
        break;
    case 'C': // Cursor right
        cursor_x_ = std::min(cols_ - 1, cursor_x_ + std::max(p0, 1));
        break;
    case 'D': // Cursor left
        cursor_x_ = std::max(0, cursor_x_ - std::max(p0, 1));
        break;
    case 'H': // Cursor position (1-based)
    case 'f': {
        int row = (params.size() >= 1 && params[0] > 0) ? params[0] - 1 : 0;
        int col = (params.size() >= 2 && params[1] > 0) ? params[1] - 1 : 0;
        cursor_y_ = std::clamp(row, 0, rows_ - 1);
        cursor_x_ = std::clamp(col, 0, cols_ - 1);
        break;
    }
    case 'J': // Erase in display
        if (p0 == 0) {
            // Clear from cursor to end
            clear_line(cursor_y_, cursor_x_);
            for (int y = cursor_y_ + 1; y < rows_; ++y)
                clear_line(y);
        } else if (p0 == 1) {
            // Clear from start to cursor
            for (int y = 0; y < cursor_y_; ++y)
                clear_line(y);
            clear_line(cursor_y_, 0, cursor_x_ + 1);
        } else if (p0 == 2 || p0 == 3) {
            // Clear entire screen
            for (int y = 0; y < rows_; ++y)
                clear_line(y);
            if (p0 == 3) {
                // Also clear scrollback
                scrollback_.clear();
            }
        }
        break;
    case 'K': // Erase in line
        if (p0 == 0) {
            clear_line(cursor_y_, cursor_x_);
        } else if (p0 == 1) {
            clear_line(cursor_y_, 0, cursor_x_ + 1);
        } else if (p0 == 2) {
            clear_line(cursor_y_);
        }
        break;
    case 'm': // SGR
        process_sgr(params);
        break;
    case 'n': // Device status report
        if (p0 == 6) {
            // Cursor position report
            std::string resp = "\x1B[" + std::to_string(cursor_y_ + 1)
                             + ";" + std::to_string(cursor_x_ + 1) + "R";
            this->write(resp);
        }
        break;
    case 'r': // Set scrolling region (ignored – we do full-screen scroll)
        break;
    case 's': // Save cursor position
        // Stash in a simple pair
        break;
    case 'u': // Restore cursor position
        break;
    case 'L': { // Insert lines
        int count = std::max(p0, 1);
        for (int i = 0; i < count && cursor_y_ < rows_; ++i) {
            // Shift lines down from cursor_y_
            for (int y = rows_ - 1; y > cursor_y_; --y) {
                screen_[y] = std::move(screen_[y - 1]);
            }
            screen_[cursor_y_] = std::vector<TermCell>(cols_);
        }
        break;
    }
    case 'M': { // Delete lines
        int count = std::max(p0, 1);
        for (int i = 0; i < count && cursor_y_ < rows_; ++i) {
            for (int y = cursor_y_; y < rows_ - 1; ++y) {
                screen_[y] = std::move(screen_[y + 1]);
            }
            screen_[rows_ - 1] = std::vector<TermCell>(cols_);
        }
        break;
    }
    case 'P': { // Delete chars
        int count = std::max(p0, 1);
        auto& row = screen_[cursor_y_];
        for (int i = cursor_x_; i < cols_; ++i) {
            if (i + count < cols_) {
                row[i] = row[i + count];
            } else {
                row[i] = TermCell{};
            }
        }
        break;
    }
    case '@': { // Insert chars
        int count = std::max(p0, 1);
        auto& row = screen_[cursor_y_];
        for (int i = cols_ - 1; i >= cursor_x_ + count; --i) {
            row[i] = row[i - count];
        }
        for (int i = cursor_x_; i < std::min(cursor_x_ + count, cols_); ++i) {
            row[i] = TermCell{};
        }
        break;
    }
    case 'h': // Set mode
    case 'l': // Reset mode
        // Private modes (e.g. ?25h = show cursor, ?1049h = alt screen)
        // We acknowledge but don't implement alternate screen buffer
        break;
    case 'd': { // VPA – Vertical line position absolute
        int row = std::max(p0, 1) - 1;
        cursor_y_ = std::clamp(row, 0, rows_ - 1);
        break;
    }
    case 'G': { // CHA – Cursor horizontal absolute
        int col = std::max(p0, 1) - 1;
        cursor_x_ = std::clamp(col, 0, cols_ - 1);
        break;
    }
    case 'X': { // ECH – Erase characters
        int count = std::max(p0, 1);
        for (int i = 0; i < count && cursor_x_ + i < cols_; ++i) {
            screen_[cursor_y_][cursor_x_ + i] = TermCell{};
        }
        break;
    }
    default:
        // Unknown CSI – ignore
        break;
    }
}

// ---------------------------------------------------------------------------
// SGR (Select Graphic Rendition)
// ---------------------------------------------------------------------------

void Terminal::process_sgr(const std::vector<int>& params) {
    if (params.empty() || (params.size() == 1 && params[0] == 0)) {
        // Reset
        current_fg_ = Color::hex(0xcdd6f4);
        current_bg_ = {0, 0, 0, 0};
        bold_ = false;
        return;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int p = params[i];
        switch (p) {
        case 0:
            current_fg_ = Color::hex(0xcdd6f4);
            current_bg_ = {0, 0, 0, 0};
            bold_ = false;
            break;
        case 1:
            bold_ = true;
            break;
        case 22:
            bold_ = false;
            break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            current_fg_ = ansi_color(p - 30 + (bold_ ? 8 : 0));
            break;
        case 38:
            // Extended foreground
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    // 256-color
                    current_fg_ = ansi_color(params[i + 2]);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    // 24-bit
                    current_fg_ = {params[i + 2] / 255.0f,
                                   params[i + 3] / 255.0f,
                                   params[i + 4] / 255.0f, 1.0f};
                    i += 4;
                }
            }
            break;
        case 39:
            current_fg_ = Color::hex(0xcdd6f4);
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            current_bg_ = ansi_color(p - 40);
            break;
        case 48:
            // Extended background
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    current_bg_ = ansi_color(params[i + 2]);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    current_bg_ = {params[i + 2] / 255.0f,
                                   params[i + 3] / 255.0f,
                                   params[i + 4] / 255.0f, 1.0f};
                    i += 4;
                }
            }
            break;
        case 49:
            current_bg_ = {0, 0, 0, 0};
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            current_fg_ = ansi_color(p - 90 + 8);
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            current_bg_ = ansi_color(p - 100 + 8);
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Terminal::render(RenderContext& ctx, Rect bounds) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    ctx.r->add_rect(bounds, ctx.theme->terminal_bg);
    bounds = {bounds.x + 6.0f, bounds.y + 4.0f, bounds.w - 12.0f, bounds.h - 8.0f};

    float cw = ctx.font->char_width();
    float lh = ctx.font->line_height();

    // Recalculate grid size from pixel bounds
    int new_cols = std::max(1, static_cast<int>(bounds.w / cw));
    int new_rows = std::max(1, static_cast<int>(bounds.h / lh));
    if (new_cols != cols_ || new_rows != rows_) {
        // We cannot call resize() here (it takes the lock), so resize inline
        cols_ = new_cols;
        rows_ = new_rows;
        screen_.resize(rows_);
        for (auto& row : screen_) {
            row.resize(cols_, TermCell{});
        }
        cursor_x_ = std::min(cursor_x_, cols_ - 1);
        cursor_y_ = std::min(cursor_y_, rows_ - 1);

        if (master_fd_ >= 0) {
            struct winsize ws{};
            ws.ws_row = static_cast<unsigned short>(rows_);
            ws.ws_col = static_cast<unsigned short>(cols_);
            ioctl(master_fd_, TIOCSWINSZ, &ws);
        }
    }

    // Background
    ctx.r->add_rect(bounds, ctx.theme->terminal_bg);

    // Clamp scroll offset
    int max_scroll = static_cast<int>(scrollback_.size());
    int scroll_lines = std::clamp(static_cast<int>(scroll_y_), 0, max_scroll);

    // Render visible lines
    for (int vy = 0; vy < rows_; ++vy) {
        int src_line = vy - scroll_lines;
        const std::vector<TermCell>* row = nullptr;

        if (src_line < 0) {
            // Drawing from scrollback
            int sb_idx = static_cast<int>(scrollback_.size()) + src_line;
            if (sb_idx >= 0 && sb_idx < static_cast<int>(scrollback_.size())) {
                row = &scrollback_[sb_idx];
            }
        } else if (src_line < rows_) {
            row = &screen_[src_line];
        }

        if (!row) continue;

        float py = bounds.y + vy * lh;
        int row_cols = static_cast<int>(row->size());

        for (int x = 0; x < std::min(cols_, row_cols); ++x) {
            const auto& cell = (*row)[x];
            float px = bounds.x + x * cw;

            // Cell background (only if non-transparent)
            if (cell.bg.a > 0.001f) {
                ctx.r->add_rect({px, py, cw, lh}, cell.bg);
            }

            // Character
            if (cell.ch > ' ') {
                ctx.r->add_char(px, py + ctx.font->ascender(), cell.ch, cell.fg, *ctx.font);
            }
        }
    }

    // Cursor (inverted block)
    if (scroll_lines == 0 && focused_ && alive_) {
        float cx = bounds.x + cursor_x_ * cw;
        float cy = bounds.y + cursor_y_ * lh;
        ctx.r->add_rect({cx, cy, cw, lh}, ctx.theme->cursor_color);

        // Draw the character under cursor with inverted color
        if (cursor_y_ >= 0 && cursor_y_ < rows_ &&
            cursor_x_ >= 0 && cursor_x_ < cols_) {
            auto& cell = screen_[cursor_y_][cursor_x_];
            if (cell.ch > ' ') {
                ctx.r->add_char(cx, cy + ctx.font->ascender(), cell.ch,
                                   ctx.theme->terminal_bg, *ctx.font);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

bool Terminal::handle_key(const KeyEvent& e) {
    if (!e.pressed || !alive_) return false;

    bool ctrl = e.mods & KeyMod::Ctrl;
    bool shift = e.mods & KeyMod::Shift;

    // Ctrl + letter -> send control code
    if (ctrl && e.keycode >= 'a' && e.keycode <= 'z') {
        char code = static_cast<char>(e.keycode & 0x1f);
        write(std::string(1, code));
        return true;
    }

    std::string seq;

    switch (e.keycode) {
    case SDLK_RETURN:
        seq = "\r";
        break;
    case SDLK_BACKSPACE:
        seq = "\x7f";
        break;
    case SDLK_TAB:
        seq = "\t";
        break;
    case SDLK_ESCAPE:
        seq = "\x1b";
        break;
    case SDLK_UP:
        seq = "\x1b[A";
        break;
    case SDLK_DOWN:
        seq = "\x1b[B";
        break;
    case SDLK_RIGHT:
        seq = "\x1b[C";
        break;
    case SDLK_LEFT:
        seq = "\x1b[D";
        break;
    case SDLK_HOME:
        seq = "\x1b[H";
        break;
    case SDLK_END:
        seq = "\x1b[F";
        break;
    case SDLK_DELETE:
        seq = "\x1b[3~";
        break;
    case SDLK_PAGEUP:
        if (shift) {
            // Scroll through scrollback
            scroll_y_ = std::min(scroll_y_ + static_cast<float>(rows_),
                                 static_cast<float>(scrollback_.size()));
            return true;
        }
        seq = "\x1b[5~";
        break;
    case SDLK_PAGEDOWN:
        if (shift) {
            scroll_y_ = std::max(scroll_y_ - static_cast<float>(rows_), 0.0f);
            return true;
        }
        seq = "\x1b[6~";
        break;
    default:
        return false;
    }

    if (!seq.empty()) {
        // Reset scroll position when user types
        scroll_y_ = 0;
        write(seq);
        return true;
    }
    return false;
}

bool Terminal::handle_text(const std::string& text) {
    if (!alive_) return false;
    scroll_y_ = 0;
    write(text);
    return true;
}

} // namespace oh
