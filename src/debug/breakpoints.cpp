#include "debug/breakpoints.h"
#include <algorithm>
#include <sstream>

namespace oh {

fs::path normalize_path(const fs::path& p) {
    if (p.empty()) return p;
    std::error_code ec;
    fs::path a = p.is_absolute() ? p : fs::absolute(p, ec);
    return a.lexically_normal();
}

size_t shift_line_for_edit(size_t line, const LineEdit& e) {
    if (line < e.start_line) return line;
    bool pure_insert = e.old_end_line == e.start_line && e.old_end_col == e.start_col;
    if (line == e.start_line) {
        // Typing at column 0 of the line (e.g. Enter there) pushes the whole line down.
        if (pure_insert && e.start_col == 0) return line + (e.new_end_line - e.start_line);
        return line;
    }
    if (line < e.old_end_line) return e.start_line;  // its line was deleted
    if (line == e.old_end_line) return e.new_end_line;  // the rest of the line moved with the edit end
    // Below the edit: shift by the change in line count.
    return line - e.old_end_line + e.new_end_line;
}

SourceBreakpoint* BreakpointStore::find(const fs::path& path, size_t line) {
    for (auto& b : source_)
        if (b.path == path && b.shown_line() == line) return &b;
    for (auto& b : source_)
        if (b.path == path && b.line == line) return &b;
    return nullptr;
}

bool BreakpointStore::toggle(const fs::path& path, size_t line) {
    if (find(path, line)) {
        remove(path, line);
        return false;
    }
    ensure(path, line);
    return true;
}

SourceBreakpoint& BreakpointStore::ensure(const fs::path& path, size_t line) {
    if (SourceBreakpoint* b = find(path, line)) return *b;
    SourceBreakpoint b;
    b.path = path;
    b.line = line;
    source_.push_back(std::move(b));
    std::stable_sort(source_.begin(), source_.end(), [](const SourceBreakpoint& a, const SourceBreakpoint& b) {
        return a.path != b.path ? a.path < b.path : a.line < b.line;
    });
    bump();
    return *find(path, line);
}

void BreakpointStore::remove(const fs::path& path, size_t line) {
    auto it = std::find_if(source_.begin(), source_.end(),
                           [&](const SourceBreakpoint& b) { return b.path == path && b.shown_line() == line; });
    if (it == source_.end())
        it = std::find_if(source_.begin(), source_.end(),
                          [&](const SourceBreakpoint& b) { return b.path == path && b.line == line; });
    if (it == source_.end()) return;
    source_.erase(it);
    bump();
}

std::vector<SourceBreakpoint*> BreakpointStore::in_file(const fs::path& path) {
    std::vector<SourceBreakpoint*> out;
    for (auto& b : source_)
        if (b.path == path) out.push_back(&b);
    return out;
}

std::vector<fs::path> BreakpointStore::files() const {
    std::vector<fs::path> out;
    for (auto& b : source_)
        if (std::find(out.begin(), out.end(), b.path) == out.end()) out.push_back(b.path);
    return out;
}

bool BreakpointStore::shift_for_edit(const fs::path& path, const LineEdit& e) {
    bool moved = false;
    for (auto& b : source_) {
        if (b.path != path) continue;
        size_t nl = shift_line_for_edit(b.line, e);
        if (nl != b.line) {
            b.line = nl;
            moved = true;
        }
        if (b.actual_line >= 0) {
            long na = static_cast<long>(shift_line_for_edit(static_cast<size_t>(b.actual_line), e));
            if (na != b.actual_line) {
                b.actual_line = na;
                moved = true;
            }
        }
    }
    if (!moved) return false;
    // Two breakpoints that collapsed onto one line become one.
    std::vector<SourceBreakpoint> kept;
    for (auto& b : source_) {
        bool dup = std::any_of(kept.begin(), kept.end(), [&](const SourceBreakpoint& k) {
            return k.path == b.path && k.shown_line() == b.shown_line();
        });
        if (!dup) kept.push_back(std::move(b));
    }
    source_ = std::move(kept);
    bump();
    return true;
}

void BreakpointStore::clear_adapter_state() {
    for (auto& b : source_) {
        b.adapter_id = -1;
        b.verified = false;
        b.actual_line = -1;
        b.message.clear();
    }
    for (auto& f : functions_) {
        f.adapter_id = -1;
        f.verified = false;
    }
    bump();
}

bool BreakpointStore::filter_on(const std::string& id) const {
    return std::find(exception_filters.begin(), exception_filters.end(), id) != exception_filters.end();
}

void BreakpointStore::set_filter(const std::string& id, bool on) {
    std::erase(exception_filters, id);
    if (on) exception_filters.push_back(id);
    bump();
}

namespace {
std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string unesc(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t t = line.find('\t', start);
        out.push_back(unesc(line.substr(start, t == std::string::npos ? std::string::npos : t - start)));
        if (t == std::string::npos) break;
        start = t + 1;
    }
    return out;
}
} // namespace

std::string BreakpointStore::serialize() const {
    std::ostringstream out;
    for (auto& b : source_)
        out << "bp\t" << (b.enabled ? 1 : 0) << '\t' << b.line << '\t' << esc(b.path.string()) << '\t'
            << esc(b.condition) << '\t' << esc(b.hit_condition) << '\t' << esc(b.log_message) << '\n';
    for (auto& f : functions_)
        out << "fn\t" << (f.enabled ? 1 : 0) << '\t' << esc(f.name) << '\t' << esc(f.condition) << '\n';
    for (auto& id : exception_filters) out << "filter\t" << esc(id) << '\n';
    return out.str();
}

void BreakpointStore::deserialize(const std::string& text) {
    source_.clear();
    functions_.clear();
    exception_filters.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        auto f = split_tabs(line);
        if (f[0] == "bp" && f.size() >= 4) {
            SourceBreakpoint b;
            b.enabled = f[1] != "0";
            b.line = static_cast<size_t>(std::strtoull(f[2].c_str(), nullptr, 10));
            b.path = f[3];
            if (f.size() > 4) b.condition = f[4];
            if (f.size() > 5) b.hit_condition = f[5];
            if (f.size() > 6) b.log_message = f[6];
            if (!find(b.path, b.line)) source_.push_back(std::move(b));
        } else if (f[0] == "fn" && f.size() >= 3) {
            FunctionBreakpoint fb;
            fb.enabled = f[1] != "0";
            fb.name = f[2];
            if (f.size() > 3) fb.condition = f[3];
            functions_.push_back(std::move(fb));
        } else if (f[0] == "filter" && f.size() >= 2) {
            exception_filters.push_back(f[1]);
        }
    }
    bump();
}

} // namespace oh
