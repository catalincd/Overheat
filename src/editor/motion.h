#pragma once
// Pure cursor-motion boundary logic (4coder-style), independent of the buffer so it can
// be unit-tested over plain strings. Positions are byte offsets; every boundary falls
// between characters of different classes, and all bytes of a UTF-8 sequence share a
// class, so results never split a code point.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oh::motion {

enum class Boundary {
    Whitespace,    // runs of non-whitespace: `foo.bar()` is one stop (4coder whitespace_boundary)
    AlphaNumeric,  // runs of [A-Za-z0-9_] and non-ASCII (4coder alpha_numeric_boundary)
    Subword,       // like AlphaNumeric, also stopping at camelCase humps and '_' (snake_case)
};

inline bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
inline bool is_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

// Generic over a byte accessor `at(i)` (i < len) so the piece-table buffer can be scanned
// without copying it. Right lands on the end of the next run, left on the start of the
// previous run (Emacs/4coder style); both cross line breaks.
template <class At> size_t seek_right(At&& at, size_t len, size_t pos, Boundary b);
template <class At> size_t seek_left(At&& at, size_t len, size_t pos, Boundary b);

inline size_t seek_right(std::string_view s, size_t pos, Boundary b);
inline size_t seek_left(std::string_view s, size_t pos, Boundary b);

// Ctrl+Up/Down (4coder move_up/down_to_blank_line_end): from `line`, skip any blank lines
// next to it, then the following paragraph, and stop on the first blank line after it
// (a line holding only whitespace counts as blank). Returns `line_count` when no such
// line exists below (dir > 0), or SIZE_MAX above (dir < 0): the caller clamps to the
// buffer's end / start.
template <class IsBlank> size_t blank_line_hop(IsBlank&& is_blank, size_t line_count, size_t line, int dir);

// ---- implementation ------------------------------------------------------------

namespace detail {
inline bool is_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
// "Lower" includes digits and non-ASCII so `utf8Decode` splits as `utf8|Decode`.
inline bool is_lower(unsigned char c) { return is_alnum(c) && c != '_' && !is_upper(c); }
inline bool in_run(Boundary b, unsigned char c) {
    switch (b) {
        case Boundary::Whitespace: return !is_space(c);
        case Boundary::AlphaNumeric: return is_alnum(c);
        case Boundary::Subword: return is_alnum(c) && c != '_';
    }
    return false;
}
}  // namespace detail

template <class At> size_t seek_right(At&& at, size_t len, size_t pos, Boundary b) {
    using namespace detail;
    auto c = [&](size_t i) { return static_cast<unsigned char>(at(i)); };
    while (pos < len && !in_run(b, c(pos))) ++pos;
    if (b != Boundary::Subword) {
        while (pos < len && in_run(b, c(pos))) ++pos;
        return pos;
    }
    if (pos >= len) return pos;
    if (is_upper(c(pos)) && pos + 1 < len && is_lower(c(pos + 1))) {
        ++pos;  // camel hump: `Foo`
        while (pos < len && is_lower(c(pos))) ++pos;
    } else if (is_upper(c(pos))) {
        // Acronym: `HTTPServer` stops before the `S` that starts the next hump.
        size_t start = pos;
        while (pos < len && is_upper(c(pos))) ++pos;
        if (pos < len && is_lower(c(pos)) && pos - start > 1) --pos;
    } else {
        while (pos < len && is_lower(c(pos))) ++pos;
    }
    return pos;
}

template <class At> size_t seek_left(At&& at, size_t len, size_t pos, Boundary b) {
    using namespace detail;
    auto c = [&](size_t i) { return static_cast<unsigned char>(at(i)); };
    if (pos > len) pos = len;
    while (pos > 0 && !in_run(b, c(pos - 1))) --pos;
    if (b != Boundary::Subword) {
        while (pos > 0 && in_run(b, c(pos - 1))) --pos;
        return pos;
    }
    if (pos == 0) return pos;
    if (is_lower(c(pos - 1))) {
        while (pos > 0 && is_lower(c(pos - 1))) --pos;
        if (pos > 0 && is_upper(c(pos - 1))) --pos;  // include the hump's capital
    } else {
        while (pos > 0 && is_upper(c(pos - 1))) --pos;
    }
    return pos;
}

template <class IsBlank> size_t blank_line_hop(IsBlank&& is_blank, size_t line_count, size_t line, int dir) {
    if (dir > 0) {
        size_t i = line + 1;
        while (i < line_count && is_blank(i)) ++i;
        while (i < line_count && !is_blank(i)) ++i;
        return i;  // == line_count: none
    }
    if (line == 0) return SIZE_MAX;
    size_t i = line - 1;
    while (i != SIZE_MAX && is_blank(i)) --i;
    while (i != SIZE_MAX && !is_blank(i)) --i;
    return i;
}

inline size_t seek_right(std::string_view s, size_t pos, Boundary b) {
    return seek_right([&](size_t i) { return s[i]; }, s.size(), pos, b);
}
inline size_t seek_left(std::string_view s, size_t pos, Boundary b) {
    return seek_left([&](size_t i) { return s[i]; }, s.size(), pos, b);
}

}  // namespace oh::motion
