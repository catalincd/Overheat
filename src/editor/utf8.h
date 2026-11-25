#pragma once
#include <cstddef>
#include <string>

// Buffer positions are byte offsets; these helpers map them to character
// boundaries and to on-screen (monospace) columns.
namespace oh::utf8 {

constexpr size_t TAB_WIDTH = 4;

inline bool is_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

inline size_t prev_boundary(const std::string& s, size_t i) {
    if (i == 0) return 0;
    do { --i; } while (i > 0 && is_continuation(s[i]));
    return i;
}

inline size_t next_boundary(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    do { ++i; } while (i < s.size() && is_continuation(s[i]));
    return i;
}

inline size_t cell_width(char lead, size_t display_col) {
    return lead == '\t' ? TAB_WIDTH - display_col % TAB_WIDTH : 1;
}

inline size_t display_col(const std::string& s, size_t byte_col) {
    size_t d = 0;
    for (size_t i = 0; i < byte_col && i < s.size(); i = next_boundary(s, i))
        d += cell_width(s[i], d);
    return d;
}

// Character boundary nearest to the given display column (ties go right).
inline size_t byte_col_at(const std::string& s, size_t dcol) {
    size_t d = 0, i = 0;
    while (i < s.size()) {
        size_t w = cell_width(s[i], d);
        if (d + w > dcol) return (dcol - d) * 2 >= w ? next_boundary(s, i) : i;
        d += w;
        i = next_boundary(s, i);
    }
    return i;
}

// LSP's default position unit is UTF-16 code units; these convert per line.
inline size_t utf16_col(const std::string& s, size_t byte_col) {
    size_t u = 0;
    for (size_t i = 0; i < byte_col && i < s.size(); i = next_boundary(s, i))
        u += (static_cast<unsigned char>(s[i]) >= 0xF0) ? 2 : 1;  // astral planes take a surrogate pair
    return u;
}

inline size_t byte_col_from_utf16(const std::string& s, size_t u16) {
    size_t u = 0, i = 0;
    while (i < s.size() && u < u16) {
        u += (static_cast<unsigned char>(s[i]) >= 0xF0) ? 2 : 1;
        i = next_boundary(s, i);
    }
    return i;
}

} // namespace oh::utf8
