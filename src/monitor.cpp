#include "monitor.h"
#include "platform/platform.h"
#include <chrono>

namespace oh {

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

} // namespace

SysMonitor::~SysMonitor() { shutdown(); }

void SysMonitor::shutdown() {
    stop();
    for (auto& r : retired_)
        if (r.t.joinable()) r.t.join();
    retired_.clear();
}

void SysMonitor::stop() {
    if (!shared_) return;
    {
        std::lock_guard lk(shared_->mu);
        shared_->stop = true;
    }
    shared_->cv.notify_all();
    retired_.push_back({std::move(worker_), std::move(shared_)});
    shared_.reset();
}

void SysMonitor::reap() {
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].s->done) {
            if (retired_[i].t.joinable()) retired_[i].t.join();
            retired_.erase(retired_.begin() + static_cast<long>(i));
        } else {
            i++;
        }
    }
}

void SysMonitor::start_remote(const SSHConfig& cfg, const std::string& folder) {
    stop();
    shared_ = std::make_shared<Shared>();
    shared_->folder = folder;
    shared_->snap.state = State::Connecting;
    remote_ = true;
    worker_ = std::thread(run_remote, shared_, cfg);
}

void SysMonitor::start_local(const std::string& folder) {
    stop();
    shared_ = std::make_shared<Shared>();
    shared_->folder = folder;
    shared_->snap.state = State::Connecting;
    remote_ = false;
    worker_ = std::thread(run_local, shared_);
}

void SysMonitor::set_folder(const std::string& folder) {
    if (!shared_) return;
    std::lock_guard lk(shared_->mu);
    shared_->folder = folder;
}

void SysMonitor::set_detail(bool on) {
    if (!shared_) return;
    {
        std::lock_guard lk(shared_->mu);
        if (shared_->detail == on) return;
        shared_->detail = on;
        shared_->wake = on;
    }
    shared_->cv.notify_all();
}

bool SysMonitor::poll(uint64_t have_version, Snapshot& out) {
    if (!shared_) return false;
    std::lock_guard lk(shared_->mu);
    if (shared_->snap.version == have_version) return false;
    out = shared_->snap;
    return true;
}

namespace {

// Sleeps until the next sample is due; false once asked to stop.
template <class Shared>
bool wait_next(Shared& sh, int ms) {
    std::unique_lock lk(sh.mu);
    sh.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return sh.stop || sh.wake; });
    sh.wake = false;
    return !sh.stop;
}

template <class Shared, class Snapshot>
void publish(Shared& sh, Snapshot&& update) {
    {
        std::lock_guard lk(sh.mu);
        update(sh.snap);
        sh.snap.version++;
    }
    request_redraw();
}

} // namespace

void SysMonitor::run_remote(std::shared_ptr<Shared> sh, SSHConfig cfg) {
    SSHSession session;
    SysSampler sampler;
    bool first = true;
    int failures = 0;
    for (;;) {
        {
            std::lock_guard lk(sh->mu);
            if (sh->stop) break;
        }
        if (!session.is_connected()) {
            SSHConnectStatus st = session.connect(cfg);
            if (st != SSHConnectStatus::Ok) {
                std::string err = session.last_error();
                publish(*sh, [&](Snapshot& s) { s.state = State::Lost; s.error = err; });
                failures++;
                if (!wait_next(*sh, std::min(30000, 2000 << std::min(failures, 4)))) break;
                continue;
            }
            sampler.reset();
            first = true;
        }
        std::string folder;
        bool detail;
        {
            std::lock_guard lk(sh->mu);
            folder = sh->folder;
            detail = sh->detail;
        }
        // `sh -c` so a fish/csh login shell still runs the POSIX probe.
        auto r = session.exec("sh -c " + shell_quote(sysmon_script(folder, first, detail)));
        if (r.stdout_data.find("@@df") == std::string::npos) {  // channel failed or connection dropped
            session.disconnect();
            publish(*sh, [&](Snapshot& s) { s.state = State::Lost; s.error = "Monitor connection lost"; });
            failures++;
            if (!wait_next(*sh, std::min(30000, 1000 << std::min(failures, 5)))) break;
            continue;
        }
        failures = 0;
        SysStats stats = sampler.parse(r.stdout_data, now_seconds());
        publish(*sh, [&](Snapshot& s) {
            s.stats = std::move(stats);
            s.state = State::Live;
            s.error.clear();
        });
        // The first sample has no CPU/net deltas yet: take the second one soon.
        int wait = first ? 700 : INTERVAL_MS;
        first = false;
        if (!wait_next(*sh, wait)) break;
    }
    session.disconnect();
    sh->done = true;
}

void SysMonitor::run_local(std::shared_ptr<Shared> sh) {
    SysSampler sampler;
    bool first = true;
    for (;;) {
        std::string folder;
        bool detail;
        {
            std::lock_guard lk(sh->mu);
            if (sh->stop) break;
            folder = sh->folder;
            detail = sh->detail;
        }
        std::string raw = platform::local_system_sample(sysmon_script(folder, first, detail), folder, detail);
        SysStats stats = sampler.parse(raw, now_seconds());
        bool ok = stats.valid;
        publish(*sh, [&](Snapshot& s) {
            s.stats = std::move(stats);
            s.state = ok ? State::Live : State::Lost;
            s.error = ok ? "" : "System statistics are not available";
        });
        int wait = first ? 700 : INTERVAL_MS;
        first = false;
        if (!wait_next(*sh, wait)) break;
    }
    sh->done = true;
}

} // namespace oh
