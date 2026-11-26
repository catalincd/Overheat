#pragma once
#include "types.h"
#include <memory>
#include <string>
#include <vector>

// Operating-system services used by the rest of Overheat. One implementation per
// platform: platform_posix.cpp (Linux) and platform_win.cpp (Windows).
namespace oh::platform {

void init();      // process-wide setup (ignore SIGPIPE / start Winsock)
void shutdown();

fs::path home_dir();
fs::path config_dir();  // ~/.config/overheat, or %APPDATA%\Overheat
fs::path exe_dir();     // directory of the running executable
std::string user_name();
std::string getenv_str(const char* name);
int process_id();

// Default shell for the integrated terminal (argv).
std::vector<std::string> default_shell();
// Fonts tried in order when no font family is configured.
std::vector<std::string> default_font_files();

// A child process with piped stdin/stdout (stderr discarded or merged).
class Process {
public:
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool start(const std::vector<std::string>& argv, const fs::path& cwd = {}, bool merge_stderr = false);
    // Blocking read from the child's stdout: >0 bytes, 0 at EOF, <0 on error.
    long read(char* buf, size_t n);
    bool write(const char* data, size_t n);
    void close_stdin();
    void terminate();   // ask the process to stop (SIGTERM / TerminateProcess)
    int wait();         // exit code, or -1
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A pseudo-terminal running a shell or command (forkpty / ConPTY).
class Pty {
public:
    Pty();
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Empty argv runs default_shell(). `env` adds or overrides environment variables.
    bool spawn(const std::vector<std::string>& argv, const fs::path& cwd, int cols, int rows,
               const std::vector<std::pair<std::string, std::string>>& env = {});
    // Waits up to timeout_ms: >0 bytes read, 0 timeout, <0 when the child is gone.
    long read(char* buf, size_t n, int timeout_ms);
    bool write(const char* data, size_t n);
    void resize(int cols, int rows);
    bool exited();  // non-blocking check; reaps the child
    // The child's exit status once it has exited (128 + signal when killed), else -1.
    int exit_code();
    void kill();    // stop the child and release everything

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One local system-monitor sample in sysmon's `@@section` format. POSIX runs
// `sh_script` with /bin/sh; Windows builds the sections from Win32 calls (CPU,
// memory, uptime, fixed drives; no network or processes).
std::string local_system_sample(const std::string& sh_script, const std::string& folder, bool detail);

// Shared libraries (plugins).
void* load_library(const fs::path& path);
void* find_symbol(void* lib, const char* name);
void close_library(void* lib);
const char* library_extension();  // ".so" / ".dll"

} // namespace oh::platform
