// Parses canned Linux and macOS monitor samples and checks rates, filters and fallbacks.
#include "sysmon.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace oh;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static bool near(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

static const char* LINUX_A =
    "@@host\nbox\n@@kernel\nLinux 6.1.0 x86_64\n@@user\nalice\n@@os\nDebian GNU/Linux 12 (bookworm)\n"
    "@@ncpu\n2\n"
    "@@stat\ncpu  100 0 100 800 0 0 0 0 0 0\ncpu0 50 0 50 400 0 0 0 0 0 0\ncpu1 50 0 50 400 0 0 0 0 0 0\n"
    "@@meminfo\nMemTotal:       16000000 kB\nMemFree:         2000000 kB\nMemAvailable:    8000000 kB\n"
    "Buffers:          500000 kB\nCached:          4000000 kB\nSReclaimable:     500000 kB\n"
    "SwapTotal:       4000000 kB\nSwapFree:        3000000 kB\n"
    "@@loadavg\n0.50 0.40 0.30 1/234 5678\n"
    "@@uptime\n1000.00 1900.00\n"
    "@@net\nInter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
    "    lo: 999999 10 0 0 0 0 0 0 999999 10 0 0 0 0 0 0\n"
    "  eth0: 1000000 10 0 0 0 0 0 0 500000 10 0 0 0 0 0 0\n"
    "@@who\n3\n"
    "@@df\nFilesystem     1024-blocks      Used Available Capacity Mounted on\n"
    "/dev/sda1        100000000  60000000  40000000      60% /\n"
    "tmpfs              1000000         0   1000000       0% /dev/shm\n"
    "devtmpfs           1000000         0   1000000       0% /dev\n"
    "/dev/sdb1        200000000  50000000 150000000      25% /mnt/My Data\n"
    "@@dfcwd\nFilesystem     1024-blocks      Used Available Capacity Mounted on\n"
    "/dev/sdb1        200000000  50000000 150000000      25% /mnt/My Data\n";

// Two seconds later: cpu0 fully busy, cpu1 idle; 2 MB received, 1 MB sent.
static const char* LINUX_B =
    "@@ncpu\n2\n"
    "@@stat\ncpu  300 0 100 1000 0 0 0 0 0 0\ncpu0 250 0 50 400 0 0 0 0 0 0\ncpu1 50 0 50 600 0 0 0 0 0 0\n"
    "@@meminfo\nMemTotal:       16000000 kB\nMemAvailable:    8000000 kB\n"
    "@@loadavg\n0.50 0.40 0.30 1/234 5678\n"
    "@@uptime\n1002.00 1903.00\n"
    "@@net\n  eth0: 3000000 10 0 0 0 0 0 0 1500000 10 0 0 0 0 0 0\n"
    "@@df\n/dev/sda1        100000000  60000000  40000000      60% /\n"
    "@@ps\n  1   0.0  0.1 systemd\n 42  12.5  3.0 cc1plus\n 77  99.0  1.0 /usr/bin/stress\n"
    "  5   1.0  0.0 a\n  6   2.0  0.0 b\n  7   3.0  0.0 c\n";

static const char* MACOS =
    "@@host\nmac\n@@os\nmacOS 14.5\n@@ncpu\n8\n@@stat\n@@meminfo\n17179869184\n"
    "@@loadavg\n{ 1.23 1.10 1.05 }\n@@uptime\n86400\n@@net\n@@who\n1\n"
    "@@df\nFilesystem   1024-blocks      Used Available Capacity  Mounted on\n"
    "/dev/disk3s1s1   482797652  10000000 300000000     4%    /\n"
    "devfs                  201       201         0   100%    /dev\n"
    "/dev/disk3s6     482797652   2000000 300000000     1%    /System/Volumes/VM\n"
    "/dev/disk3s5     482797652 160000000 300000000    35%    /System/Volumes/Data\n"
    "map auto_home            0         0         0   100%    /System/Volumes/Data/home\n";

int main() {
    SysSampler s;
    SysStats a = s.parse(LINUX_A, 0);
    CHECK(a.valid);
    CHECK(a.host == "box" && a.user == "alice" && a.os == "Debian GNU/Linux 12 (bookworm)");
    CHECK(!a.has_cpu);  // needs a delta
    CHECK(!a.has_net);
    CHECK(a.ncpu == 2);
    CHECK(a.has_mem && a.has_mem_detail);
    CHECK(a.mem_total == 16000000ull * 1024);
    CHECK(a.mem_used == 8000000ull * 1024);
    CHECK(a.mem_cached == 4500000ull * 1024);
    CHECK(a.has_swap && a.swap_used == 1000000ull * 1024);
    CHECK(a.has_load && near(a.load[0], 0.5) && near(a.load[2], 0.3));
    CHECK(a.has_uptime && near(a.uptime, 1000));
    CHECK(a.users == 3);
    CHECK(a.disks.size() == 2);  // tmpfs and devtmpfs filtered out
    CHECK(a.root_disk == 0 && a.disks[0].mount == "/");
    CHECK(near(a.disks[0].pct(), 60.0, 0.1));
    CHECK(a.folder_disk == 1 && a.disks[1].mount == "/mnt/My Data");
    CHECK(a.procs.empty());

    SysStats b = s.parse(LINUX_B, 100);  // remote uptime is the clock, not 100
    CHECK(b.host == "box");  // static facts carried over
    CHECK(b.has_cpu);
    CHECK(near(b.cpu, 50.0, 0.1));  // 200 busy of 400
    CHECK(b.cores.size() == 2 && near(b.cores[0], 100.0, 0.1) && near(b.cores[1], 0.0, 0.1));
    CHECK(b.has_net && near(b.rx_rate, 1000000.0, 1) && near(b.tx_rate, 500000.0, 1));
    CHECK(!b.has_swap);
    CHECK(b.folder_disk == -1);
    CHECK(b.procs.size() == 5);
    CHECK(b.procs[0].cmd == "stress" && b.procs[0].pid == 77 && near(b.procs[0].cpu, 99.0));
    CHECK(b.procs[1].cmd == "cc1plus");

    SysSampler m;
    SysStats c = m.parse(MACOS, 0);
    CHECK(c.valid && c.host == "mac" && c.ncpu == 8);
    CHECK(c.has_mem && !c.has_mem_detail && c.mem_total == 17179869184ull);
    CHECK(c.has_load && near(c.load[0], 1.23));
    CHECK(c.has_uptime && near(c.uptime, 86400));
    CHECK(!c.has_cpu && !c.has_net);
    CHECK(c.disks.size() == 2);
    CHECK(c.root_disk == 0 && c.disks[1].mount == "/System/Volumes/Data");

    CHECK(!SysSampler().parse("", 0).valid);
    CHECK(!SysSampler().parse("garbage\nmore\n", 0).valid);

    CHECK(sysfmt::bytes(512) == "512B");
    CHECK(sysfmt::bytes(16000000ull * 1024) == "15G");
    CHECK(sysfmt::bytes(1536ull * 1024 * 1024) == "1.5G");
    CHECK(sysfmt::duration(3 * 86400 + 4 * 3600 + 60) == "3d 4h");
    CHECK(sysfmt::duration(5 * 3600 + 12 * 60) == "5h 12m");
    CHECK(sysfmt::rate(0.2) == "0B/s");

    std::string sc = sysmon_script("/srv/it's", true, true);
    CHECK(sc.find("@@ps") != std::string::npos && sc.find("'/srv/it'\\''s'") != std::string::npos);
    CHECK(sysmon_script("", false, false).find("@@host") == std::string::npos);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("sysmon_test: ok\n");
    return failures ? 1 : 0;
}
