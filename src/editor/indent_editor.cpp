// Editor side of structural indentation: auto-indent after edits, the reindent
// commands, smart Enter / Tab, and the virtual-whitespace column mapping.
//
// Virtual whitespace keeps the buffer untouched. For a line whose computed indent is
// `vi` columns and whose leading whitespace is `ws` bytes, byte column c is shown at
//     c <= ws : display column vi          (the whole indent collapses onto one spot)
//     c >  ws : vi + width(text[ws, c))
// so the line reads as indent_string(vi) + text[ws..]. Carets are kept at c >= ws
// (virtual_normalize); copy emits the computed indent; Backspace at the indent and
// Delete at a line end join lines without leaving stray whitespace behind (also in
// auto mode).
#include "editor/editor.h"
#include "editor/utf8.h"
#include <algorithm>

namespace oh {

void Editor::indent_sync() const {
    if (indent_.language() != syntax_.language_id() || !indent_.has_classifier()) {
        indent_.set_language(syntax_.language_id());
        indent_.set_classifier([this](size_t byte) {
            if (syntax_.parsing()) return BraceKind::Unknown;
            auto [parent, grandparent] = syntax_.brace_context(byte);
            return brace_kind_from_tree(parent, grandparent);
        });
    }
    // Changed without going through indent_consume_dirty (a reload): start over.
    if (buffer_.version() != indent_version_ || buffer_.loads() != indent_loads_) {
        indent_.invalidate_all();
        indent_version_ = buffer_.version();
        indent_loads_ = buffer_.loads();
    }
}

void Editor::indent_consume_dirty(std::vector<size_t>* lines) {
    // Edits are covered by the dirty ranges; only a reload needs a full recompute.
    bool known = indent_loads_ == buffer_.loads();
    if (known) indent_version_ = buffer_.version();
    auto dirty = buffer_.take_dirty();
    size_t first = SIZE_MAX, len = buffer_.length();
    for (auto [lo, hi] : dirty) {
        size_t a = buffer_.position_of(std::min(lo, len)).line, b = buffer_.position_of(std::min(hi, len)).line;
        first = std::min(first, a);
        if (lines)
            for (size_t l = a; l <= b; l++) lines->push_back(l);
    }
    indent_sync();  // language; a version mismatch invalidates everything
    if (known && first != SIZE_MAX) indent_.invalidate_from(first);
    indent_version_ = buffer_.version();
}

int Editor::computed_indent(size_t line) const {
    indent_sync();
    return indent_.indent(buffer_, line);
}

bool Editor::indent_supported() const {
    indent_sync();
    return indent_.supported();
}

bool Editor::virtual_ws() const {
    return IndentSettings::mode == IndentMode::Virtual && indent_supported();
}

size_t Editor::view_col(size_t line, const std::string& text, size_t byte) const {
    if (!virtual_ws()) return utf8::display_col(text, byte);
    int vi = computed_indent(line);
    if (vi == IndentEngine::KEEP) return utf8::display_col(text, byte);
    size_t ws = leading_ws(text);
    size_t d = static_cast<size_t>(vi);
    for (size_t i = ws; i < byte && i < text.size(); i = utf8::next_boundary(text, i)) d += utf8::cell_width(text[i], d);
    return d;
}

size_t Editor::view_byte(size_t line, const std::string& text, size_t dcol) const {
    if (!virtual_ws()) return utf8::byte_col_at(text, dcol);
    int vi = computed_indent(line);
    if (vi == IndentEngine::KEEP) return utf8::byte_col_at(text, dcol);
    size_t ws = leading_ws(text);
    size_t d = static_cast<size_t>(vi), i = ws;
    if (dcol <= d) return ws;
    while (i < text.size()) {
        size_t w = utf8::cell_width(text[i], d);
        if (d + w > dcol) return (dcol - d) * 2 >= w ? utf8::next_boundary(text, i) : i;
        d += w;
        i = utf8::next_boundary(text, i);
    }
    return i;
}

void Editor::set_selections(const std::vector<Selection>& sels) {
    if (sels.empty()) return;
    size_t pref = cursors_.preferred_col();
    cursors_.set_primary(sels[0].anchor);
    cursors_.extend_primary(sels[0].head);
    for (size_t i = 1; i < sels.size(); i++) cursors_.add_selection(sels[i]);
    cursors_.set_preferred_col(pref);
}

// Rewrites the leading whitespace of `lines` to their computed indent (inside the
// caller's undo group). Blank lines become empty unless a caret sits on them.
size_t Editor::reindent_lines(std::vector<size_t> lines) {
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    struct Fix { size_t line, ws; std::string want; };
    std::vector<Fix> fixes;
    const auto& sels = cursors_.selections();
    for (size_t l : lines) {
        if (l >= buffer_.line_count()) break;
        int ind = computed_indent(l);
        if (ind == IndentEngine::KEEP) continue;
        std::string text = buffer_.line_text(l);
        size_t ws = leading_ws(text);
        bool blank = ws == text.size();
        bool caret = std::any_of(sels.begin(), sels.end(), [&](const Selection& s) { return s.head.line == l; });
        std::string want = blank && !caret ? std::string() : indent_string(ind);
        if (text.compare(0, ws, want) != 0) fixes.push_back({l, ws, std::move(want)});
    }
    if (fixes.empty()) return 0;
    for (auto it = fixes.rbegin(); it != fixes.rend(); ++it)  // bottom-up keeps offsets valid
        buffer_.replace(buffer_.line_start(it->line), it->ws, it->want);

    std::vector<Selection> moved = sels;
    for (Selection& s : moved) {
        bool empty = s.empty();
        for (Position* p : {&s.anchor, &s.head}) {
            auto f = std::lower_bound(fixes.begin(), fixes.end(), p->line, [](const Fix& a, size_t l) { return a.line < l; });
            if (f == fixes.end() || f->line != p->line) continue;
            if (p->col >= f->ws) p->col = p->col - f->ws + f->want.size();
            else p->col = (p->col == 0 && !empty) ? 0 : f->want.size();  // a caret in the indent goes to its end
        }
    }
    set_selections(moved);
    return fixes.size();
}

void Editor::indent_after_edit() {
    std::vector<size_t> lines;
    indent_consume_dirty(&lines);
    if (indent_hold_ || IndentSettings::mode != IndentMode::Auto || lines.empty() || !indent_.supported()) return;
    if (!buffer_.resume_undo_group()) buffer_.begin_undo_group();  // undo together with the edit
    reindent_lines(std::move(lines));
    buffer_.end_undo_group();
    indent_consume_dirty(nullptr);
    sync_syntax();
}

size_t Editor::reindent_selection() {
    std::vector<size_t> lines;
    for (const Selection& s : cursors_.selections()) {
        size_t a = s.min().line, b = s.max().line;
        if (s.max().col == 0 && b > a) --b;
        for (size_t l = a; l <= b; l++) lines.push_back(l);
    }
    indent_hold_ = true;
    buffer_.begin_undo_group();
    size_t n = reindent_lines(std::move(lines));
    buffer_.end_undo_group();
    on_text_changed();
    indent_hold_ = false;
    return n;
}

size_t Editor::reindent_file() {
    std::vector<size_t> lines(buffer_.line_count());
    for (size_t l = 0; l < lines.size(); l++) lines[l] = l;
    indent_hold_ = true;
    buffer_.begin_undo_group();
    size_t n = reindent_lines(std::move(lines));
    buffer_.end_undo_group();
    on_text_changed();
    indent_hold_ = false;
    return n;
}

void Editor::before_save() {
    if (IndentSettings::mode == IndentMode::Virtual && IndentSettings::write_on_save && indent_supported())
        reindent_file();
}

bool Editor::indent_newline() {
    if (IndentSettings::mode == IndentMode::Off || !indent_supported()) return false;
    const auto& sels = cursors_.selections();
    if (sels.size() == 1 && sels[0].empty()) {
        // Enter between a bracket pair opens an indented empty line between them.
        Position p = sels[0].head;
        std::string text = buffer_.line_text(p.line);
        if (p.col > 0 && p.col < text.size()) {
            char a = text[p.col - 1], b = text[p.col];
            if ((a == '{' && b == '}') || (a == '(' && b == ')') || (a == '[' && b == ']')) {
                buffer_.begin_undo_group();
                buffer_.insert(buffer_.offset_of(p.line, p.col), "\n\n");
                buffer_.end_undo_group();
                cursors_.set_primary({p.line + 1, 0});
                on_text_changed();  // auto: indents the caret line and the closer
                return true;
            }
        }
    }
    // No copied indentation: auto mode indents the new line, virtual mode draws it.
    insert_text("\n");
    return true;
}

bool Editor::indent_tab() {
    if (IndentSettings::mode == IndentMode::Off || !indent_supported()) return false;
    const auto& sels = cursors_.selections();
    bool selection = std::any_of(sels.begin(), sels.end(), [](const Selection& s) { return !s.empty(); });
    bool at_indent = std::all_of(sels.begin(), sels.end(), [&](const Selection& s) {
        return s.head.col <= leading_ws(buffer_.line_text(s.head.line));
    });
    if (!selection && !at_indent) return false;  // mid-line Tab inserts spaces (alignment)
    if (IndentSettings::mode == IndentMode::Auto || selection) reindent_selection();
    return true;
}

// Auto and virtual modes: indentation is not typed, so deleting inside it means
// joining lines (in auto mode a deleted space would just be re-indented back).
void Editor::virtual_extend_delete(bool backward) {
    if (IndentSettings::mode == IndentMode::Off || !indent_supported()) return;
    std::vector<Selection> sels = cursors_.selections();
    bool changed = false;
    for (Selection& s : sels) {
        if (!s.empty()) continue;
        Position h = s.head;
        if (computed_indent(h.line) == IndentEngine::KEEP) continue;
        std::string text = buffer_.line_text(h.line);
        size_t ws = leading_ws(text);
        if (backward && h.col > 0 && h.col <= ws) {
            // Backspace at the (invisible) indent joins with the previous line.
            s.anchor = h.line > 0 ? Position{h.line - 1, buffer_.line_length(h.line - 1)} : Position{0, 0};
            s.head = {h.line, ws};
            changed = true;
        } else if (!backward && h.col == text.size() && h.line + 1 < buffer_.line_count() &&
                   computed_indent(h.line + 1) != IndentEngine::KEEP) {
            // Delete at a line end also removes the next line's leading whitespace.
            s.head = {h.line + 1, leading_ws(buffer_.line_text(h.line + 1))};
            changed = true;
        }
    }
    if (changed) set_selections(sels);
}

std::string Editor::virtual_copy(bool& line_mode) const {
    const auto& sels = cursors_.selections();
    line_mode = std::all_of(sels.begin(), sels.end(), [](const Selection& s) { return s.empty(); });
    // Text of line `l` between byte columns, with the leading whitespace replaced by the
    // computed indent when the range starts at the line start.
    auto piece = [&](size_t l, size_t from, size_t to) {
        std::string text = buffer_.line_text(l);
        to = std::min(to, text.size());
        from = std::min(from, to);
        int vi = computed_indent(l);
        size_t ws = leading_ws(text);
        if (vi == IndentEngine::KEEP || from > ws) return text.substr(from, to - from);
        if (to <= ws) return std::string();  // only (invisible) whitespace selected
        return (from == 0 ? indent_string(vi) : std::string()) + text.substr(ws, to - ws);
    };
    std::string out;
    for (size_t i = 0; i < sels.size(); i++) {
        if (i) out += '\n';
        if (line_mode) {
            out += piece(sels[i].head.line, 0, SIZE_MAX) + "\n";
            continue;
        }
        Position mn = sels[i].min(), mx = sels[i].max();
        for (size_t l = mn.line; l <= mx.line; l++) {
            out += piece(l, l == mn.line ? mn.col : 0, l == mx.line ? mx.col : SIZE_MAX);
            if (l != mx.line) out += '\n';
        }
    }
    if (line_mode && sels.size() > 1) out.pop_back();
    return out;
}

void Editor::virtual_normalize(const std::vector<Selection>* before, Nav nav, size_t pref_before) {
    if (!virtual_ws()) return;
    std::vector<Selection> sels = cursors_.selections();
    bool changed = false;
    // Vertical moves keep the on-screen column (the cursor set measures real columns).
    if (nav == Nav::Vertical && before && before->size() == sels.size() && !before->empty()) {
        if (pref_before == SIZE_MAX) {
            const Position& h = (*before)[0].head;
            virtual_pref_ = view_col(h.line, buffer_.line_text(h.line), h.col);
        }
        for (size_t i = 0; i < sels.size(); i++) {
            Selection& s = sels[i];
            if (s.head.line == (*before)[i].head.line) continue;
            bool empty = s.empty();
            s.head.col = view_byte(s.head.line, buffer_.line_text(s.head.line), virtual_pref_);
            if (empty) s.anchor = s.head;
            changed = true;
        }
    }
    for (size_t i = 0; i < sels.size(); i++) {
        Selection& s = sels[i];
        bool empty = s.empty();
        auto ws_of = [&](size_t line) -> size_t {
            if (computed_indent(line) == IndentEngine::KEEP) return 0;
            return leading_ws(buffer_.line_text(line));
        };
        size_t ws = ws_of(s.head.line);
        if (s.head.col < ws) {
            // Stepping left out of the indent continues on the previous line.
            bool left = nav == Nav::Other && before && i < before->size() && (*before)[i].head.line == s.head.line &&
                        (*before)[i].head.col >= ws && s.head.line > 0;
            if (left) s.head = {s.head.line - 1, buffer_.line_length(s.head.line - 1)};
            else s.head.col = ws;
            changed = true;
        }
        if (empty) {
            s.anchor = s.head;
        } else if (s.anchor.col > 0 && s.anchor.col < ws_of(s.anchor.line)) {
            s.anchor.col = ws_of(s.anchor.line);  // column 0 stays: whole-line selections
            changed = true;
        }
    }
    if (changed) set_selections(sels);
}

} // namespace oh
