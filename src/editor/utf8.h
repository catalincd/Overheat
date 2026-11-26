#pragma once
#include <cstddef>
#include <string>

// Buffer positions are byte offsets; these helpers map them to character
// boundaries and to on-screen (monospace) columns.
namespace oh::utf8 {

inline size_t TAB_WIDTH = 4;  // settings: tab_width

inline bool is_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Length of the well-formed UTF-8 sequence starting at s[i] (1-4), or 0 if the
// bytes there are not one (stray continuation, bad lead, truncated, overlong,
// surrogate, > U+10FFFF). Same rules as HarfBuzz, which shapes each byte of an
// ill-formed sequence as its own U+FFFD.
inline size_t sequence_length(const std::string& s, size_t i) {
    auto at = [&](size_t k) { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0u; };
    unsigned b = at(i);
    if (b < 0x80) return 1;
    size_t n;
    unsigned lo = 0x80, hi = 0xBF;  // allowed range of the second byte
    if (b >= 0xC2 && b <= 0xDF) n = 2;
    else if (b >= 0xE0 && b <= 0xEF) { n = 3; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
    else if (b >= 0xF0 && b <= 0xF4) { n = 4; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
    else return 0;
    if (i + n > s.size()) return 0;
    if (at(i + 1) < lo || at(i + 1) > hi) return 0;
    for (size_t k = 2; k < n; k++)
        if (!is_continuation(s[i + k])) return 0;
    return n;
}

// Character boundaries. A valid sequence is one character; every byte of
// invalid UTF-8 is a character of its own (drawn as U+FFFD), so the cursor can
// step through binary or Latin-1 text one byte at a time without skipping.
inline size_t next_boundary(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    size_t n = sequence_length(s, i);
    return i + (n ? n : 1);
}

inline size_t prev_boundary(const std::string& s, size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) return s.size();
    if (!is_continuation(s[i - 1])) return i - 1;
    // The character ending at i starts at most 3 bytes back, at a lead byte whose
    // sequence reaches exactly i; otherwise the byte before i stands alone.
    for (size_t k = 2; k <= 4 && k <= i; k++) {
        size_t j = i - k;
        if (is_continuation(s[j])) continue;
        return sequence_length(s, j) == k ? j : i - 1;
    }
    return i - 1;
}

// Code point of the character at s[i] (U+FFFD for an invalid byte).
inline char32_t decode(const std::string& s, size_t i) {
    size_t n = sequence_length(s, i);
    auto b = static_cast<unsigned char>(s[i]);
    if (n == 0) return 0xFFFD;
    if (n == 1) return b;
    char32_t cp = b & (0x7F >> n);
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return cp;
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
        u += sequence_length(s, i) == 4 ? 2 : 1;  // astral planes take a surrogate pair
    return u;
}

inline size_t byte_col_from_utf16(const std::string& s, size_t u16) {
    size_t u = 0, i = 0;
    while (i < s.size() && u < u16) {
        u += sequence_length(s, i) == 4 ? 2 : 1;
        i = next_boundary(s, i);
    }
    return i;
}

} // namespace oh::utf8
