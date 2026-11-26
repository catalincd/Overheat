#pragma once
// System monitor data: the POSIX sh probe script, its parser, and formatting helpers.
// Free of SSH/UI dependencies so it can be unit-tested (tests/sysmon_test.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace oh {

struct SysStats {
    bool valid = false;  // at least one sample parsed
    std::string host, user, os, kernel;
    int users = -1;      // logged-in sessions (`who`), -1 unknown

    bool has_cpu = false;  // needs two /proc/stat samples
    float cpu = 0;         // 0..100, all cores
    std::vector<float> cores;
    int ncpu = 0;

    bool has_mem = false;      // total only (macOS/BSD) leaves has_mem_detail false
    bool has_mem_detail = false;
    uint64_t mem_total = 0, mem_used = 0, mem_free = 0, mem_buffers = 0, mem_cached = 0;
    bool has_swap = false;
    uint64_t swap_total = 0, swap_used = 0;

    bool has_load = false;
    float load[3] = {0, 0, 0};
    bool has_uptime = false;
    double uptime = 0;  // seconds

    bool has_net = false;  // needs two samples
    double rx_rate = 0, tx_rate = 0;  // bytes/s, all interfaces but loopback
    uint64_t rx_total = 0, tx_total = 0;

    struct Disk {
        std::string fs, mount;
        uint64_t total = 0, used = 0, avail = 0;  // bytes
        float pct() const { return used + avail ? 100.0f * static_cast<float>(used) / static_cast<float>(used + avail) : 0; }
    };
    std::vector<Disk> disks;  // real filesystems, in df order
    int root_disk = -1;       // index of "/"
    int folder_disk = -1;     // mount holding the open folder (may equal root_disk)

    struct Proc {
        int pid = 0;
        float cpu = 0, mem = 0;
        std::string cmd;
    };
    std::vector<Proc> procs;  // top by CPU; only in detail samples
};

// Sections are `@@name` lines followed by command output. `folder` (may be empty)
// gets its own df. `first` adds the static facts (host, OS, kernel); `detail` adds ps.
std::string sysmon_script(const std::string& folder, bool first, bool detail);

// Turns successive raw samples into stats; keeps the counters needed for rates.
class SysSampler {
public:
    // `now` is a local monotonic time in seconds, used when the remote has no uptime.
    SysStats parse(const std::string& raw, double now);
    void reset() { *this = SysSampler(); }

private:
    struct CpuTimes { uint64_t total = 0, idle = 0; };
    std::vector<CpuTimes> prev_cpu_;  // [0] = aggregate, then cpu0..N
    uint64_t prev_rx_ = 0, prev_tx_ = 0;
    double prev_time_ = -1;
    std::string host_, user_, os_, kernel_;  // static facts arrive once
};

namespace sysfmt {
std::string bytes(uint64_t b);           // "15.6G", "512M", "980K"
std::string rate(double bytes_per_s);    // "1.2M/s"
std::string duration(double seconds);    // "3d 4h", "5h 12m", "12m"
} // namespace sysfmt

} // namespace oh
