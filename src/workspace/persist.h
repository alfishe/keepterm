// The save scheduler of the state file. Core code.
//
// Subscribes to the workspace's change bus. A persistent change marks the state dirty; after
// 750 ms of quiet (at most 5 s after the first unsaved change) poll() serializes on the UI
// thread and hands the bytes to the writer. A session id swap skips the debounce. Identical
// content is not written again. Write errors retry after 2 s, 4 s, ... 60 s.
//
// Threads: poll(), flush() and every other call on the UI thread. The writer is a worker thread
// (Mode::Thread) or runs inside poll() (Mode::Inline, tests). With a Poster the scheduler also
// runs a timer thread that posts poll() to the UI loop at the deadline, so the UI needs no timer.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "workspace/state.h"
#include "workspace/workspace.h"

namespace kt::ws {

// Runs a function on the UI thread later (the UI loop's post). Any thread may call it.
using Poster = std::function<void(std::function<void()>)>;

struct PersistOptions {
    std::int64_t idle_ms = 750;
    std::int64_t max_delay_ms = 5000;
    std::int64_t retry_min_ms = 2000, retry_max_ms = 60'000;
    WriterInfo writer;
    enum class Mode : std::uint8_t { Thread, Inline } mode = Mode::Thread;
    Poster post;   // optional: timer thread that posts poll()
    // UI thread, after each successful write (the runtime retires the old per-window files after the first).
    std::function<void()> on_written;
};

enum class PersistState : std::uint8_t { Clean, Dirty, Writing, Failed };

class PersistScheduler {
public:
    PersistScheduler(Workspace& ws, StateStore& store, IClock& clock, PersistOptions opts = {});
    ~PersistScheduler();
    PersistScheduler(const PersistScheduler&) = delete;
    PersistScheduler& operator=(const PersistScheduler&) = delete;

    // Writes when a deadline passed. Cheap when nothing is due.
    void poll();
    // Steady-clock ms of the next poll that may write; nullopt when nothing is pending.
    std::optional<std::int64_t> next_deadline() const;
    // Quit: serialize and write now on this thread; waits for a running write (deadline 1 s).
    WsResult<void> flush();

    // Read-only mode (newer schema, unreadable file): never writes until save_anyway().
    void set_read_only(bool ro, int file_schema = 0);
    bool read_only() const { return read_only_; }
    WsResult<void> save_anyway();
    // Marks the state unsaved without a model change (after an import or a repaired load).
    void request_save(bool immediate = false) { mark_dirty(immediate); }

    PersistState state() const;
    std::uint64_t generation() const { return gen_; }
    std::uint64_t written_generation() const { return written_gen_.load(); }
    int writes() const { return writes_.load(); }
    std::string last_error() const;

private:
    struct WriteResult {
        std::uint64_t gen = 0;
        bool ok = false;
        std::string error;
        std::size_t hash = 0;
    };
    void on_changes(std::span<const Change> changes);
    void mark_dirty(bool immediate);
    WsResult<void> write_now(bool sync);
    void writer_loop();
    void timer_loop();
    void kick_timer();
    void collect();
    void finish_write(const WriteResult& r);

    Workspace& ws_;
    StateStore& store_;
    IClock& clock_;
    PersistOptions opts_;
    Subscription sub_;

    // UI thread.
    bool dirty_ = false;
    bool immediate_ = false;
    std::int64_t first_dirty_ = 0, last_change_ = 0;
    std::int64_t retry_at_ = 0, retry_delay_ = 0;
    bool failed_ = false;
    bool read_only_ = false;
    int ro_schema_ = 0;
    std::uint64_t gen_ = 0;
    std::size_t last_hash_ = 0;
    bool have_hash_ = false;
    std::string last_error_;

    // Shared with the writer and the timer.
    mutable std::mutex mu_;
    std::condition_variable cv_;
    struct Job {
        std::uint64_t gen;
        std::string bytes;
        std::size_t hash;
    };
    std::optional<Job> pending_;
    bool writing_ = false;
    bool stop_ = false;
    std::optional<WriteResult> result_;   // a finished write, for the UI thread
    std::timed_mutex write_mu_;           // one write at a time (worker or flush)
    std::atomic<std::uint64_t> written_gen_{0};
    std::atomic<int> writes_{0};
    std::thread worker_, timer_;
    std::condition_variable timer_cv_;
    std::int64_t timer_deadline_ = -1;   // under mu_
    std::atomic<bool> poll_posted_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace kt::ws
