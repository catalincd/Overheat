#include "sysmon.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

namespace oh {

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> fields(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

uint64_t to_u64(std::string_view s) { return std::strtoull(std::string(s).c_str(), nullptr, 10); }
double to_f(std::string_view s) { return std::strtod(std::string(s).c_str(), nullptr); }

// Splits the raw output into named sections of lines.
struct Sections {
    std::vector<std::pair<std::string, std::vector<std::string_view>>> list;
    const std::vector<std::string_view>* get(const char* name) const {
        for (auto& [n, lines] : list)
            if (n == name) return &lines;
        return nullptr;
    }
};

Sections split_sections(const std::string& raw) {
    Sections s;
    size_t start = 0;
    while (start < raw.size()) {
        size_t nl = raw.find('\n', start);
        std::string_view line(raw.data() + start, (nl == std::string::npos ? raw.size() : nl) - start);
        start = nl == std::string::npos ? raw.size() : nl + 1;
        if (line.size() > 2 && line[0] == '@' && line[1] == '@') s.list.push_back({std::string(trim(line.substr(2))), {}});
        else if (!s.list.empty() && !trim(line).empty()) s.list.back().second.push_back(line);
    }
    return s;
}

std::string first_line(const Sections& s, const char* name) {
    auto* l = s.get(name);
    return l && !l->empty() ? std::string(trim(l->front())) : std::string();
}

bool pseudo_fs(std::string_view fs, std::string_view mount) {
    static const char* names[] = {"tmpfs", "devtmpfs", "udev", "none", "shm", "efivarfs", "proc", "sysfs",
                                  "devfs", "cgroup", "cgroup2", "run", "map", "fdesc", "linprocfs", "securityfs"};
    for (const char* n : names)
        if (fs == n) return true;
    if (fs.rfind("map ", 0) == 0) return true;  // macOS automounter
    auto under = [&](std::string_view p) {
        return mount == p || (mount.size() > p.size() && mount.rfind(p, 0) == 0 && mount[p.size()] == '/');
    };
    if (under("/dev") || under("/sys") || under("/proc") || under("/snap")) return true;
    if (under("/run") && !under("/run/media")) return true;
    if (under("/System/Volumes") && mount != "/System/Volumes/Data") return true;
    return false;
}

// One `df -P -k` row: fs blocks used avail capacity mount (the mount may contain spaces).
bool parse_df_row(std::string_view line, SysStats::Disk& d) {
    auto f = fields(line);
    // The header ("1024-blocks Used ... Capacity") has no "NN%" column.
    if (f.size() < 6 || f[4].empty() || f[4].back() != '%' || f[1].find_first_not_of("0123456789") != std::string_view::npos)
        return false;
    d.fs = std::string(f[0]);
    d.total = to_u64(f[1]) * 1024;
    d.used = to_u64(f[2]) * 1024;
    d.avail = to_u64(f[3]) * 1024;
    size_t mpos = static_cast<size_t>(f[5].data() - line.data());
    d.mount = std::string(trim(line.substr(mpos)));
    return true;
}

} // namespace

std::string sysmon_script(const std::string& folder, bool first, bool detail) {
    std::string s =
        "exec 2>/dev/null; LC_ALL=C; export LC_ALL\n";
    if (first) {
        s += "echo @@host; uname -n\n"
             "echo @@kernel; uname -srm\n"
             "echo @@user; id -un\n"
             "echo @@os; if [ -r /etc/os-release ]; then (. /etc/os-release; echo \"$PRETTY_NAME\");"
             " elif command -v sw_vers >/dev/null; then echo \"$(sw_vers -productName) $(sw_vers -productVersion)\";"
             " else uname -sr; fi\n";
    }
    s += "echo @@ncpu; getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu\n"
         "echo @@stat; grep '^cpu' /proc/stat\n"
         "echo @@meminfo; cat /proc/meminfo || sysctl -n hw.memsize || sysctl -n hw.physmem\n"
         "echo @@loadavg; cat /proc/loadavg || sysctl -n vm.loadavg\n"
         "echo @@uptime; cat /proc/uptime || { b=$(sysctl -n kern.boottime | sed 's/^[^0-9]*\\([0-9]*\\).*/\\1/');"
         " [ -n \"$b\" ] && echo $(( $(date +%s) - b )); }\n"
         "echo @@net; cat /proc/net/dev\n"
         "echo @@who; who | wc -l\n"
         "echo @@df; df -P -k\n";
    if (!folder.empty()) s += "echo @@dfcwd; df -P -k " + shell_quote(folder) + "\n";
    if (detail) s += "echo @@ps; ps -Ao pid=,pcpu=,pmem=,comm=\n";
    s += "exit 0\n";
    return s;
}

SysStats SysSampler::parse(const std::string& raw, double now) {
    SysStats st;
    Sections sec = split_sections(raw);
    if (sec.list.empty()) return st;
    st.valid = true;

    if (auto v = first_line(sec, "host"); !v.empty()) host_ = v;
    if (auto v = first_line(sec, "user"); !v.empty()) user_ = v;
    if (auto v = first_line(sec, "os"); !v.empty()) os_ = v;
    if (auto v = first_line(sec, "kernel"); !v.empty()) kernel_ = v;
    st.host = host_;
    st.user = user_;
    st.os = os_;
    st.kernel = kernel_;
    if (auto v = first_line(sec, "who"); !v.empty()) st.users = std::atoi(v.c_str());
    if (auto v = first_line(sec, "ncpu"); !v.empty()) st.ncpu = std::atoi(v.c_str());

    // Uptime first: it is the remote clock used for rates.
    bool remote_clock = false;  // /proc/uptime has centisecond resolution
    if (auto v = first_line(sec, "uptime"); !v.empty()) {
        auto f = fields(v);
        if (!f.empty() && to_f(f[0]) > 0) {
            st.has_uptime = true;
            st.uptime = to_f(f[0]);
            remote_clock = f.size() >= 2;
        }
    }
    double t = remote_clock ? st.uptime : now;
    double dt = prev_time_ >= 0 ? t - prev_time_ : 0;
    prev_time_ = t;

    // CPU: /proc/stat aggregate and per-core jiffies.
    if (auto* lines = sec.get("stat")) {
        std::vector<CpuTimes> cur;
        for (auto line : *lines) {
            auto f = fields(line);
            if (f.size() < 5 || f[0].rfind("cpu", 0) != 0) continue;
            CpuTimes c;
            for (size_t i = 1; i < f.size() && i <= 8; i++) c.total += to_u64(f[i]);
            c.idle = to_u64(f[4]) + (f.size() > 5 ? to_u64(f[5]) : 0);
            cur.push_back(c);
        }
        if (!cur.empty() && prev_cpu_.size() == cur.size()) {
            auto pct = [](const CpuTimes& a, const CpuTimes& b) {
                uint64_t dt = b.total > a.total ? b.total - a.total : 0;
                uint64_t di = b.idle > a.idle ? b.idle - a.idle : 0;
                return dt ? std::clamp(100.0f * static_cast<float>(dt - std::min(di, dt)) / static_cast<float>(dt),
                                       0.0f, 100.0f)
                          : 0.0f;
            };
            st.has_cpu = true;
            st.cpu = pct(prev_cpu_[0], cur[0]);
            for (size_t i = 1; i < cur.size(); i++) st.cores.push_back(pct(prev_cpu_[i], cur[i]));
        }
        if (cur.size() > 1 && st.ncpu <= 0) st.ncpu = static_cast<int>(cur.size() - 1);
        prev_cpu_ = std::move(cur);
    }

    // Memory: Linux meminfo (kB), or a bare byte count from sysctl.
    if (auto* lines = sec.get("meminfo")) {
        uint64_t total = 0, free = 0, avail = 0, buffers = 0, cached = 0, sreclaim = 0, stotal = 0, sfree = 0;
        bool have_avail = false, bare = false;
        for (auto line : *lines) {
            auto f = fields(line);
            if (f.size() == 1 && f[0][0] >= '0' && f[0][0] <= '9') {
                total = to_u64(f[0]);
                bare = true;
                break;
            }
            if (f.size() < 2) continue;
            uint64_t v = to_u64(f[1]) * 1024;
            if (f[0] == "MemTotal:") total = v;
            else if (f[0] == "MemFree:") free = v;
            else if (f[0] == "MemAvailable:") { avail = v; have_avail = true; }
            else if (f[0] == "Buffers:") buffers = v;
            else if (f[0] == "Cached:") cached = v;
            else if (f[0] == "SReclaimable:") sreclaim = v;
            else if (f[0] == "SwapTotal:") stotal = v;
            else if (f[0] == "SwapFree:") sfree = v;
        }
        if (total) {
            st.has_mem = true;
            st.mem_total = total;
            if (!bare) {
                st.has_mem_detail = true;
                cached += sreclaim;
                if (!have_avail) avail = std::min(total, free + buffers + cached);
                st.mem_used = total - std::min(avail, total);
                st.mem_free = free;
                st.mem_buffers = buffers;
                st.mem_cached = cached;
                st.has_swap = stotal > 0;
                st.swap_total = stotal;
                st.swap_used = stotal - std::min(sfree, stotal);
            }
        }
    }

    if (auto v = first_line(sec, "loadavg"); !v.empty()) {
        std::string s(v);
        for (auto& c : s)
            if (c == '{' || c == '}' || c == ',') c = ' ';
        auto f = fields(s);
        if (f.size() >= 3) {
            st.has_load = true;
            for (int i = 0; i < 3; i++) st.load[i] = static_cast<float>(to_f(f[static_cast<size_t>(i)]));
        }
    }

    // Network: /proc/net/dev, all interfaces except loopback.
    if (auto* lines = sec.get("net")) {
        uint64_t rx = 0, tx = 0;
        bool any = false;
        for (auto line : *lines) {
            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            auto name = trim(line.substr(0, colon));
            if (name == "lo") continue;
            auto f = fields(line.substr(colon + 1));
            if (f.size() < 9) continue;
            rx += to_u64(f[0]);
            tx += to_u64(f[8]);
            any = true;
        }
        if (any) {
            st.rx_total = rx;
            st.tx_total = tx;
            if (dt > 0 && (prev_rx_ || prev_tx_)) {
                st.has_net = true;
                st.rx_rate = rx >= prev_rx_ ? static_cast<double>(rx - prev_rx_) / dt : 0;
                st.tx_rate = tx >= prev_tx_ ? static_cast<double>(tx - prev_tx_) / dt : 0;
            }
            prev_rx_ = rx;
            prev_tx_ = tx;
        }
    }

    if (auto* lines = sec.get("df")) {
        for (auto line : *lines) {
            SysStats::Disk d;
            if (!parse_df_row(line, d) || d.total == 0 || pseudo_fs(d.fs, d.mount)) continue;
            bool dup = false;  // bind mounts of the same mount point
            for (auto& o : st.disks) dup |= o.mount == d.mount;
            if (dup) continue;
            if (d.mount == "/") st.root_disk = static_cast<int>(st.disks.size());
            st.disks.push_back(std::move(d));
        }
    }
    if (auto* lines = sec.get("dfcwd")) {
        for (auto line : *lines) {
            SysStats::Disk d;
            if (!parse_df_row(line, d) || d.total == 0) continue;
            for (size_t i = 0; i < st.disks.size(); i++)
                if (st.disks[i].mount == d.mount) st.folder_disk = static_cast<int>(i);
            if (st.folder_disk < 0) {
                st.folder_disk = static_cast<int>(st.disks.size());
                st.disks.push_back(std::move(d));
            }
            break;
        }
    }

    if (auto* lines = sec.get("ps")) {
        for (auto line : *lines) {
            auto f = fields(line);
            if (f.size() < 4) continue;
            SysStats::Proc p;
            p.pid = std::atoi(std::string(f[0]).c_str());
            p.cpu = static_cast<float>(to_f(f[1]));
            p.mem = static_cast<float>(to_f(f[2]));
            size_t cpos = static_cast<size_t>(f[3].data() - line.data());
            std::string cmd(trim(line.substr(cpos)));
            if (auto slash = cmd.rfind('/'); slash != std::string::npos && slash + 1 < cmd.size() && cmd[0] == '/')
                cmd = cmd.substr(slash + 1);  // macOS prints the full path
            p.cmd = std::move(cmd);
            if (p.pid > 0) st.procs.push_back(std::move(p));
        }
        std::stable_sort(st.procs.begin(), st.procs.end(),
                         [](const SysStats::Proc& a, const SysStats::Proc& b) { return a.cpu > b.cpu; });
        if (st.procs.size() > 5) st.procs.resize(5);
    }
    return st;
}

namespace sysfmt {

std::string bytes(uint64_t b) {
    static const char* units = "BKMGTP";
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1000.0 && u < 5) {
        v /= 1024.0;
        u++;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%lluB", static_cast<unsigned long long>(b));
    else std::snprintf(buf, sizeof buf, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
    return buf;
}

std::string rate(double bps) {
    if (bps < 1.0) return "0B/s";
    return bytes(static_cast<uint64_t>(bps)) + "/s";
}

std::string duration(double s) {
    long long t = static_cast<long long>(s);
    long long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
    char buf[48];
    if (d > 0) std::snprintf(buf, sizeof buf, "%lldd %lldh", d, h);
    else if (h > 0) std::snprintf(buf, sizeof buf, "%lldh %lldm", h, m);
    else std::snprintf(buf, sizeof buf, "%lldm", m);
    return buf;
}

} // namespace sysfmt

} // namespace oh
