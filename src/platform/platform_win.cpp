#include "platform/platform.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>

namespace oh::platform {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Quote one argument by the rules CommandLineToArgvW / the CRT use.
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') { i++; backslashes++; }
        if (i == a.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += a[i];
        }
    }
    return out + L"\"";
}

std::wstring command_line(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quote_arg(widen(argv[i]));
    }
    return cmd;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

} // namespace

void init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleOutputCP(CP_UTF8);
}

void shutdown() { WSACleanup(); }
int process_id() { return static_cast<int>(GetCurrentProcessId()); }

std::string getenv_str(const char* name) {
    std::wstring wname = widen(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return "";
    std::wstring v(n, L'\0');
    GetEnvironmentVariableW(wname.c_str(), v.data(), n);
    v.resize(n - 1);
    return narrow(v);
}

fs::path home_dir() {
    std::wstring p = known_folder(FOLDERID_Profile);
    return p.empty() ? fs::path(getenv_str("USERPROFILE")) : fs::path(p);
}

fs::path config_dir() {
    std::wstring p = known_folder(FOLDERID_RoamingAppData);
    return (p.empty() ? home_dir() / "AppData" / "Roaming" : fs::path(p)) / "Overheat";
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::string user_name() {
    std::string u = getenv_str("USERNAME");
    return u.empty() ? "user" : u;
}

std::vector<std::string> environment() {
    std::vector<std::string> out;
    wchar_t* block = GetEnvironmentStringsW();
    for (wchar_t* e = block; e && *e; e += wcslen(e) + 1)
        if (*e != L'=') out.push_back(narrow(e));  // skip the per-drive "=C:=C:\" entries
    if (block) FreeEnvironmentStringsW(block);
    return out;
}

void kill_process(long pid) {
    if (pid <= 0) return;
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

bool process_alive(long pid) {
    if (pid <= 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

std::vector<std::string> default_shell() {
    // PowerShell 7 if installed, then Windows PowerShell, then cmd.
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, found, nullptr)) return {narrow(found), "-NoLogo"};
    if (SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH, found, nullptr)) return {narrow(found), "-NoLogo"};
    std::string comspec = getenv_str("COMSPEC");
    return {comspec.empty() ? "cmd.exe" : comspec};
}

std::vector<std::string> default_font_files() {
    std::string fonts = (fs::path(getenv_str("WINDIR").empty() ? "C:\\Windows" : getenv_str("WINDIR")) / "Fonts").string();
    std::string user_fonts = (fs::path(getenv_str("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts").string();
    return {
        (exe_dir() / "fonts" / "JetBrainsMonoNerdFontMono-Regular.ttf").string(),  // bundled by the installer
        user_fonts + "\\JetBrainsMonoNerdFontMono-Regular.ttf",
        fonts + "\\CascadiaMono.ttf",
        fonts + "\\consola.ttf",
        fonts + "\\seguisym.ttf",
        fonts + "\\segoeui.ttf",
        fonts + "\\msgothic.ttc",
    };
}

// ---------------------------------------------------------------- Process

struct Process::Impl {
    HANDLE process = nullptr;
    HANDLE in_write = nullptr, out_read = nullptr;
};

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() {
    terminate();
    wait();
}

bool Process::start(const std::vector<std::string>& argv, const fs::path& cwd, bool merge_stderr) {
    if (argv.empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) return false;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    // Our ends must not leak into the child.
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = merge_stderr ? nullptr
                              : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = merge_stderr ? out_write : nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line(argv);
    std::wstring wcwd = cwd.empty() ? L"" : cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->in_write = in_write;
    impl_->out_read = out_read;
    return true;
}

long Process::read(char* buf, size_t n) {
    if (!impl_->out_read) return -1;
    DWORD got = 0;
    if (!ReadFile(impl_->out_read, buf, static_cast<DWORD>(n), &got, nullptr)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return static_cast<long>(got);
}

bool Process::write(const char* data, size_t n) {
    while (n > 0 && impl_->in_write) {
        DWORD w = 0;
        if (!WriteFile(impl_->in_write, data, static_cast<DWORD>(n), &w, nullptr) || w == 0) return false;
        data += w;
        n -= w;
    }
    return n == 0;
}

void Process::close_stdin() {
    if (impl_->in_write) CloseHandle(impl_->in_write);
    impl_->in_write = nullptr;
}

void Process::terminate() {
    if (impl_->process) TerminateProcess(impl_->process, 1);
}

int Process::wait() {
    close_stdin();
    int code = -1;
    if (impl_->process) {
        WaitForSingleObject(impl_->process, INFINITE);
        DWORD c = 0;
        if (GetExitCodeProcess(impl_->process, &c)) code = static_cast<int>(c);
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
    if (impl_->out_read) CloseHandle(impl_->out_read);
    impl_->out_read = nullptr;
    return code;
}

bool Process::running() const { return impl_->process != nullptr; }
long Process::pid() const { return impl_->process ? static_cast<long>(GetProcessId(impl_->process)) : 0; }

// ---------------------------------------------------------------- Pty (ConPTY)

struct Pty::Impl {
    HPCON console = nullptr;
    HANDLE process = nullptr;
    HANDLE in_write = nullptr, out_read = nullptr;
};

Pty::Pty() : impl_(std::make_unique<Impl>()) {}
Pty::~Pty() { kill(); }

bool Pty::spawn(const std::vector<std::string>& argv_in, const fs::path& cwd, int cols, int rows) {
    kill();
    std::vector<std::string> argv = argv_in.empty() ? default_shell() : argv_in;
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, nullptr, 0)) return false;
    if (!CreatePipe(&out_read, &out_write, nullptr, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HPCON hpc = nullptr;
    HRESULT hr = CreatePseudoConsole(size, in_read, out_write, 0, &hpc);
    CloseHandle(in_read);    // the console owns these now
    CloseHandle(out_write);
    if (FAILED(hr)) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(hpc), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // Null std handles: otherwise a parent with redirected stdio (a launcher, a log
    // file) hands those to the child and its output bypasses the pseudoconsole.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line(argv);
    std::wstring wcwd = cwd.empty() ? L"" : cwd.wstring();
    SetEnvironmentVariableW(L"TERM", L"xterm-256color");
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        ClosePseudoConsole(hpc);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->console = hpc;
    impl_->process = pi.hProcess;
    impl_->in_write = in_write;
    impl_->out_read = out_read;
    return true;
}

long Pty::read(char* buf, size_t n, int timeout_ms) {
    if (!impl_->out_read) return -1;
    // Anonymous pipes have no timed read: poll for data, then read what is there.
    DWORD avail = 0;
    for (int waited = 0;; waited += 5) {
        if (!PeekNamedPipe(impl_->out_read, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (avail > 0) break;
        if (waited >= timeout_ms) return 0;
        Sleep(5);
    }
    DWORD got = 0;
    if (!ReadFile(impl_->out_read, buf, static_cast<DWORD>(std::min<size_t>(n, avail)), &got, nullptr)) return -1;
    return static_cast<long>(got);
}

bool Pty::write(const char* data, size_t n) {
    while (n > 0 && impl_->in_write) {
        DWORD w = 0;
        if (!WriteFile(impl_->in_write, data, static_cast<DWORD>(n), &w, nullptr) || w == 0) return false;
        data += w;
        n -= w;
    }
    return n == 0;
}

void Pty::resize(int cols, int rows) {
    if (impl_->console) ResizePseudoConsole(impl_->console, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
}

bool Pty::exited() {
    if (!impl_->process) return true;
    return WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0;
}

void Pty::kill() {
    // Closing the console ends the session; the shell then exits.
    if (impl_->console) ClosePseudoConsole(impl_->console);
    impl_->console = nullptr;
    if (impl_->process) {
        if (WaitForSingleObject(impl_->process, 500) != WAIT_OBJECT_0) TerminateProcess(impl_->process, 1);
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
    if (impl_->in_write) CloseHandle(impl_->in_write);
    if (impl_->out_read) CloseHandle(impl_->out_read);
    impl_->in_write = impl_->out_read = nullptr;
}

// ---------------------------------------------------------------- libraries

void* load_library(const fs::path& path) { return reinterpret_cast<void*>(LoadLibraryW(path.wstring().c_str())); }
void* find_symbol(void* lib, const char* name) {
    return lib ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)) : nullptr;
}
void close_library(void* lib) {
    if (lib) FreeLibrary(static_cast<HMODULE>(lib));
}
const char* library_extension() { return ".dll"; }

// ---------------------------------------------------------------- system monitor

std::string local_system_sample(const std::string&, const std::string& folder, bool) {
    std::string out;
    char buf[512];
    auto u64 = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    out += "@@host\n" + getenv_str("COMPUTERNAME") + "\n@@user\n" + user_name() + "\n@@os\nWindows\n";
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    out += "@@ncpu\n" + std::to_string(si.dwNumberOfProcessors) + "\n";
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {  // kernel time includes idle time
        std::snprintf(buf, sizeof buf, "@@stat\ncpu %llu 0 %llu %llu\n", u64(user), u64(kernel) - u64(idle), u64(idle));
        out += buf;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        std::snprintf(buf, sizeof buf, "@@meminfo\nMemTotal: %llu kB\nMemFree: %llu kB\nMemAvailable: %llu kB\n",
                      ms.ullTotalPhys / 1024, ms.ullAvailPhys / 1024, ms.ullAvailPhys / 1024);
        out += buf;
    }
    std::snprintf(buf, sizeof buf, "@@uptime\n%.1f\n", static_cast<double>(GetTickCount64()) / 1000.0);
    out += buf;
    // Fixed drives as `df -P -k` rows; the folder's drive again under @@dfcwd.
    std::string rows, cwd_row;
    char drive = folder.size() >= 2 && folder[1] == ':' ? static_cast<char>(folder[0] & ~0x20) : 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER avail{}, total{}, free_bytes{};
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes)) continue;
        unsigned long long t = total.QuadPart / 1024, f = free_bytes.QuadPart / 1024, a = avail.QuadPart / 1024;
        std::snprintf(buf, sizeof buf, "%c: %llu %llu %llu %llu%% %c:\\\n", 'A' + i, t, t - f, a,
                      t ? (t - f) * 100 / t : 0ull, 'A' + i);
        rows += buf;
        if (drive == 'A' + i) cwd_row = buf;
    }
    out += "@@df\n" + rows;
    if (!cwd_row.empty()) out += "@@dfcwd\n" + cwd_row;
    return out;
}

} // namespace oh::platform
#endif
