#include "tasks/matchers.h"
#include "tasks/json.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace oh {

namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

size_t to_index(const std::string& digits) {  // "12" -> 11 (0-based); 0 stays 0
    unsigned long v = std::strtoul(digits.c_str(), nullptr, 10);
    return v > 0 ? static_cast<size_t>(v - 1) : 0;
}

struct Loc {
    std::string file;
    size_t line = 0, col = 0;
    bool has_col = false;
    size_t end = 0;  // index just past the location's trailing ':'
};

// "<file>:<line>[:<col>]:" at the start of s (gcc style). The file part is
// everything before the first ":<digits>" that is followed by ':' (so Windows
// drive letters and "C:\x" survive).
bool parse_colon_loc(const std::string& s, size_t from, Loc& loc) {
    for (size_t p = s.find(':', from + 1); p != std::string::npos; p = s.find(':', p + 1)) {
        size_t q = p + 1;
        while (q < s.size() && is_digit(s[q])) q++;
        if (q == p + 1 || q >= s.size() || s[q] != ':') continue;
        std::string line = s.substr(p + 1, q - p - 1);
        size_t r = q + 1;
        while (r < s.size() && is_digit(s[r])) r++;
        loc.file = s.substr(from, p - from);
        loc.line = to_index(line);
        if (r > q + 1 && r < s.size() && s[r] == ':') {
            loc.has_col = true;
            loc.col = to_index(s.substr(q + 1, r - q - 1));
            loc.end = r + 1;
        } else if (r > q + 1 && (r == s.size() || s[r] == ' ')) {
            loc.has_col = true;  // "file:3:4" at the end of the line (rustc -->) or before a space
            loc.col = to_index(s.substr(q + 1, r - q - 1));
            loc.end = r;
        } else {
            loc.has_col = false;
            loc.end = q + 1;
        }
        return !loc.file.empty();
    }
    // "file:12" at the very end (rustc's "--> file:3:4" handled above; this is file:line).
    return false;
}

// Moves a trailing " [-Wfoo]" / " [-Werror=foo]" / " [clang-diagnostic-x]" into `code`.
void split_flag(std::string& msg, std::string& code) {
    if (msg.size() < 4 || msg.back() != ']') return;
    size_t open = msg.rfind(" [");
    if (open == std::string::npos) return;
    std::string inner = msg.substr(open + 2, msg.size() - open - 3);
    if (inner.empty() || inner.find(' ') != std::string::npos) return;
    code = inner;
    msg.erase(open);
}

bool looks_like_path(const std::string& p) {
    if (p.empty() || p.size() > 1024) return false;
    for (char c : p)
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '*' ||
            c == '?' || c == '[' || c == ']' || c == '(' || c == ')' || c == '=')
            return false;
    return p.find('.') != std::string::npos || p.find('/') != std::string::npos ||
           p.find('\\') != std::string::npos;
}

} // namespace

std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r') continue;
        if (c != 0x1B) {
            out += static_cast<char>(c);
            continue;
        }
        if (i + 1 >= s.size()) break;
        char k = s[i + 1];
        if (k == '[') {  // CSI: parameters, then a final byte in @..~
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) j++;
            i = j;
        } else if (k == ']') {  // OSC: up to BEL or ST (ESC \)
            size_t j = i + 2;
            while (j < s.size() && s[j] != '\a' && !(s[j] == 0x1B && j + 1 < s.size() && s[j + 1] == '\\')) j++;
            i = (j < s.size() && s[j] == 0x1B) ? j + 1 : j;
        } else {
            i += 1;  // two-byte escape (ESC 7, ESC =, ...)
        }
    }
    return out;
}

std::string ProblemCollector::canonical(const std::string& name) {
    std::string n = name;
    if (!n.empty() && n[0] == '$') n.erase(0, 1);
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "gcc" || n == "clang" || n == "nvcc" || n == "gcc-ld") return "gcc";
    if (n == "mscompile" || n == "msvc" || n == "cl") return "msvc";
    if (n == "cmake") return "cmake";
    if (n == "rustc" || n == "rust" || n == "rustc-watch" || n == "cargo") return "rustc";
    if (n == "cargo-json" || n == "rustc-json") return "cargo-json";
    if (n == "generic" || n == "file") return "generic";
    return "";
}

ProblemCollector::ProblemCollector(const std::vector<std::string>& matchers) {
    for (auto& m : matchers) {
        std::string c = canonical(m);
        if (!c.empty() && std::find(names_.begin(), names_.end(), c) == names_.end()) names_.push_back(c);
    }
}
ProblemCollector::~ProblemCollector() = default;
ProblemCollector::ProblemCollector(ProblemCollector&&) noexcept = default;
ProblemCollector& ProblemCollector::operator=(ProblemCollector&&) noexcept = default;

bool ProblemCollector::uses(const std::string& matcher) const {
    return std::find(names_.begin(), names_.end(), matcher) != names_.end();
}

void ProblemCollector::feed(const std::string& chunk) {
    partial_ += chunk;
    size_t start = 0;
    for (size_t nl; (nl = partial_.find('\n', start)) != std::string::npos; start = nl + 1)
        feed_line(partial_.substr(start, nl - start));
    partial_.erase(0, start);
}

void ProblemCollector::finish() {
    if (!partial_.empty()) {
        std::string rest;
        rest.swap(partial_);
        feed_line(rest);
    }
    close_cmake();
    rust_header_ = false;
}

void ProblemCollector::feed_line(const std::string& raw_line) {
    // A carriage return inside the line (progress bars) returns to column 0: what
    // follows the last one is what the terminal ends up showing.
    size_t cr = raw_line.find_last_of('\r', raw_line.size() >= 2 ? raw_line.size() - 2 : 0);
    std::string line = strip_ansi(cr == std::string::npos || raw_line.size() < 2 ? raw_line : raw_line.substr(cr + 1));
    if (cmake_open_ && uses("cmake") && match_cmake(line)) return;
    for (auto& n : names_) {
        if (n == "gcc" && match_gcc(line)) return;
        if (n == "msvc" && match_msvc(line)) return;
        if (n == "cmake" && match_cmake(line)) return;
        if (n == "rustc" && match_rustc(line)) return;
        if (n == "cargo-json" && starts_with(line, "{\"reason\":")) {
            std::string display;
            cargo_json_line(line, display, &problems_);
            return;
        }
        if (n == "generic" && match_generic(line)) return;
    }
}

// ---------------------------------------------------------------------------
// gcc / clang
// ---------------------------------------------------------------------------

bool ProblemCollector::match_gcc(const std::string& line) {
    // Include chain: "In file included from a.c:2:" then "                 from b.h:3,".
    std::string t = trim(line);
    bool included = starts_with(line, "In file included from ");
    if (included || (starts_with(t, "from ") && !gcc_pending_.empty() && line.size() > t.size())) {
        if (included) gcc_pending_.clear();
        std::string rest = included ? line.substr(22) : t.substr(5);
        while (!rest.empty() && (rest.back() == ':' || rest.back() == ',')) rest.pop_back();
        Loc loc;
        ProblemNote n;
        if (parse_colon_loc(rest + ":", 0, loc)) {
            n.file = loc.file;
            n.line = loc.line;
            n.col = loc.col;
        }
        n.message = "In file included from " + rest;
        gcc_pending_.push_back(std::move(n));
        return true;
    }
    Loc loc;
    if (!parse_colon_loc(line, 0, loc)) {
        // Context headers: "file: In instantiation of 'X':", "file: In function 'f':", "file: At top level:".
        size_t sep = line.find(": In ");
        if (sep == std::string::npos) sep = line.find(": At ");
        if (sep != std::string::npos && sep > 0 && !line.empty() && line.back() == ':' &&
            looks_like_path(line.substr(0, sep))) {
            std::string what = line.substr(sep + 2, line.size() - sep - 3);
            // "In function 'f'" / "At top level": the next diagnostic says where; nothing to keep.
            if (starts_with(what, "In instantiation of") || starts_with(what, "In substitution of"))
                gcc_pending_.push_back({line.substr(0, sep), 0, 0, what});
            return true;
        }
        return false;
    }
    if (loc.file.find(' ') != std::string::npos && !looks_like_path(trim(loc.file))) return false;
    std::string rest = line.substr(loc.end);
    std::string body = trim(rest);
    int severity = 0;
    bool note = false;
    std::string msg;
    auto kw = [&](const char* k) {
        if (!starts_with(body, k)) return false;
        msg = trim(body.substr(std::char_traits<char>::length(k)));
        return true;
    };
    if (kw("fatal error:") || kw("error:")) severity = 1;
    else if (kw("warning:")) severity = 2;
    else if (kw("remark:")) severity = 3;
    else if (kw("note:")) note = true;
    else if (starts_with(body, "required from") || starts_with(body, "required by") ||
             starts_with(body, "recursively required") || starts_with(body, "in ")) {
        // gcc's instantiation context, printed before the error it explains.
        gcc_pending_.push_back({loc.file, loc.line, loc.col, body});
        return true;
    } else {
        return false;
    }

    if (note) {
        // Notes explain the diagnostic before them (clang's "in instantiation of ...
        // requested here", gcc's "declared here").
        if (!problems_.empty() && problems_.back().source == "gcc") {
            problems_.back().notes.push_back({loc.file, loc.line, loc.col, msg});
            gcc_pending_.clear();
            return true;
        }
        severity = 3;
    }
    Problem p;
    p.file = loc.file;
    p.line = p.end_line = loc.line;
    p.col = p.end_col = loc.col;
    p.severity = severity;
    p.message = msg;
    p.source = "gcc";
    split_flag(p.message, p.code);
    p.notes = std::move(gcc_pending_);
    gcc_pending_.clear();
    problems_.push_back(std::move(p));
    return true;
}

// ---------------------------------------------------------------------------
// MSVC
// ---------------------------------------------------------------------------

bool ProblemCollector::match_msvc(const std::string& raw) {
    std::string line = trim(raw);
    // msbuild prefixes "12>" (project number) and appends " [C:\x\proj.vcxproj]".
    size_t gt = 0;
    while (gt < line.size() && is_digit(line[gt])) gt++;
    if (gt > 0 && gt < line.size() && line[gt] == '>') line = line.substr(gt + 1);
    if (!line.empty() && line.back() == ']') {
        size_t open = line.rfind(" [");
        if (open != std::string::npos && line.find(".vcxproj", open) != std::string::npos) line.erase(open);
    }
    for (size_t open = line.find('('); open != std::string::npos; open = line.find('(', open + 1)) {
        size_t close = line.find(')', open);
        if (close == std::string::npos) return false;
        std::string inside = line.substr(open + 1, close - open - 1);
        if (inside.empty() || !std::all_of(inside.begin(), inside.end(), [](char c) { return is_digit(c) || c == ','; }))
            continue;
        size_t colon = close + 1;
        while (colon < line.size() && line[colon] == ' ') colon++;
        if (colon >= line.size() || line[colon] != ':') continue;
        std::string body = trim(line.substr(colon + 1));
        int severity = 0;
        size_t kw_len = 0;
        for (auto [k, sev] : {std::pair{"fatal error", 1}, {"error", 1}, {"warning", 2}, {"note", 3}, {"info", 3}})
            if (starts_with(body, k)) {
                severity = sev;
                kw_len = std::char_traits<char>::length(k);
                break;
            }
        if (!severity) return false;
        std::string after = body.substr(kw_len);  // " C2065: msg" or ": msg"
        std::string code;
        size_t c2 = after.find(':');
        if (c2 == std::string::npos) return false;
        code = trim(after.substr(0, c2));
        if (code.find(' ') != std::string::npos) return false;
        Problem p;
        p.file = trim(line.substr(0, open));
        std::vector<size_t> nums;
        for (size_t s = 0; s <= inside.size();) {
            size_t e = inside.find(',', s);
            if (e == std::string::npos) e = inside.size();
            nums.push_back(to_index(inside.substr(s, e - s)));
            s = e + 1;
        }
        p.line = p.end_line = nums[0];
        if (nums.size() >= 2) p.col = p.end_col = nums[1];
        if (nums.size() == 4) p.end_line = nums[2], p.end_col = nums[3];
        p.severity = severity;
        p.code = code;
        p.message = trim(after.substr(c2 + 1));
        p.source = "msvc";
        if (severity == 3 && !problems_.empty() && problems_.back().source == "msvc") {
            problems_.back().notes.push_back({p.file, p.line, p.col, p.message});
            return true;
        }
        if (p.file.empty()) return false;
        problems_.push_back(std::move(p));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// CMake
// ---------------------------------------------------------------------------

void ProblemCollector::close_cmake() {
    if (!cmake_open_) return;
    cmake_open_ = false;
    cmake_.message = trim(cmake_.message);
    if (cmake_.message.empty()) cmake_.message = cmake_.severity == 1 ? "CMake Error" : "CMake Warning";
    problems_.push_back(std::move(cmake_));
    cmake_ = Problem{};
}

bool ProblemCollector::match_cmake(const std::string& line) {
    if (cmake_open_) {
        if (line.empty() || starts_with(line, "  ")) {
            std::string t = trim(line);
            if (t.empty() || t == "Call Stack (most recent call first):") {
                if (!t.empty()) cmake_.message += "\n";
                return true;
            }
            if (!cmake_.message.empty() && cmake_.message.back() != '\n') cmake_.message += ' ';
            cmake_.message += t;
            return true;
        }
        close_cmake();
    }
    if (!starts_with(line, "CMake Error") && !starts_with(line, "CMake Warning") &&
        !starts_with(line, "CMake Deprecation Warning"))
        return false;
    Problem p;
    p.source = "cmake";
    p.severity = starts_with(line, "CMake Error") ? 1 : 2;
    size_t at = line.find(" at ");
    size_t in = line.find(" in ");
    size_t colon = line.find(':');
    if (at != std::string::npos && (colon == std::string::npos || at < colon)) {
        // "CMake Error at dir/CMakeLists.txt:15 (cmd):"
        std::string rest = line.substr(at + 4);
        size_t paren = rest.rfind(" (");
        std::string where = paren == std::string::npos ? rest : rest.substr(0, paren);
        while (!where.empty() && where.back() == ':') where.pop_back();
        size_t c = where.rfind(':');
        if (c != std::string::npos && c + 1 < where.size() &&
            std::all_of(where.begin() + static_cast<long>(c) + 1, where.end(), is_digit)) {
            p.file = where.substr(0, c);
            p.line = p.end_line = to_index(where.substr(c + 1));
        } else {
            p.file = where;
        }
        if (paren != std::string::npos) {
            size_t close = rest.find(')', paren);
            if (close != std::string::npos) p.code = rest.substr(paren + 2, close - paren - 2);
        }
    } else if (in != std::string::npos && (colon == std::string::npos || in < colon)) {
        // "CMake Error in CMakeLists.txt:" (generate step)
        std::string where = line.substr(in + 4);
        while (!where.empty() && where.back() == ':') where.pop_back();
        p.file = where;
    } else if (colon != std::string::npos) {
        // "CMake Error: message" (no location)
        p.message = trim(line.substr(colon + 1));
    }
    cmake_ = std::move(p);
    cmake_open_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// rustc / cargo (human-readable)
// ---------------------------------------------------------------------------

bool ProblemCollector::match_rustc(const std::string& line) {
    if (rust_panic_) {  // the line after "panicked at file:l:c:" is the message
        rust_panic_ = false;
        if (!problems_.empty()) problems_.back().message = "panicked: " + trim(line);
        return true;
    }
    // Header: "error[E0308]: msg", "warning: msg", "note: msg", "help: msg".
    for (auto [k, sev] : {std::pair{"error", 1}, {"warning", 2}, {"note", 3}, {"help", 3}}) {
        size_t n = std::char_traits<char>::length(k);
        if (!starts_with(line, k)) continue;
        size_t i = n;
        std::string code;
        if (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            code = line.substr(i + 1, close - i - 1);
            i = close + 1;
        }
        if (line.compare(i, 2, ": ") != 0) break;
        Problem p;
        p.severity = sev;
        p.code = code;
        p.message = trim(line.substr(i + 2));
        p.source = "rustc";
        rust_ = std::move(p);
        rust_header_ = true;
        rust_attached_ = false;
        return true;
    }
    std::string t = trim(line);
    bool arrow = starts_with(t, "--> ");
    bool secondary = starts_with(t, "::: ");
    if ((arrow || secondary) && rust_header_) {
        Loc loc;
        if (!parse_colon_loc(t.substr(4), 0, loc)) return false;
        if (arrow && !rust_attached_ && rust_.severity <= 2) {
            Problem p = rust_;
            p.file = loc.file;
            p.line = p.end_line = loc.line;
            p.col = p.end_col = loc.col;
            problems_.push_back(std::move(p));
            rust_attached_ = true;
        } else if (!problems_.empty() && problems_.back().source == "rustc") {
            problems_.back().notes.push_back({loc.file, loc.line, loc.col, rust_attached_ && arrow ? rust_.message : ""});
        }
        return true;
    }
    // "thread 'main' panicked at src/main.rs:5:9:" (message on the next line), or the
    // older "panicked at 'msg', src/main.rs:5:9".
    size_t pa = line.find("panicked at ");
    if (pa != std::string::npos) {
        std::string rest = line.substr(pa + 12);
        Problem p;
        p.source = "rustc";
        p.severity = 1;
        Loc loc;
        if (!rest.empty() && rest[0] == '\'') {
            size_t q = rest.rfind("', ");
            if (q == std::string::npos || !parse_colon_loc(rest.substr(q + 3), 0, loc)) return false;
            p.message = "panicked: " + rest.substr(1, q - 1);
        } else {
            if (!parse_colon_loc(rest, 0, loc)) return false;
            p.message = "panicked";
            rust_panic_ = true;
        }
        p.file = loc.file;
        p.line = p.end_line = loc.line;
        p.col = p.end_col = loc.col;
        problems_.push_back(std::move(p));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Generic file:line[:col]: message
// ---------------------------------------------------------------------------

bool ProblemCollector::match_generic(const std::string& raw) {
    size_t from = raw.find_first_not_of(" \t");
    if (from == std::string::npos) return false;
    Loc loc;
    if (!parse_colon_loc(raw, from, loc) || !looks_like_path(loc.file)) return false;
    std::string msg = trim(raw.substr(loc.end));
    Problem p;
    p.file = loc.file;
    p.line = p.end_line = loc.line;
    p.col = p.end_col = loc.col;
    std::string lower = msg;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    p.severity = starts_with(lower, "warning") ? 2 : (starts_with(lower, "note") || starts_with(lower, "info")) ? 3 : 1;
    for (const char* k : {"error:", "warning:", "note:", "info:"})
        if (starts_with(lower, k)) msg = trim(msg.substr(std::char_traits<char>::length(k)));
    p.message = msg.empty() ? "(no message)" : msg;
    p.source = "generic";
    problems_.push_back(std::move(p));
    return true;
}

// ---------------------------------------------------------------------------
// cargo --message-format=json
// ---------------------------------------------------------------------------

namespace {
const json::Value& outer_span(const json::Value& span) {
    // A primary span inside a std macro (/rustc/...) points at its call site instead.
    const json::Value* s = &span;
    for (int depth = 0; depth < 16; depth++) {
        const std::string& f = (*s)["file_name"].str();
        if (!(starts_with(f, "/rustc/") || starts_with(f, "<")) || !(*s)["expansion"].is_object()) break;
        s = &(*s)["expansion"]["span"];
    }
    return *s;
}
void span_range(const json::Value& s, size_t& line, size_t& col, size_t& end_line, size_t& end_col) {
    auto idx = [](const json::Value& v) {
        long long n = v.integer(1);
        return n > 0 ? static_cast<size_t>(n - 1) : 0;
    };
    line = idx(s["line_start"]);
    col = idx(s["column_start"]);
    end_line = idx(s["line_end"]);
    end_col = idx(s["column_end"]);
}
} // namespace

bool ProblemCollector::cargo_json_line(const std::string& line, std::string& display, std::vector<Problem>* out) {
    display.clear();
    if (!starts_with(line, "{\"reason\":")) return false;
    json::Value v;
    if (!json::parse(line, v)) return false;
    const std::string& reason = v["reason"].str();
    if (reason != "compiler-message") return true;  // artifacts, build-finished: nothing to show
    const json::Value& m = v["message"];
    display = m["rendered"].str();
    if (!out) return true;
    const std::string& level = m["level"].str();
    int severity = starts_with(level, "error") ? 1 : level == "warning" ? 2 : (level == "note" || level == "help") ? 3 : 0;
    if (!severity) return true;  // failure-note
    const json::Value* primary = nullptr;
    for (auto& s : m["spans"].items())
        if (s["is_primary"].boolean()) {
            primary = &s;
            break;
        }
    if (!primary) return true;  // "aborting due to ..." and other summary lines
    Problem p;
    p.source = "rustc";
    p.severity = severity;
    p.message = m["message"].str();
    p.code = m["code"]["code"].str();
    const json::Value& ps = outer_span(*primary);
    p.file = ps["file_name"].str();
    span_range(ps, p.line, p.col, p.end_line, p.end_col);
    for (auto& s : m["spans"].items()) {
        if (&s == primary || s["label"].str().empty()) continue;
        const json::Value& os = outer_span(s);
        ProblemNote n;
        n.file = os["file_name"].str();
        size_t el, ec;
        span_range(os, n.line, n.col, el, ec);
        n.message = s["label"].str();
        p.notes.push_back(std::move(n));
    }
    for (auto& c : m["children"].items()) {
        const std::string& msg = c["message"].str();
        ProblemFix fix;
        bool has_location = false;
        for (auto& s : c["spans"].items()) {
            has_location = true;
            if (!s["suggested_replacement"].is_string()) continue;
            ProblemEdit e;
            e.file = s["file_name"].str();
            span_range(s, e.line, e.col, e.end_line, e.end_col);
            e.text = s["suggested_replacement"].str();
            fix.edits.push_back(std::move(e));
        }
        if (!fix.edits.empty()) {
            fix.title = msg;
            if (fix.edits.size() == 1 && !fix.edits[0].text.empty() && fix.edits[0].text.find('\n') == std::string::npos)
                fix.title += ": `" + fix.edits[0].text + "`";
            p.fixes.push_back(std::move(fix));
            continue;
        }
        ProblemNote n;
        n.message = c["level"].str() + ": " + msg;
        if (has_location) {
            size_t el, ec;
            const json::Value& os = outer_span(c["spans"][size_t{0}]);
            n.file = os["file_name"].str();
            span_range(os, n.line, n.col, el, ec);
        }
        p.notes.push_back(std::move(n));
    }
    out->push_back(std::move(p));
    return true;
}

} // namespace oh
