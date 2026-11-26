#include "platform/platform.h"
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oh::platform {

void init() { std::signal(SIGPIPE, SIG_IGN); }  // children may exit mid-write: report EPIPE instead
void shutdown() {}
int process_id() { return static_cast<int>(getpid()); }

std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

fs::path home_dir() {
    if (auto h = getenv_str("HOME"); !h.empty()) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

fs::path config_dir() {
    if (auto x = getenv_str("XDG_CONFIG_HOME"); !x.empty()) return fs::path(x) / "overheat";
    return home_dir() / ".config" / "overheat";
}

fs::path exe_dir() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : p.parent_path();
}

std::string user_name() {
    if (auto u = getenv_str("USER"); !u.empty()) return u;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_name;
    return "user";
}

std::vector<std::string> environment() {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; e++) out.emplace_back(*e);
    return out;
}

void kill_process(long pid) {
    if (pid > 0) ::kill(static_cast<pid_t>(pid), SIGKILL);
}

bool process_alive(long pid) {
    return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

std::vector<std::string> default_shell() {
    std::string sh = getenv_str("SHELL");
    return {sh.empty() ? "/bin/bash" : sh};
}

std::vector<std::string> default_font_files() {
    return {
        (exe_dir() / "fonts" / "JetBrainsMonoNerdFontMono-Regular.ttf").string(),  // bundled with packages
        (exe_dir() / ".." / "share" / "overheat" / "fonts" / "JetBrainsMonoNerdFontMono-Regular.ttf").string(),
        "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
        "/usr/share/fonts/TTF/SymbolsNerdFontMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
    };
}

// ---------------------------------------------------------------- Process

struct Process::Impl {
    pid_t pid = -1;
    int in_fd = -1, out_fd = -1;
};

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() {
    terminate();
    wait();
}

bool Process::start(const std::vector<std::string>& argv, const fs::path& cwd, bool merge_stderr) {
    if (argv.empty()) return false;
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        if (merge_stderr) dup2(out_pipe[1], 2);
        else if (int devnull = open("/dev/null", O_WRONLY); devnull >= 0) dup2(devnull, 2);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {}
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    impl_->pid = pid;
    impl_->in_fd = in_pipe[1];
    impl_->out_fd = out_pipe[0];
    return true;
}

long Process::read(char* buf, size_t n) {
    if (impl_->out_fd < 0) return -1;
    for (;;) {
        ssize_t r = ::read(impl_->out_fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return static_cast<long>(r);
    }
}

bool Process::write(const char* data, size_t n) {
    while (n > 0 && impl_->in_fd >= 0) {
        ssize_t w = ::write(impl_->in_fd, data, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        data += w;
        n -= static_cast<size_t>(w);
    }
    return n == 0;
}

void Process::close_stdin() {
    if (impl_->in_fd >= 0) close(impl_->in_fd);
    impl_->in_fd = -1;
}

void Process::terminate() {
    if (impl_->pid > 0) ::kill(impl_->pid, SIGTERM);
}

int Process::wait() {
    close_stdin();
    int status = 0;
    int code = -1;
    if (impl_->pid > 0) {
        waitpid(impl_->pid, &status, 0);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        impl_->pid = -1;
    }
    if (impl_->out_fd >= 0) close(impl_->out_fd);
    impl_->out_fd = -1;
    return code;
}

bool Process::running() const { return impl_->pid > 0; }
long Process::pid() const { return impl_->pid > 0 ? impl_->pid : 0; }

// ---------------------------------------------------------------- Pty

struct Pty::Impl {
    pid_t pid = -1;
    int fd = -1;
    int exit_code = -1;
    void reaped(int status) {
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }
};

Pty::Pty() : impl_(std::make_unique<Impl>()) {}
Pty::~Pty() { kill(); }

bool Pty::spawn(const std::vector<std::string>& argv_in, const fs::path& cwd, int cols, int rows,
               const std::vector<std::pair<std::string, std::string>>& env) {
    kill();
    impl_->exit_code = -1;
    std::vector<std::string> argv = argv_in.empty() ? default_shell() : argv_in;
    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {}
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    impl_->pid = pid;
    impl_->fd = fd;
    return true;
}

long Pty::read(char* buf, size_t n, int timeout_ms) {
    if (impl_->fd < 0) return -1;
    pollfd p{impl_->fd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    ssize_t got = ::read(impl_->fd, buf, n);
    if (got > 0) return static_cast<long>(got);
    if (got < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;  // EOF or EIO: the child side is closed
}

bool Pty::write(const char* data, size_t n) {
    while (n > 0 && impl_->fd >= 0) {
        ssize_t w = ::write(impl_->fd, data, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        data += w;
        n -= static_cast<size_t>(w);
    }
    return n == 0;
}

void Pty::resize(int cols, int rows) {
    if (impl_->fd < 0) return;
    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ioctl(impl_->fd, TIOCSWINSZ, &ws);
    if (impl_->pid > 0) ::kill(impl_->pid, SIGWINCH);
}

bool Pty::exited() {
    if (impl_->pid <= 0) return true;
    int status = 0;
    if (waitpid(impl_->pid, &status, WNOHANG) > 0) {
        impl_->pid = -1;  // reaped: never signal this pid again (it may be reused)
        impl_->reaped(status);
        return true;
    }
    return false;
}

int Pty::exit_code() {
    if (impl_->pid > 0) exited();
    return impl_->exit_code;
}

void Pty::kill() {
    if (impl_->pid > 0) {
        ::kill(impl_->pid, SIGHUP);
        // A shell still busy in its rc files can hold off SIGHUP; never block the UI on it.
        bool gone = false;
        for (int i = 0; i < 30 && !gone; i++) {
            int status = 0;
            pid_t r = waitpid(impl_->pid, &status, WNOHANG);
            if (r == impl_->pid) impl_->reaped(status);
            gone = r == impl_->pid || (r < 0 && errno != EINTR);
            if (!gone) usleep(10000);
        }
        if (!gone) {
            ::kill(-impl_->pid, SIGKILL);  // its session's process group (forkpty: pgid == pid)
            ::kill(impl_->pid, SIGKILL);
            waitpid(impl_->pid, nullptr, 0);
        }
        impl_->pid = -1;
    }
    if (impl_->fd >= 0) close(impl_->fd);
    impl_->fd = -1;
}

// ---------------------------------------------------------------- libraries

void* load_library(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* find_symbol(void* lib, const char* name) { return lib ? dlsym(lib, name) : nullptr; }
void close_library(void* lib) {
    if (lib) dlclose(lib);
}
const char* library_extension() { return ".so"; }

// ---------------------------------------------------------------- system monitor

std::string local_system_sample(const std::string& sh_script, const std::string&, bool) {
    Process p;
    if (!p.start({"/bin/sh", "-c", sh_script})) return {};
    std::string out;
    char buf[8192];
    long n;
    while ((n = p.read(buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    p.wait();
    return out;
}

} // namespace oh::platform
#endif
