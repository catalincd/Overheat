#include "tasks/links.h"
#include <algorithm>
#include <cstdlib>

namespace oh {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool path_char(char c) {
    switch (c) {
        case ' ': case '\t': case '"': case '\'': case '<': case '>': case '(': case ')': case '[': case ']':
        case '{': case '}': case '|': case ',': case ';': case ':': case '`': case '=':
            return false;
        default:
            return static_cast<unsigned char>(c) >= 0x20;
    }
}

size_t read_number(const std::string& s, size_t i, size_t& value) {
    size_t j = i;
    while (j < s.size() && is_digit(s[j]) && j - i < 9) j++;
    value = j > i ? std::strtoul(s.substr(i, j - i).c_str(), nullptr, 10) : 0;
    return j;
}

bool plausible(const std::string& p, bool has_line) {
    if (p.empty() || p == "." || p == "..") return false;
    bool slash = p.find('/') != std::string::npos || p.find('\\') != std::string::npos;
    size_t dot = p.rfind('.');
    bool ext = dot != std::string::npos && dot + 1 < p.size() && dot + 7 >= p.size() && is_alpha(p[dot + 1]);
    if (has_line) return slash || dot != std::string::npos;
    return slash || ext;  // a bare word with no extension is not a path
}

} // namespace

std::vector<TextLink> find_links(const std::string& s) {
    std::vector<TextLink> out;
    size_t i = 0;
    while (i < s.size()) {
        // Python: File "path", line N
        if (s.compare(i, 6, "File \"") == 0) {
            size_t q = s.find('"', i + 6);
            if (q != std::string::npos && s.compare(q, 8, "\", line ") == 0) {
                TextLink l;
                l.begin = i + 6;
                l.path = s.substr(i + 6, q - i - 6);
                size_t end = read_number(s, q + 8, l.line);
                if (l.line > 0) {
                    l.end = end;
                    out.push_back(l);
                    i = end;
                    continue;
                }
            }
        }
        if (!path_char(s[i])) {
            i++;
            continue;
        }
        size_t start = i;
        size_t j = i;
        // Windows drive: C:\ or C:/
        if (j + 2 < s.size() && is_alpha(s[j]) && s[j + 1] == ':' && (s[j + 2] == '\\' || s[j + 2] == '/') &&
            (j == 0 || !path_char(s[j - 1])))
            j += 2;
        while (j < s.size() && path_char(s[j])) j++;
        size_t path_end = j;
        // A sentence's full stop is not part of the path ("see src/a.c.").
        bool followed = j < s.size() && (s[j] == ':' || s[j] == '(');
        while (!followed && path_end > start + 1 && s[path_end - 1] == '.' && s[path_end - 2] != '.') path_end--;
        std::string path = s.substr(start, path_end - start);
        TextLink l;
        l.begin = start;
        l.path = path;
        size_t end = path_end;
        if (path_end == j && j + 1 < s.size() && s[j] == ':' && is_digit(s[j + 1])) {
            end = read_number(s, j + 1, l.line);
            if (end + 1 < s.size() && s[end] == ':' && is_digit(s[end + 1])) end = read_number(s, end + 1, l.col);
        } else if (path_end == j && j + 1 < s.size() && s[j] == '(' && is_digit(s[j + 1])) {
            size_t k = read_number(s, j + 1, l.line);
            size_t col = 0;
            if (k + 1 < s.size() && s[k] == ',' && is_digit(s[k + 1])) k = read_number(s, k + 1, col);
            if (k < s.size() && s[k] == ')') {
                l.col = col;
                end = k + 1;
            } else {
                l.line = 0;
            }
        }
        if (plausible(path, l.line > 0)) {
            l.end = end;
            out.push_back(l);
        }
        i = std::max(end, j);
        if (i == start) i++;
    }
    return out;
}

bool link_at(const std::string& line, size_t at, TextLink& out) {
    for (auto& l : find_links(line))
        if (at >= l.begin && at < l.end) {
            out = l;
            return true;
        }
    return false;
}

} // namespace oh
