#include "ui/markdown.h"
#include <cctype>
#include <cstring>
#include <algorithm>

namespace oh::md {

namespace {

bool is_punct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

size_t indent_of(const std::string& s) {
    size_t n = 0;
    for (char c : s) {
        if (c == ' ') n++;
        else if (c == '\t') n += 4 - n % 4;
        else break;
    }
    return n;
}

void push(std::vector<Span>& out, std::string text, Style st) {
    if (text.empty()) return;
    if (!out.empty() && out.back().style == st && out.back().text != "\n" && text != "\n")
        out.back().text += text;
    else
        out.push_back({std::move(text), st});
}

// Decodes the few HTML entities servers emit.
bool entity(const std::string& t, size_t& i, std::string& out) {
    static const struct { const char* name; const char* text; } ents[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}};
    for (auto& e : ents) {
        size_t n = std::strlen(e.name);
        if (t.compare(i, n, e.name) == 0) {
            out += e.text;
            i += n;
            return true;
        }
    }
    return false;
}

void inline_into(const std::string& t, Style outer, std::vector<Span>& out);

// Closing delimiter run of `delim` (exactly `n` chars) after `from`, skipping code spans.
size_t find_closing(const std::string& t, size_t from, char delim, size_t n) {
    for (size_t j = from; j < t.size(); j++) {
        if (t[j] == '\\') { j++; continue; }
        if (t[j] == '`') {
            size_t run = 0;
            while (j + run < t.size() && t[j + run] == '`') run++;
            size_t close = t.find(std::string(run, '`'), j + run);
            if (close != std::string::npos) j = close + run - 1;
            continue;
        }
        if (t[j] != delim) continue;
        size_t run = 0;
        while (j + run < t.size() && t[j + run] == delim) run++;
        bool before_ok = j > from && t[j - 1] != ' ';
        bool after_word = j + run < t.size() && is_word(t[j + run]);
        if (run >= n && before_ok && !(delim == '_' && after_word)) return j + (run - n);
        j += run - 1;
    }
    return std::string::npos;
}

void inline_into(const std::string& t, Style outer, std::vector<Span>& out) {
    std::string buf;
    auto flush = [&] {
        push(out, buf, outer);
        buf.clear();
    };
    for (size_t i = 0; i < t.size();) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size() && is_punct(t[i + 1])) {
            buf += t[i + 1];
            i += 2;
            continue;
        }
        if (c == '&' && entity(t, i, buf)) continue;
        if (c == '`') {
            size_t run = 0;
            while (i + run < t.size() && t[i + run] == '`') run++;
            size_t close = t.find(std::string(run, '`'), i + run);
            while (close != std::string::npos && close + run < t.size() && t[close + run] == '`')
                close = t.find(std::string(run, '`'), close + run + 1);
            if (close == std::string::npos) {
                buf.append(run, '`');
                i += run;
                continue;
            }
            std::string code = t.substr(i + run, close - i - run);
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos)
                code = code.substr(1, code.size() - 2);
            flush();
            push(out, code, Style::Code);
            i = close + run;
            continue;
        }
        if (c == '*' || c == '_') {
            size_t run = 0;
            while (i + run < t.size() && t[i + run] == c) run++;
            bool left_ok = i + run < t.size() && t[i + run] != ' ' &&
                           !(c == '_' && i > 0 && is_word(t[i - 1]));  // snake_case stays literal
            size_t n = run >= 2 ? 2 : 1;
            size_t close = left_ok ? find_closing(t, i + n, c, n) : std::string::npos;
            if (close == std::string::npos && n == 2 && left_ok) {
                n = 1;
                close = find_closing(t, i + 1, c, 1);
            }
            if (close == std::string::npos) {
                buf.append(run, c);
                i += run;
                continue;
            }
            flush();
            Style st = n == 2 ? Style::Bold : Style::Italic;
            if (outer == Style::Bold) st = Style::Bold;  // bold wins when nested
            inline_into(t.substr(i + n, close - i - n), st, out);
            i = close + n;
            continue;
        }
        if (c == '[' || (c == '!' && i + 1 < t.size() && t[i + 1] == '[')) {
            size_t open = c == '!' ? i + 1 : i;
            size_t close = t.find(']', open + 1);
            if (close != std::string::npos && close + 1 < t.size() && t[close + 1] == '(') {
                size_t end = t.find(')', close + 2);
                if (end != std::string::npos) {
                    flush();
                    inline_into(t.substr(open + 1, close - open - 1), Style::Link, out);
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == '<') {
            size_t end = t.find('>', i + 1);
            if (end != std::string::npos) {
                std::string inner = t.substr(i + 1, end - i - 1);
                if (inner.rfind("http://", 0) == 0 || inner.rfind("https://", 0) == 0) {
                    flush();
                    push(out, inner, Style::Link);
                    i = end + 1;
                    continue;
                }
            }
        }
        buf += c;
        i++;
    }
    flush();
}

bool is_rule(const std::string& line) {
    std::string t = trim(line);
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    size_t count = 0;
    for (char ch : t) {
        if (ch == c) count++;
        else if (ch != ' ') return false;
    }
    return count >= 3;
}

bool is_fence(const std::string& line, std::string* info) {
    std::string t = trim(line);
    if (t.rfind("```", 0) != 0 && t.rfind("~~~", 0) != 0) return false;
    if (info) *info = trim(t.substr(3));
    return true;
}

// "- x", "* x", "+ x", "1. x", "2) x": returns the content start, or npos.
size_t list_marker(const std::string& line, std::string& marker) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return std::string::npos;
    char c = line[i];
    if ((c == '-' || c == '*' || c == '+') && i + 1 < line.size() && line[i + 1] == ' ') {
        marker = "•";
        return line.find_first_not_of(' ', i + 1);
    }
    size_t j = i;
    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) j++;
    if (j > i && j - i <= 9 && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
        marker = line.substr(i, j - i) + ".";
        return line.find_first_not_of(' ', j + 1);
    }
    return std::string::npos;
}

} // namespace

std::vector<Span> parse_inline(const std::string& text) {
    std::vector<Span> out;
    inline_into(text, Style::Normal, out);
    return out;
}

std::vector<Block> parse(const std::string& markdown) {
    std::vector<std::string> lines;
    {
        size_t start = 0;
        for (;;) {
            size_t nl = markdown.find('\n', start);
            std::string l = markdown.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(std::move(l));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    std::vector<Block> blocks;
    // Text of the open paragraph / list item, joined with hard-break markers.
    std::string para;
    Block* open = nullptr;  // open Paragraph or ListItem awaiting more lines
    std::vector<size_t> list_indents;  // content indent per nesting level
    auto close = [&] {
        if (open) {
            open->spans = parse_inline(para);
            // Hard breaks were encoded as \x01 so inline parsing sees one string.
            std::vector<Span> fixed;
            for (auto& s : open->spans) {
                size_t from = 0;
                for (;;) {
                    size_t k = s.text.find('\x01', from);
                    std::string piece = s.text.substr(from, k == std::string::npos ? std::string::npos : k - from);
                    if (!piece.empty()) fixed.push_back({piece, s.style});
                    if (k == std::string::npos) break;
                    fixed.push_back({"\n", Style::Normal});
                    from = k + 1;
                }
            }
            while (!fixed.empty() && fixed.back().text == "\n") fixed.pop_back();
            open->spans = std::move(fixed);
            if (open->spans.empty() && open->kind == Block::Paragraph) blocks.pop_back();
        }
        open = nullptr;
        para.clear();
    };
    auto add_text = [&](const std::string& raw) {
        bool hard = raw.size() >= 2 && raw.compare(raw.size() - 2, 2, "  ") == 0;
        std::string t = trim(raw);
        bool backslash = !t.empty() && t.back() == '\\' && (t.size() < 2 || t[t.size() - 2] != '\\');
        if (backslash) t.pop_back();
        if (!para.empty() && para.back() != '\x01') para += ' ';
        para += t;
        if (hard || backslash) para += '\x01';
    };

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];
        std::string info;
        if (is_fence(line, &info)) {
            close();
            list_indents.clear();
            Block b;
            b.kind = Block::Code;
            size_t first_space = info.find_first_of(" {,");
            b.lang = info.substr(0, first_space);
            std::string fence = trim(line).substr(0, 3);
            size_t fence_indent = indent_of(line);
            for (i++; i < lines.size(); i++) {
                if (trim(lines[i]).rfind(fence, 0) == 0 && trim(lines[i]).find_first_not_of(fence[0]) == std::string::npos) break;
                std::string l = lines[i];
                size_t strip = std::min(fence_indent, indent_of(l));
                b.lines.push_back(l.substr(std::min(strip, l.size())));
            }
            blocks.push_back(std::move(b));
            continue;
        }
        if (trim(line).empty()) {
            close();
            continue;
        }
        if (is_rule(line) && !(open && open->kind == Block::ListItem && line.find_first_not_of(' ') > 0)) {
            close();
            list_indents.clear();
            Block b;
            b.kind = Block::Rule;
            blocks.push_back(std::move(b));
            continue;
        }
        std::string t = trim(line);
        if (t[0] == '#') {
            size_t level = t.find_first_not_of('#');
            if (level != std::string::npos && level <= 6 && t[level] == ' ') {
                close();
                list_indents.clear();
                Block b;
                b.kind = Block::Heading;
                b.level = static_cast<int>(level);
                std::string body = trim(t.substr(level));
                while (!body.empty() && body.back() == '#') body.pop_back();  // closing hashes
                b.spans = parse_inline(trim(body));
                blocks.push_back(std::move(b));
                continue;
            }
        }
        std::string marker;
        size_t content = list_marker(line, marker);
        if (content != std::string::npos) {
            close();
            size_t ind = indent_of(line);
            while (!list_indents.empty() && ind < list_indents.back()) list_indents.pop_back();
            Block b;
            b.kind = Block::ListItem;
            b.marker = marker;
            b.level = static_cast<int>(list_indents.size());
            list_indents.push_back(content);  // nested items start at or past this column
            blocks.push_back(std::move(b));
            open = &blocks.back();
            add_text(line.substr(content));
            continue;
        }
        if (open && open->kind == Block::ListItem) {  // lazy continuation of the item
            add_text(line);
            continue;
        }
        if (!open) {
            list_indents.clear();
            blocks.push_back(Block{});
            open = &blocks.back();
        }
        add_text(line);
    }
    close();
    return blocks;
}

} // namespace oh::md
