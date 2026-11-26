#pragma once
// Problem matchers: turn build output into Problems, line by line, as it streams.
//
//   gcc        gcc / clang: `file:line:col: error|warning|note: msg`, with
//              "In file included from" chains and template / instantiation context
//              ("required from here", clang's "in instantiation of ... requested
//              here" notes) attached to the diagnostic they explain
//   msvc       cl.exe / clang-cl: `file(line,col): error C1234: msg`
//   cmake      `CMake Error at file:line (command):` plus its indented message
//   rustc      rustc / cargo human output: `error[E0308]: msg` + ` --> file:line:col`,
//              and `panicked at file:line:col:`
//   cargo-json cargo --message-format=json(-diagnostic-rendered-ansi) lines, with
//              rustc's machine-applicable suggestions as quick fixes
//   generic    any `file:line[:col]: message` (the fallback)
//
// VS Code names ($gcc, $msCompile, $rustc, ...) are accepted too (see canonical()).
#include "tasks/problem.h"
#include <memory>
#include <string>
#include <vector>

namespace oh {

// Removes ANSI escape sequences (CSI, OSC including OSC 8 links, two-byte escapes)
// and carriage returns.
std::string strip_ansi(const std::string& s);

class ProblemCollector {
public:
    // Matcher names in priority order; unknown names are ignored.
    explicit ProblemCollector(const std::vector<std::string>& matchers);
    ~ProblemCollector();
    ProblemCollector(ProblemCollector&&) noexcept;
    ProblemCollector& operator=(ProblemCollector&&) noexcept;

    // One line of output (without the newline). ANSI codes are stripped here.
    void feed_line(const std::string& raw_line);
    // Feed arbitrary chunks; complete lines are matched, the rest is kept.
    void feed(const std::string& chunk);
    // End of output: flushes multi-line state (a pending CMake message, ...).
    void finish();

    const std::vector<Problem>& problems() const { return problems_; }
    bool uses(const std::string& matcher) const;

    // "$gcc" -> "gcc", "$msCompile" -> "msvc", "$rustc" -> "rustc", ... ("" if unknown).
    static std::string canonical(const std::string& name);

    // A cargo JSON line ({"reason":...}). Returns true when `line` is one; `display`
    // gets the text to show instead (the rendered diagnostic, possibly empty).
    static bool cargo_json_line(const std::string& line, std::string& display, std::vector<Problem>* out);

private:
    std::vector<std::string> names_;
    std::vector<Problem> problems_;
    std::string partial_;
    // gcc
    std::vector<ProblemNote> gcc_pending_;  // include chain / instantiation context
    bool gcc_last_ = false;                 // the previous line was a gcc diagnostic (notes attach)
    // cmake
    bool cmake_open_ = false;
    Problem cmake_;
    // rustc human
    bool rust_header_ = false;
    Problem rust_;
    bool rust_attached_ = false;  // rust_ already emitted; later --> lines are notes
    bool rust_panic_ = false;

    bool match_gcc(const std::string& line);
    bool match_msvc(const std::string& line);
    bool match_cmake(const std::string& line);
    bool match_rustc(const std::string& line);
    bool match_generic(const std::string& line);
    void close_cmake();
};

} // namespace oh
