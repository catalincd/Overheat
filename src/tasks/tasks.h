#pragma once
// Build tasks: definitions, auto-detection (CMake with presets, Make, Cargo), the
// project task file `.overheat/tasks.toml`, and an import of `.vscode/tasks.json`.
//
// .overheat/tasks.toml:
//   [[task]]
//   label = "build"                  # required; shown in Tasks: Run Task...
//   command = "make -j8"             # a shell command line (sh -c / cmd /c), or
//   program = "cargo"                # a program run directly,
//   args = ["build", "--release"]    #   with its arguments
//   cwd = "${workspaceFolder}/sub"   # default: the project folder
//   env = { RUST_BACKTRACE = "1" }
//   group = "build"                  # build | test | run
//   default = true                   # Ctrl+Shift+B (build) / Ctrl+F5 (run) pick it
//   matcher = ["gcc"]                # gcc msvc cmake rustc cargo-json generic none
//   depends_on = ["configure"]       # labels run first, in order
//
// Variables in command, args, cwd and env: ${workspaceFolder},
// ${workspaceFolderBasename}, ${file}, ${fileDirname}, ${fileBasename},
// ${fileBasenameNoExtension}, ${relativeFile}, ${lineNumber}, ${env:NAME}.
#include "types.h"
#include <string>
#include <utility>
#include <vector>

namespace oh {

struct TaskDef {
    std::string label;
    std::string source;  // "tasks.toml", "tasks.json", "CMake", "Make", "Cargo"
    bool shell = false;  // command is a shell command line; else a program + args
    std::string command;
    std::vector<std::string> args;
    std::string cwd;     // may hold variables; "" = the project folder
    std::vector<std::pair<std::string, std::string>> env;
    std::string group;   // "build", "test", "run" or ""
    bool is_default = false;
    std::vector<std::string> matchers;  // ProblemCollector names
    std::vector<std::string> depends_on;
    std::string detail;  // one-line description for the picker
};

struct TaskContext {
    fs::path workspace;  // the project folder
    fs::path file;       // the active editor's file (may be empty)
    size_t line = 0;     // 1-based cursor line
};

std::string expand_task_vars(const std::string& s, const TaskContext& ctx);

// argv to run: shell tasks go through /bin/sh -c (cmd.exe /d /s /c on Windows).
std::vector<std::string> task_argv(const TaskDef& t, const TaskContext& ctx);
fs::path task_cwd(const TaskDef& t, const TaskContext& ctx);
// "cargo build --message-format=..." as shown in the terminal banner.
std::string task_command_line(const TaskDef& t, const TaskContext& ctx);

// Auto-detected tasks for a project folder (CMake, Make, Cargo).
std::vector<TaskDef> detect_tasks(const fs::path& root);
// Parsers, exposed for tests. Return false with *error on a malformed file.
bool parse_tasks_toml(const std::string& text, std::vector<TaskDef>& out, std::string* error = nullptr);
bool parse_vscode_tasks(const std::string& text, std::vector<TaskDef>& out, std::string* error = nullptr);
// Make targets worth offering (no pattern rules, no variables, no special targets).
std::vector<std::string> make_targets(const std::string& makefile_text);

// Everything for `root`: tasks.toml first, then tasks.json, then detected tasks
// whose labels are not taken. Problems reading the files go to *errors.
std::vector<TaskDef> load_tasks(const fs::path& root, std::vector<std::string>* errors = nullptr);

// The default task of a group: the one marked default, else the first in the group.
const TaskDef* default_task(const std::vector<TaskDef>& tasks, const std::string& group);
const TaskDef* find_task(const std::vector<TaskDef>& tasks, const std::string& label);

// Where the project task file lives.
inline fs::path tasks_file(const fs::path& root) { return root / ".overheat" / "tasks.toml"; }

} // namespace oh
