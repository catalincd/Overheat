#include "ui/terminal.h"
#include "renderer/font_atlas.h"
#include "ui/cursor_fx.h"
#include "renderer/renderer2d.h"
#include "renderer/vulkan_context.h"

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

bool Terminal::spawn(const std::vector<std::string>& argv, const fs::path& cwd) {
    kill();  // also reaps a previous session that ended by itself
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (auto& row : screen_) std::fill(row.begin(), row.end(), TermCell{});
        cursor_x_ = cursor_y_ = 0;
        current_fg_ = current_bg_ = termcolor::DEFAULT;
        parse_state_ = ParseState::Normal;
        bracketed_paste_ = false;
    }

    if (!pty_.spawn(argv, cwd, cols_, rows_)) return false;
    alive_ = true;
    reader_thread_ = std::thread([this]() {
        char buf[4096];
        while (alive_) {
            long n = pty_.read(buf, sizeof(buf), 20);  // short timeout so kill() can join promptly
            if (n > 0) {
                process_output(buf, static_cast<size_t>(n));
                request_redraw();
            } else if (n < 0) {
                alive_ = false;  // the shell exited
                request_redraw();
            }
        }
    });
    return true;
}

void Terminal::kill() {
    alive_ = false;
    // Join before closing: the reader must not touch a closed (or reused) handle.
    if (reader_thread_.joinable()) reader_thread_.join();
    pty_.kill();
}

// ---------------------------------------------------------------------------
// Write to PTY
// ---------------------------------------------------------------------------

void Terminal::paste(const std::string& text) {
    std::string data;
    data.reserve(text.size() + 12);
    if (bracketed_paste_) data += "\x1b[200~";
    for (char c : text) {
        if (c == '\r') continue;
        data.push_back(c == '\n' ? '\r' : c);
    }
    if (bracketed_paste_) data += "\x1b[201~";
    write(data);
}

void Terminal::write(const std::string& data) {
    if (alive_) pty_.write(data.data(), data.size());
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

    pty_.resize(cols_, rows_);
}

// ---------------------------------------------------------------------------
// Update (called each frame)
// ---------------------------------------------------------------------------

void Terminal::update(float /*dt*/) {
    if (alive_ && pty_.exited()) alive_ = false;
}

// ---------------------------------------------------------------------------
// ANSI color palette (Catppuccin Mocha)
// ---------------------------------------------------------------------------

Color Terminal::resolve(uint32_t code, const Theme& th, bool foreground) {
    uint32_t kind = code & 0xFF000000u;
    if (kind == termcolor::RGB)
        return {((code >> 16) & 0xFF) / 255.0f, ((code >> 8) & 0xFF) / 255.0f, (code & 0xFF) / 255.0f, 1.0f};
    if (kind == termcolor::INDEXED) {
        int idx = static_cast<int>(code & 0xFF);
        if (idx < 16) return th.ansi[idx];
        if (idx < 232) {  // 6x6x6 cube
            int ci = idx - 16;
            auto comp = [](int v) { return v == 0 ? 0.0f : (55.0f + 40.0f * v) / 255.0f; };
            return {comp(ci / 36), comp((ci / 6) % 6), comp(ci % 6), 1.0f};
        }
        float v = (8.0f + 10.0f * (idx - 232)) / 255.0f;  // grayscale ramp
        return {v, v, v, 1.0f};
    }
    return foreground ? th.terminal_fg : Color{0, 0, 0, 0};
}

// ---------------------------------------------------------------------------
// Screen manipulation helpers
// ---------------------------------------------------------------------------

void Terminal::scroll_up() {
    // Top line goes to scrollback
    scrollback_.push_back(std::move(screen_[0]));
    if (static_cast<int>(scrollback_.size()) > MAX_SCROLLBACK) {
        scrollback_.pop_front();
        dropped_++;
    }
    if (scroll_y_ > 0) scroll_y_ += 1;  // keep a scrolled-back view anchored on its content
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
            current_fg_ = termcolor::DEFAULT;
            current_bg_ = termcolor::DEFAULT;
            bold_ = false;
            cursor_x_ = 0;
            cursor_y_ = 0;
            for (int y = 0; y < rows_; ++y)
                clear_line(y);
            parse_state_ = ParseState::Normal;
        } else if (uc == '7') {  // DECSC
            saved_x_ = cursor_x_;
            saved_y_ = cursor_y_;
            saved_fg_ = current_fg_;
            saved_bg_ = current_bg_;
            parse_state_ = ParseState::Normal;
        } else if (uc == '8') {  // DECRC
            cursor_x_ = std::clamp(saved_x_, 0, cols_ - 1);
            cursor_y_ = std::clamp(saved_y_, 0, rows_ - 1);
            current_fg_ = saved_fg_;
            current_bg_ = saved_bg_;
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
            // ST is ESC '\': the Escape state swallows the backslash.
            parse_state_ = ParseState::Escape;
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
        if (is_private) {
            for (int mode : params)
                if (mode == 2004) bracketed_paste_ = (final_char == 'h');
        }
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
        current_fg_ = termcolor::DEFAULT;
        current_bg_ = termcolor::DEFAULT;
        bold_ = false;
        return;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int p = params[i];
        switch (p) {
        case 0:
            current_fg_ = termcolor::DEFAULT;
            current_bg_ = termcolor::DEFAULT;
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
            current_fg_ = termcolor::indexed(p - 30 + (bold_ ? 8 : 0));
            break;
        case 38:
            // Extended foreground
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    // 256-color
                    current_fg_ = termcolor::indexed(params[i + 2]);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    // 24-bit
                    current_fg_ = termcolor::rgb(params[i + 2], params[i + 3], params[i + 4]);
                    i += 4;
                }
            }
            break;
        case 39:
            current_fg_ = termcolor::DEFAULT;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            current_bg_ = termcolor::indexed(p - 40);
            break;
        case 48:
            // Extended background
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    current_bg_ = termcolor::indexed(params[i + 2]);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    current_bg_ = termcolor::rgb(params[i + 2], params[i + 3], params[i + 4]);
                    i += 4;
                }
            }
            break;
        case 49:
            current_bg_ = termcolor::DEFAULT;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            current_fg_ = termcolor::indexed(p - 90 + 8);
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            current_bg_ = termcolor::indexed(p - 100 + 8);
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

        pty_.resize(cols_, rows_);
    }

    // Background
    ctx.r->add_rect(bounds, ctx.theme->terminal_bg);
    inner_ = bounds;
    cell_w_ = cw;
    cell_h_ = lh;

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
        if (has_selection_) {
            int64_t abs = static_cast<int64_t>(dropped_ + scrollback_.size()) + src_line;
            for (int x = 0; x < std::min(cols_, row_cols); ++x)
                if (selected(abs, x)) ctx.r->add_rect({bounds.x + x * cw, py, cw, lh}, ctx.theme->selection);
        }

        for (int x = 0; x < std::min(cols_, row_cols); ++x) {
            const auto& cell = (*row)[x];
            float px = bounds.x + x * cw;

            // Cell background (only if non-transparent)
            if (cell.bg != termcolor::DEFAULT) {
                ctx.r->add_rect({px, py, cw, lh}, resolve(cell.bg, *ctx.theme, false));
            }

            // Character
            if (cell.ch > ' ') {
                ctx.r->add_char(px, py + ctx.font->ascender(), cell.ch,
                                resolve(cell.fg, *ctx.theme, true), *ctx.font);
            }
        }
    }

    // Cursor (inverted block)
    if (scroll_lines == 0 && focused_ && alive_) {
        float cx = bounds.x + cursor_x_ * cw;
        float cy = bounds.y + cursor_y_ * lh;
        // While the animated cursor (cursor_fx.cpp) is in flight it draws the block.
        bool draw_self = CursorFx::get().report(0, this, {cx, cy, cw, lh}, bounds, bounds.x, bounds.y,
                                                ctx.theme->cursor_color, cw);
        if (draw_self) ctx.r->add_rect({cx, cy, cw, lh}, ctx.theme->cursor_color);

        // Draw the character under cursor with inverted color
        if (draw_self && cursor_y_ >= 0 && cursor_y_ < rows_ &&
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

Terminal::CellPos Terminal::cell_at(float x, float y) const {
    int scroll_lines = std::clamp(static_cast<int>(scroll_y_), 0, static_cast<int>(scrollback_.size()));
    int vy = std::clamp(static_cast<int>((y - inner_.y) / cell_h_), 0, rows_ - 1);
    int col = std::clamp(static_cast<int>((x - inner_.x) / cell_w_ + 0.5f), 0, cols_);
    return {static_cast<int64_t>(dropped_ + scrollback_.size()) + vy - scroll_lines, col};
}

const std::vector<TermCell>* Terminal::row_at(int64_t abs_line) const {
    int64_t idx = abs_line - static_cast<int64_t>(dropped_);
    if (idx < 0) return nullptr;
    if (idx < static_cast<int64_t>(scrollback_.size())) return &scrollback_[static_cast<size_t>(idx)];
    idx -= static_cast<int64_t>(scrollback_.size());
    return idx < rows_ ? &screen_[static_cast<size_t>(idx)] : nullptr;
}

bool Terminal::selected(int64_t line, int col) const {
    CellPos a = sel_anchor_, b = sel_head_;
    if (b.line < a.line || (b.line == a.line && b.col < a.col)) std::swap(a, b);
    if (line < a.line || line > b.line) return false;
    if (line == a.line && col < a.col) return false;
    if (line == b.line && col >= b.col) return false;
    return true;
}

bool Terminal::handle_mouse(const MouseEvent& e) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (e.scroll_y != 0.0f) {
        scroll_y_ = std::clamp(scroll_y_ + e.scroll_y * 3.0f, 0.0f, static_cast<float>(scrollback_.size()));
        return true;
    }
    if (e.button != 1) return false;
    if (e.motion) {
        if (selecting_) {
            sel_head_ = cell_at(e.x, e.y);
            has_selection_ = sel_head_.line != sel_anchor_.line || sel_head_.col != sel_anchor_.col;
        }
        return true;
    }
    if (!e.pressed) {
        selecting_ = false;
        return true;
    }
    CellPos p = cell_at(e.x, e.y);
    if (e.double_click) {  // select the word under the pointer
        const auto* row = row_at(p.line);
        if (!row) return true;
        auto word = [&](int c) { return c >= 0 && c < static_cast<int>(row->size()) && (*row)[static_cast<size_t>(c)].ch > ' '; };
        int a = std::min(p.col, static_cast<int>(row->size()) - 1), b = a;
        while (word(a - 1)) a--;
        while (word(b)) b++;
        sel_anchor_ = {p.line, a};
        sel_head_ = {p.line, b};
        has_selection_ = b > a;
        selecting_ = false;
        return true;
    }
    sel_anchor_ = sel_head_ = p;
    has_selection_ = false;
    selecting_ = true;
    return true;
}

std::string Terminal::selection_text() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!has_selection_) return {};
    CellPos a = sel_anchor_, b = sel_head_;
    if (b.line < a.line || (b.line == a.line && b.col < a.col)) std::swap(a, b);
    std::string out;
    auto put = [&](uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    };
    for (int64_t line = a.line; line <= b.line; line++) {
        const auto* row = row_at(line);
        if (!row) continue;
        int c0 = line == a.line ? a.col : 0;
        int c1 = line == b.line ? b.col : static_cast<int>(row->size());
        std::string piece;
        std::swap(piece, out);
        for (int c = c0; c < c1 && c < static_cast<int>(row->size()); c++) put((*row)[static_cast<size_t>(c)].ch);
        std::swap(piece, out);
        while (!piece.empty() && piece.back() == ' ') piece.pop_back();  // trailing cell padding
        out += piece;
        if (line != b.line) out += '\n';
    }
    return out;
}

bool Terminal::handle_text(const std::string& text) {
    if (!alive_) return false;
    scroll_y_ = 0;
    has_selection_ = false;
    write(text);
    return true;
}

} // namespace oh
