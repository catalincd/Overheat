#pragma once
#include "ui/panel.h"
#include "ui/markdown.h"
#include "editor/syntax.h"
#include <string>
#include <vector>

namespace oh {

// LSP hover tooltip: markdown (markdown.h) laid out in the editor font, with code
// blocks coloured by the tree-sitter highlighter. Capped size; scrolls with the wheel.
class HoverPopup {
public:
    // anchor: screen rect of the hovered word; default_lang: the file's language id,
    // used for fenced blocks without an info string.
    void show(const std::string& markdown, Rect anchor, const std::string& default_lang);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }
    Rect rect() const { return rect_; }
    Rect anchor() const { return anchor_; }
    // Pointer inside the popup, the anchor, or the gap between them (moving into
    // the popup must not dismiss it).
    bool keeps_open(float x, float y) const;
    bool scroll(float lines);  // false when not scrollable
    void render(RenderContext& ctx, Rect bounds);
    size_t block_count() const { return blocks_.size(); }
    const std::string& text() const { return markdown_; }

private:
    struct Run {
        std::string text;
        md::Style style = md::Style::Normal;
        int token = -1;  // TokenType for code-block text, -1 otherwise
        float x = 0;     // in cells from the content left edge
    };
    struct Line {
        std::vector<Run> runs;
        enum Kind { Text, Code, Rule, Gap } kind = Text;
        bool heading = false;
    };
    void layout(size_t cols);
    // Per code block: token type per byte of each line.
    std::vector<std::vector<std::vector<int>>> code_tokens_;

    std::string markdown_;
    std::vector<md::Block> blocks_;
    std::vector<Line> lines_;
    size_t layout_cols_ = 0;
    std::string lang_;
    Rect anchor_{}, rect_{};
    float scroll_ = 0, max_scroll_ = 0;
    bool visible_ = false;
};

} // namespace oh
