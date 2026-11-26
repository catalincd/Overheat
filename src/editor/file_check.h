#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// What to do with a file before reading it into an editor (App::open_file):
// binaries and very large files get a card in place of the text first.
namespace oh::filecheck {

constexpr size_t BINARY_PROBE = 8192;            // bytes sniffed for NUL (as search does)
constexpr uint64_t LARGE_MODE = 16ull << 20;     // large-file mode above this
constexpr uint64_t ASK_SIZE = 50ull << 20;       // ask before opening above this
constexpr uint64_t HARD_CAP = 256ull << 20;      // never read more than this

enum class Verdict {
    Open,      // plain text: open normally
    Binary,    // NUL in the first 8 KiB: ask, then open read-only
    AskLarge,  // above ASK_SIZE: ask, then open in large-file mode
    TooLarge,  // above HARD_CAP: refuse
};

inline bool looks_binary(std::string_view head) {
    return head.substr(0, BINARY_PROBE).find('\0') != std::string_view::npos;
}

inline Verdict classify(uint64_t size, std::string_view head) {
    if (size > HARD_CAP) return Verdict::TooLarge;
    if (looks_binary(head)) return Verdict::Binary;
    if (size > ASK_SIZE) return Verdict::AskLarge;
    return Verdict::Open;
}

// Large-file mode: no syntax tree, language server, minimap, word wrap or blame.
inline bool large_mode(uint64_t size) { return size > LARGE_MODE; }

inline std::string human_size(uint64_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
    else if (n < (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(n) / 1024.0);
    else if (n < (1ull << 30)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(n) / (1 << 20));
    else std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(n) / (1 << 30));
    return buf;
}

} // namespace oh::filecheck
