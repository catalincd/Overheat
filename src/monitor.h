#pragma once
// Background system monitor for the Remote-SSH host (or the local machine).
//
// Remote mode opens its *own* SSH connection on a worker thread, with the same
// credentials as the main session and the host key pinned to the fingerprint the
// main session verified. libssh2 sessions are not safe to share between threads,
// so the monitor never touches the UI thread's session. One exec per sample
// (every ~2 s) runs a small POSIX sh probe (sysmon_script) and the output is
// parsed into SysStats; the UI thread only copies the latest snapshot.
#include "sysmon.h"
#include "ssh/session.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oh {

class SysMonitor {
public:
    enum class State { Off, Connecting, Live, Lost };
    struct Snapshot {
        SysStats stats;
        State state = State::Off;
        std::string error;
        uint64_t version = 0;
    };

    ~SysMonitor();

    // Starting while running restarts. `folder` gets its own disk entry.
    void start_remote(const SSHConfig& cfg, const std::string& folder);
    void start_local(const std::string& folder);
    void stop();  // never blocks: a worker stuck in a network call finishes on its own
    bool running() const { return shared_ != nullptr; }
    bool remote() const { return remote_; }

    void set_folder(const std::string& folder);
    // Detail samples also list processes; turning it on samples right away.
    void set_detail(bool on);

    // Latest snapshot if newer than `have_version`.
    bool poll(uint64_t have_version, Snapshot& out);
    void reap();      // join finished, stopped workers (call now and then)
    void shutdown();  // stop and join everything (app exit; may wait for a network timeout)

    static constexpr int INTERVAL_MS = 2000;

private:
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        bool stop = false, wake = false, detail = false;
        std::string folder;
        Snapshot snap;
        std::atomic<bool> done{false};
    };
    std::shared_ptr<Shared> shared_;
    std::thread worker_;
    bool remote_ = false;
    struct Retired { std::thread t; std::shared_ptr<Shared> s; };
    std::vector<Retired> retired_;

    static void run_remote(std::shared_ptr<Shared> sh, SSHConfig cfg);
    static void run_local(std::shared_ptr<Shared> sh);
};

} // namespace oh
