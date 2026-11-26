#include "tasks/runner.h"
#include "ui/terminal.h"
#include <cstdio>

namespace oh {

namespace {
std::string crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}
} // namespace

void TaskRunner::start(std::vector<TaskDef> chain, const TaskContext& ctx, Terminal& term) {
    if (chain.empty()) return;
    if (term_ && running()) term_->kill();
    term_ = &term;
    chain_ = std::move(chain);
    ctx_ = ctx;
    step_ = 0;
    label_ = chain_.back().label;
    exit_code_ = -1;
    started_ = Clock::now();
    cancelling_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_.clear();
        collector_ = ProblemCollector(std::vector<std::string>{});
        seen_ = 0;
    }
    version_++;
    state_ = State::Running;
    term.clear_all();
    run_step();
}

void TaskRunner::run_step() {
    const TaskDef& t = chain_[step_];
    fs::path cwd = task_cwd(t, ctx_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        collector_ = ProblemCollector(t.matchers);
        json_ = collector_.uses("cargo-json");
        pending_.clear();
        step_cwd_ = cwd;
        seen_ = 0;
    }
    std::vector<std::pair<std::string, std::string>> env;
    for (auto& [k, v] : t.env) env.emplace_back(k, expand_task_vars(v, ctx_));
    if (json_) env.emplace_back("CARGO_TERM_COLOR", "always");  // stderr is still a tty; keep its colours
    term_->set_output_filter([this](const std::string& chunk) { return filter(chunk); });
    term_->feed("\x1b[2m> Executing task: " + task_command_line(t, ctx_) + " <\x1b[0m\n\n");
    std::error_code ec;
    if (!fs::is_directory(cwd, ec)) {
        term_->feed("\x1b[31mThe task's folder does not exist: " + cwd.string() + "\x1b[0m\n");
        finish_step(127);
        return;
    }
    if (!term_->spawn(task_argv(t, ctx_), cwd, env, true)) {
        term_->feed("\x1b[31mCould not start " + t.command + "\x1b[0m\n");
        finish_step(127);
        return;
    }
    waiting_exit_ = false;
}

std::string TaskRunner::filter(const std::string& chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string shown;
    if (!json_) {
        collector_.feed(chunk);
        shown = chunk;
    } else {
        // cargo --message-format=json: whole JSON lines become their rendered text. On a
        // tty cargo's progress bar (stderr, ending in \r and an erase) shares the line
        // with the JSON (stdout), so the JSON may start mid-line.
        pending_ += chunk;
        size_t start = 0;
        for (size_t nl; (nl = pending_.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string line = pending_.substr(start, nl - start);
            size_t at = line.find("{\"reason\":");
            if (at != std::string::npos) {
                std::string bare = line.substr(at);
                if (!bare.empty() && bare.back() == '\r') bare.pop_back();
                std::string display;
                if (collector_.feed_cargo_json(bare, display)) {
                    shown += line.substr(0, at) + crlf(display);  // the progress bar's own \r + erase
                    continue;
                }
            }
            collector_.feed(line + "\n");
            shown += line + "\n";
        }
        pending_.erase(0, start);
        // A partial line is shown now (prompts, progress bars), up to where JSON may start.
        size_t brace = pending_.find('{');
        std::string now = brace == std::string::npos ? pending_ : pending_.substr(0, brace);
        if (!now.empty()) {
            collector_.feed(now);
            shown += now;
            pending_.erase(0, now.size());
        }
    }
    if (collector_.problems().size() != seen_) {
        seen_ = collector_.problems().size();
        version_++;
    }
    return shown;
}

bool TaskRunner::poll() {
    if (state_ != State::Running || !term_) return false;
    auto now = Clock::now();
    if (cancelling_ && term_->is_alive() && now > cancel_at_) term_->kill();  // ignored Ctrl+C
    if (term_->is_alive()) return false;
    int code = term_->exit_code();
    if (code < 0) {  // output closed; the exit status follows shortly
        if (!waiting_exit_) {
            waiting_exit_ = true;
            exited_at_ = now;
        }
        if (now - exited_at_ < std::chrono::seconds(2)) return false;
    }
    finish_step(code);
    return true;
}

void TaskRunner::finish_step(int code) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.empty()) collector_.feed(pending_);
        pending_.clear();
        collector_.finish();
        std::vector<Problem> ps = collector_.problems();
        absolutize(ps);
        done_.insert(done_.end(), ps.begin(), ps.end());
        collector_ = ProblemCollector(std::vector<std::string>{});
        seen_ = 0;
    }
    version_++;
    term_->set_output_filter(nullptr);
    exit_code_ = code;
    if (code == 0 && !cancelling_ && step_ + 1 < chain_.size()) {
        step_++;
        term_->feed("\n");
        run_step();
        return;
    }
    finished_ = Clock::now();
    state_ = cancelling_ ? State::Cancelled : code == 0 ? State::Succeeded : State::Failed;
    int errors = 0, warnings = 0;
    for (auto& p : problems()) {
        if (p.severity == 1) errors++;
        else if (p.severity == 2) warnings++;
    }
    char buf[200];
    if (state_ == State::Cancelled)
        std::snprintf(buf, sizeof buf, "\n\x1b[33m * Task cancelled.\x1b[0m\n");
    else
        std::snprintf(buf, sizeof buf, "\n\x1b[%sm * Task %s (exit code %d): %d error%s, %d warning%s, %.1f s.\x1b[0m\n",
                      code == 0 ? "32" : "31", code == 0 ? "succeeded" : "failed", code, errors, errors == 1 ? "" : "s",
                      warnings, warnings == 1 ? "" : "s", elapsed());
    term_->feed(buf);
}

void TaskRunner::cancel() {
    if (state_ != State::Running || !term_) return;
    cancelling_ = true;
    cancel_at_ = Clock::now() + std::chrono::milliseconds(1500);
    if (term_->is_alive()) term_->write("\x03");  // SIGINT to the foreground process group
}

std::string TaskRunner::step_label() const {
    return step_ < chain_.size() ? chain_[step_].label : label_;
}

float TaskRunner::elapsed() const {
    auto end = state_ == State::Running ? Clock::now() : finished_;
    return std::chrono::duration<float>(end - started_).count();
}

void TaskRunner::absolutize(std::vector<Problem>& ps) const {
    auto fix = [&](std::string& f) {
        if (f.empty()) return;
        fs::path p = f;
        if (!p.is_absolute()) f = (step_cwd_ / p).lexically_normal().string();
    };
    for (auto& p : ps) {
        fix(p.file);
        for (auto& n : p.notes) fix(n.file);
        for (auto& x : p.fixes)
            for (auto& e : x.edits) fix(e.file);
    }
}

std::vector<Problem> TaskRunner::problems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Problem> out = done_;
    if (state_ == State::Running) {
        std::vector<Problem> live = collector_.problems();
        absolutize(live);
        out.insert(out.end(), live.begin(), live.end());
    }
    return out;
}

} // namespace oh
