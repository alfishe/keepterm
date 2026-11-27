// The session host's writer of the warm history store.
//
// Two threads per host, fed from the journal: the journal writer hands
// every record to a RecordQueue (journal::Tap) on the PTY thread, which only copies bytes into a
// preallocated ring. Before it takes the terminal lock, the PTY thread calls wait_room(): when the
// storage thread is behind, the PTY waits for it (at most max_stall per chunk) instead of dropping
// history. The queue and the open raw block together stay below 3/4 of the journal ring, so after
// a crash the journal still has what the store had not written. The shadow thread drains the
// queue and hands what is to be stored, in order, to the store thread through a small bounded
// pipe (pipe_bytes; it waits when the store thread is behind), so the terminal work and the
// store work (compression, sealing, the index) run side by side. Together:
//   - output records go to the raw kind (fresh codec and block size, storage.codec*);
//   - the shadow thread feeds the records to its own copy of the terminal (a shadow, restored from the host's
//     terminal when the writer starts), in pieces small enough that no row leaves the shadow's
//     scrollback before store::ScrollbackTap has turned it into a text line: the text kind gets
//     every line however fast the output comes, and the host's terminal lock is not taken for it;
//   - stores command lines from the shadow's OSC 133 marks (the commands kind and a catalog row
//     with the exit code), each at the stream offset of the end of its journal record, like the
//     catch-up after a crash (session/history_catchup.h);
//   - flushes open blocks every few seconds so other processes can search them, and fsyncs
//     them every durable_interval;
//   - checks the store's usage against its limits every usage_interval (store/usage.h).
// Private panes and session.history=false never create one.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/error.h"
#include "session/history_catchup.h"
#include "session/record_queue.h"
#include "store/catalog.h"
#include "store/projection.h"
#include "store/retention.h"
#include "store/session_store.h"
#include "store/usage.h"
#include "vt/terminal.h"

namespace kt::session {

struct HistoryWriterOptions {
    std::filesystem::path root;          // <state>/history
    store::SessionRow session;           // id: RecoveryRecord::history_id
    store::WriterOptions fresh;          // store::fresh_options_from_config()
    store::Policy policy;                // the disk guard and the usage limits
    vt::TerminalOptions term;            // the host terminal's (width rules); the shadow has its own scrollback
    std::size_t queue_bytes = 16u << 20;
    // After this long without output the queue's pages go back to the OS (RecordQueue::release).
    std::chrono::milliseconds queue_idle{1000};
    // The pipe from the shadow thread to the store thread: what is in it is not stored yet, so
    // the host counts it into the journal ring's budget.
    std::size_t pipe_bytes = 1u << 20;
    std::chrono::milliseconds flush_interval{5000};
    std::chrono::milliseconds durable_interval{30000};
    // The longest wait_room() waits for the storage thread; the size of the journal's output
    // records (wait_room counts their headers).
    std::chrono::milliseconds max_stall{2000};
    std::size_t record_chunk = 32u << 10;
    // The longest checkpoint() waits for the storage thread to store the output up to the snapshot.
    std::chrono::milliseconds max_barrier{10000};
    // The host's journal file: a hole left by a stall is filled from it.
    std::string journal_path;
    // The folder of a command whose shell reports none (OSC 7, 9;9); storage thread. Unset: the
    // session's cwd.
    std::function<std::string()> cwd;
    std::chrono::milliseconds usage_interval{60000};
    std::function<void(const store::UsageEvent&)> on_usage;  // on the storage thread
};

struct HistoryWriterStats {
    uint64_t raw_bytes = 0, lines = 0, commands = 0;
    uint64_t dropped_records = 0, dropped_bytes = 0;  // the queue was full
    uint64_t waits = 0, stalls = 0;                   // wait_room() waited; ran out of max_stall
    uint64_t gaps = 0;                                // jumps in the stream offsets
    uint64_t gap_bytes_filled = 0, gap_bytes_lost = 0;  // from the journal file; gone from it too
    uint64_t missed_rows = 0;                         // rows the shadow evicted before the tap saw them
    uint64_t barrier_timeouts = 0;                    // checkpoint() gave up waiting
    uint64_t pipe_peak = 0;                           // the most bytes the pipe held
    uint64_t queue_releases = 0;                      // the queue gave its pages back while idle
    uint64_t queue_resident = 0;                      // bytes of the queue's ring in memory now
};

class HistoryWriter final : private vt::TerminalSink {
public:
    // Opens the store, makes the shadow from `term` (under `mu`) and starts the thread. The tap
    // starts after the rows already in the scrollback (a restored history is in the store already).
    static Result<std::unique_ptr<HistoryWriter>> start(HistoryWriterOptions o, std::mutex& mu, const vt::Terminal& term);
    ~HistoryWriter() override;
    HistoryWriter(const HistoryWriter&) = delete;
    HistoryWriter& operator=(const HistoryWriter&) = delete;

    // For journal::Writer::set_tap.
    journal::Tap* tap() { return &queue_; }
    // PTY thread, before it takes the terminal lock to feed and journal `bytes` of output: waits
    // until the queue has room for them (bounded by max_stall; at once after stop()).
    void wait_room(std::size_t bytes) { (void)queue_.wait_room(bytes, opt_.record_chunk, opt_.max_stall); }

    // Host thread, for a checkpoint: runs `snapshot` under the terminal lock, then
    // waits until the storage thread has stored the text and the commands of the output up to it
    // and written the open blocks (no fsync). True when it got there: a recovery from the
    // snapshot then starts where the store's text ends. False after max_barrier: the caller must
    // not write that checkpoint. After stop() it only runs `snapshot` and returns true. The caller
    // must not hold `mu`.
    bool checkpoint(const std::function<void()>& snapshot);

    // Stops the thread and stores what is queued, then: with `ended_ms` the shadow's visible
    // screen too, seals every segment and marks the session ended; without, flushes (a restore
    // continues the series).
    void stop(std::optional<uint64_t> ended_ms);

    HistoryWriterStats stats() const;
    // After stop(): closes the store's files and the catalog, so another SessionStore of this
    // process may open the series (the archive at close).
    void release();

private:
    // One step for the store thread; its strings are in the batch's bytes.
    struct Op {
        enum class Type : uint8_t { raw, line, command_started, command_finished, flush, close, event };
        Type type = Type::raw;
        bool flag = false;  // flush: durable; command_finished: has an exit code; close: ended
        bool barrier = false;  // flush: a checkpoint waits for it
        int exit_code = 0;
        uint64_t seq = 0, ts = 0;  // flush: the offset stored with it; close: ended_ms
        uint32_t off = 0, len = 0, off2 = 0, len2 = 0;
    };
    struct Batch {
        std::string bytes;
        std::vector<Op> ops;
    };
    // The shadow's tap writes its lines into the pipe.
    struct PipeSink final : store::LineSink {
        HistoryWriter* w = nullptr;
        Result<void> line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) override;
    };

    HistoryWriter(HistoryWriterOptions o, std::mutex& mu);
    void run();
    void run_store();
    // Shadow thread: adds an op to the open batch (its strings copied); ship() hands the batch to
    // the store thread, waiting while the pipe is full.
    Op& emit(Op::Type t, uint64_t seq, uint64_t ts, std::string_view a = {}, std::string_view b = {});
    void ship();
    void execute(const Batch& b);
    bool pump(std::size_t max_bytes);
    // Storage thread: one journal record into raw, the shadow, text and commands.
    void process(journal::Kind kind, uint64_t seq, uint64_t ts, std::span<const std::byte> payload);
    void fill_gap(uint64_t from, uint64_t to);
    // Shadow thread: a resize event at fed_ when the size differs from the last one stored.
    void size_event(vt::GridSize g, uint64_t ts);
    void collect(uint64_t ts);
    void check_usage(uint64_t now_ms);
    void publish_stats();

    // vt::TerminalSink of the shadow (storage thread).
    void reply(std::span<const std::byte>) override {}
    void cwd_changed(const vt::CwdReport& r) override;
    void command_started(std::u8string_view line) override;
    void command_finished(std::optional<int> exit_code) override;

    HistoryWriterOptions opt_;
    std::mutex& term_mu_;
    const vt::Terminal* term_ = nullptr;  // the host's, for checkpoint()
    RecordQueue queue_;
    std::unique_ptr<store::Catalog> catalog_;
    std::unique_ptr<store::SessionStore> store_;
    std::unique_ptr<store::TextProjector> projector_;  // store thread
    PipeSink pipe_sink_;
    std::unique_ptr<store::ScrollbackTap> tap_;
    std::unique_ptr<CommandLog> command_log_;
    std::unique_ptr<vt::Terminal> shadow_;
    store::UsageWatcher watcher_;

    // Storage thread (and stop() after the join).
    uint64_t next_raw_ = UINT64_MAX;  // the raw kind's end: output before it is stored already
    uint64_t fed_ = 0;                // the stream offset the shadow has consumed up to
    vt::GridSize last_size_{0, 0};    // of the last resize event (0x0: none yet)
    uint64_t record_end_ = 0, record_ts_ = 0;  // the record being fed: its commands' seq and time
    bool filling_ = false;
    std::string cwd_;                 // the shell's last OSC 7 / 9;9 report
    uint64_t gaps_ = 0, gap_filled_ = 0, gap_lost_ = 0, raw_bytes_ = 0;
    std::unique_ptr<Batch> open_batch_;
    std::size_t batch_bytes_ = 64u << 10;

    // The pipe, under pipe_mu_.
    mutable std::mutex pipe_mu_;
    std::condition_variable pipe_cv_;   // a batch for the store thread, or its stop
    std::condition_variable room_cv_;   // room in the pipe
    std::deque<std::unique_ptr<Batch>> pipe_;
    std::vector<std::unique_ptr<Batch>> spare_;
    std::size_t in_pipe_ = 0;           // bytes of the shipped batches not executed yet
    std::size_t pipe_peak_ = 0;
    bool store_stop_ = false;
    std::thread store_thread_;

    mutable std::mutex stats_mu_;
    HistoryWriterStats stats_;

    std::mutex mu_;
    std::condition_variable cv_;       // wakes the thread
    std::condition_variable done_cv_;  // stored_ moved
    uint64_t barrier_ = 0;             // under mu_: a checkpoint waits for this offset
    uint64_t stored_ = 0;              // under mu_: everything up to here is stored and written
    bool stop_ = false;
    bool stopped_ = false;
    std::thread thread_;
};

}  // namespace kt::session
