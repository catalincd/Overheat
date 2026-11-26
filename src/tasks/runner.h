#pragma once
// Runs a task (and the tasks it depends on, first) in a Terminal, feeding the
// output through the task's problem matchers. UI thread, except the output
// filter, which runs on the terminal's reader thread.
#include "tasks/matchers.h"
#include "tasks/tasks.h"
#include <atomic>
#include <mutex>

namespace oh {

class Terminal;

class TaskRunner {
public:
    enum class State { Idle, Running, Succeeded, Failed, Cancelled };

    // `chain`: dependencies first, the requested task last. Clears the terminal.
    void start(std::vector<TaskDef> chain, const TaskContext& ctx, Terminal& term);
    // Ctrl+C to the task's process group, then a hard kill if it does not stop.
    void cancel();
    // Advances the chain; true when the state or the problem list changed.
    bool poll();

    State state() const { return state_; }
    bool running() const { return state_ == State::Running; }
    const std::string& label() const { return label_; }        // the requested task
    std::string step_label() const;                            // the step running now
    int exit_code() const { return exit_code_; }
    float elapsed() const;
    // Problems found so far, with absolute paths (relative ones resolved against the
    // cwd of the step that printed them).
    std::vector<Problem> problems() const;
    uint64_t problems_version() const { return version_.load(); }

private:
    Terminal* term_ = nullptr;
    std::vector<TaskDef> chain_;
    TaskContext ctx_;
    size_t step_ = 0;
    State state_ = State::Idle;
    std::string label_;
    int exit_code_ = -1;
    TimePoint started_{}, finished_{}, exited_at_{}, cancel_at_{};
    bool waiting_exit_ = false, cancelling_ = false;

    mutable std::mutex mutex_;  // collector_, done_, pending_ (reader thread vs UI)
    ProblemCollector collector_{std::vector<std::string>{}};
    std::vector<Problem> done_;  // earlier steps
    fs::path step_cwd_;
    std::string pending_;  // partial cargo JSON line
    bool json_ = false;
    std::atomic<uint64_t> version_{0};
    size_t seen_ = 0;  // collector size at the last version bump

    void run_step();
    void finish_step(int code);
    std::string filter(const std::string& chunk);
    void absolutize(std::vector<Problem>& ps) const;
};

} // namespace oh
