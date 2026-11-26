#pragma once
// A small markdown parser for hover tooltips (LSP MarkupContent): paragraphs with
// hard breaks, ATX headings, fenced code blocks, horizontal rules and (nested)
// lists; inline code, bold, italic, links and backslash escapes. Rendering lives
// in hover_popup.cpp; this part is pure so it can be unit-tested.
#include <cstdint>
#include <string>
#include <vector>

namespace oh::md {

enum class Style : uint8_t { Normal, Code, Bold, Italic, Link };

struct Span {
    std::string text;  // "\n" alone = hard line break
    Style style = Style::Normal;
    bool operator==(const Span& o) const { return text == o.text && style == o.style; }
};

struct Block {
    enum Kind { Paragraph, Heading, Code, Rule, ListItem } kind = Paragraph;
    std::vector<Span> spans;         // Paragraph / Heading / ListItem
    std::string lang;                // Code: info string ("cpp", "rust", ...)
    std::vector<std::string> lines;  // Code
    int level = 0;                   // Heading: 1..6; ListItem: nesting depth (0 = top)
    std::string marker;              // ListItem: "•" or "1."
};

std::vector<Block> parse(const std::string& markdown);
std::vector<Span> parse_inline(const std::string& text);

} // namespace oh::md
