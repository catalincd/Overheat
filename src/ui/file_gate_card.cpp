// The card an EditorView shows instead of text for a binary or very large file
// (see filecheck::classify and App::open_file).
#include "ui/editor_view.h"
#include "renderer/renderer2d.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>

namespace oh {

namespace {

// Greedy word wrap by character count (the UI font is monospace).
std::vector<std::string> wrap_words(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > cols) {
            out.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) out.push_back(line);
    return out;
}

} // namespace

bool EditorView::gated() const {
    return editor_ && editor_->gate() != filecheck::Verdict::Open;
}

void EditorView::render_gate_card(RenderContext& ctx, Rect bounds) {
    using filecheck::Verdict;
    const Theme& th = *ctx.theme;
    const FontAtlas& font = *ctx.font;
    float cw = font.char_width(), lh = font.line_height();
    ctx.r->add_rect(bounds, th.bg);
    gate_button_ = {};

    std::string name = editor_->buffer().file_path().filename().string();
    std::string size = filecheck::human_size(editor_->file_size());
    std::string title, body, button;
    uint32_t icon = 0xEA6C;  // nf-cod-warning
    switch (editor_->gate()) {
        case Verdict::Binary:
            icon = 0xEAE8;  // nf-cod-file_binary
            title = "Binary file";
            body = name + " (" + size + ") looks binary, so it is not shown as text. "
                   "It opens read-only; invalid characters show as \xEF\xBF\xBD.";
            button = "Open Anyway";
            break;
        case Verdict::AskLarge:
            title = "Large file (" + size + ")";
            body = name + " is large and may be slow to open. It opens in large-file mode: "
                   "no syntax highlighting, language server, minimap, word wrap or git blame.";
            button = "Open Anyway";
            break;
        default:
            title = "File too large (" + size + ")";
            body = name + " is over the " + filecheck::human_size(filecheck::HARD_CAP) +
                   " limit and cannot be opened in the editor.";
            break;
    }

    float card_w = std::min(560.0f, bounds.w - 32.0f);
    if (card_w < 120.0f) return;
    size_t cols = static_cast<size_t>(std::max(10.0f, (card_w - 48.0f) / cw));
    auto lines = wrap_words(body, cols);
    float btn_h = std::round(lh + 14.0f);
    float card_h = 24.0f + lh + 14.0f + static_cast<float>(lines.size()) * lh + 24.0f +
                   (button.empty() ? 0.0f : btn_h + 20.0f);
    float x = std::floor(bounds.x + (bounds.w - card_w) * 0.5f);
    float y = std::floor(bounds.y + std::max(16.0f, (bounds.h - card_h) * 0.4f));
    Rect card{x, y, card_w, card_h};
    ctx.r->add_rect(card, th.surface, 10.0f);
    ctx.r->add_rect({card.x, card.y + 12.0f, 3.0f, card.h - 24.0f},
                    editor_->gate() == Verdict::TooLarge ? th.error : th.warning, 1.5f);

    float tx = x + 24.0f, ty = y + 24.0f;
    if (const GlyphInfo* gi = font.glyph(icon); gi && gi->width > 0) {
        ctx.r->add_glyph(tx, ty + font.ascender(), gi, th.fg_muted);
        tx += cw * 2.0f;
    }
    ctx.r->add_text(tx, ty + font.ascender(), title, th.fg, font);
    ty += lh + 14.0f;
    for (auto& l : lines) {
        ctx.r->add_text(x + 24.0f, ty + font.ascender(), l, th.fg_muted, font);
        ty += lh;
    }
    if (!button.empty()) {
        ty += 20.0f;
        float bw = static_cast<float>(button.size()) * cw + 32.0f;
        gate_button_ = {x + 24.0f, ty, bw, btn_h};
        bool hover = gate_button_.contains(ctx.mouse_x, ctx.mouse_y);
        Color bg = th.accent;
        if (hover) bg.a *= 0.85f;
        ctx.r->add_rect(gate_button_, bg, 6.0f);
        ctx.r->add_text(gate_button_.x + 16.0f, ty + (btn_h - lh) * 0.5f + font.ascender(), button,
                        th.dark ? th.bg : Color::hex(0xffffff), font);
        std::string hint = "or press Enter";
        ctx.r->add_text(gate_button_.x + bw + 14.0f, ty + (btn_h - lh) * 0.5f + font.ascender(),
                        hint, th.fg_muted, font);
    }
}

bool EditorView::gate_key(const KeyEvent& e) {
    if (e.pressed && (e.keycode == SDLK_RETURN || e.keycode == SDLK_KP_ENTER) &&
        editor_->gate() != filecheck::Verdict::TooLarge && on_open_anyway_)
        on_open_anyway_(*editor_);
    return true;  // nothing to edit yet: swallow everything
}

bool EditorView::gate_mouse(const MouseEvent& e) {
    if (e.button == 1 && e.pressed && !e.motion && gate_button_.contains(e.x, e.y) && on_open_anyway_)
        on_open_anyway_(*editor_);
    return true;
}

} // namespace oh
