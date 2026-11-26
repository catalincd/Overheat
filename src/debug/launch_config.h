#pragma once
// Launch configurations: what to debug and with which adapter. They come from
// three places, in this order in the picker:
//   1. .overheat/launch.toml   (our format, see README "Debugging")
//   2. .vscode/launch.json     (cppdbg / cppvsdbg / lldb / lldb-dap / gdb types)
//   3. auto-detected targets   (Cargo binaries, CMake executables)
#include "types.h"
#include <map>
#include <string>
#include <vector>

namespace oh {

class Json;

// Which debug adapter runs a configuration.
enum class AdapterKind {
    Gdb,      // gdb -i dap (gdb >= 14)
    RustGdb,  // gdb -i dap plus the Rust toolchain's pretty printers (like rust-gdb)
    LldbDap,  // lldb-dap (formerly lldb-vscode)
    CodeLldb, // CodeLLDB's adapter, over stdio
};
const char* adapter_name(AdapterKind k);
// "gdb", "rust-gdb", "lldb-dap", "lldb", "codelldb", "cppdbg"... ; false if unknown.
bool parse_adapter(const std::string& s, AdapterKind& out);

struct LaunchConfig {
    std::string name;
    std::string origin;  // "launch.toml", "launch.json", "Cargo", "CMake"
    AdapterKind adapter = AdapterKind::Gdb;
    std::string adapter_path;  // overrides the settings' path for this config
    bool attach = false;
    std::string program;  // absolute after resolve()
    std::vector<std::string> args;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;  // added to the inherited environment
    bool stop_at_entry = false;
    long pid = 0;  // attach: process id (0 = ask)
    // Pre-launch build: a command line (split on spaces, quotes group) run in build_cwd.
    std::string build_command;
    std::string build_cwd;
    std::string pre_launch_task;  // from launch.json; informational until tasks exist
    std::string language;         // "rust" when known (panic breakpoint, printers)
};

// ${workspaceFolder}-style substitution context.
struct LaunchVars {
    fs::path workspace;
    fs::path file;  // the active editor's file (may be empty)
};
std::string substitute_vars(const std::string& s, const LaunchVars& v);
// Substitutes variables in every string field and makes program/cwd absolute.
void resolve_config(LaunchConfig& c, const LaunchVars& v);

// Split a command line on spaces; double quotes group words.
std::vector<std::string> split_command(const std::string& cmd);

bool parse_launch_toml(const std::string& text, std::vector<LaunchConfig>& out, std::string* error = nullptr);
bool parse_vscode_launch(const std::string& text, std::vector<LaunchConfig>& out, std::string* error = nullptr);
// Cargo: binaries from `cargo metadata` output (JSON) or, without it, Cargo.toml.
std::vector<LaunchConfig> cargo_configs_from_metadata(const Json& metadata);
std::vector<LaunchConfig> cargo_configs_from_manifest(const std::string& cargo_toml, const fs::path& root);
// CMake: executables of a configured build directory (file API reply, build.ninja,
// or executables named like add_executable() targets).
std::vector<LaunchConfig> cmake_configs(const fs::path& root, const fs::path& build_dir);
std::vector<fs::path> cmake_build_dirs(const fs::path& root);

// Every configuration for a project, in picker order. Runs `cargo metadata` when
// there is a Cargo.toml (a short subprocess).
std::vector<LaunchConfig> discover_launch_configs(const fs::path& root, std::vector<std::string>* problems = nullptr);

// Template written by "Debug: Open launch.toml" when the project has none.
std::string launch_toml_template(const std::vector<LaunchConfig>& detected);

} // namespace oh
