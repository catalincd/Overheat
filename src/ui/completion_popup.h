#pragma once
#include "ui/panel.h"
#include "editor/editor.h"
#include "lsp/client.h"

namespace oh {

// Code completion list anchored at the start of the word being typed.
class CompletionPopup {
public:
    void show(std::vector<LSPCompletion> items, Position anchor);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }
    const Position& anchor() const { return anchor_; }

    // Re-filter against the text between the anchor and the cursor; hides when it no longer applies.
    void refilter(const Editor& ed);
    // Keys: true if consumed (navigation / accept / dismiss).
    bool handle_key(Editor& ed, const KeyEvent& e);
    void render(RenderContext& ctx, float x, float line_top, float line_h, Rect clip);
    bool handle_mouse(Editor& ed, const MouseEvent& e);

private:
    std::vector<LSPCompletion> all_;
    std::vector<int> shown_;
    Position anchor_{};
    int selected_ = 0, first_ = 0;
    bool visible_ = false;
    Rect rect_{};
    float row_h_ = 22.0f;
    static constexpr int MAX_ROWS = 10;

    void accept(Editor& ed);
};

} // namespace oh
