// The session host's writer of the warm history store.
//
// One low-priority storage thread per host. It is fed from the journal: the journal writer
// hands every record to a RecordQueue (journal::Tap) on the PTY thread, which only copies bytes
// into a preallocated ring and never waits; a full queue drops records and counts them. The
// storage thread:
//   - appends output records to the raw kind (fresh codec and block size, storage.codec*);
//   - every text_interval takes the host's terminal lock for at most rows_per_lock scrollback
//     rows, converts the rows that left the screen into lines (store::ScrollbackTap) and,
//     after releasing the lock, appends them to the text kind;
//   - stores command lines from OSC 133 (the commands kind and a catalog row with the exit
//     code), fed by the host's TerminalSink callbacks;
//   - flushes open blocks every few seconds so other processes can search them;
//   - checks the store's usage against its limits every usage_interval (store/usage.h).
// Private panes and session.history=false never create one.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/error.h"
#include "session/record_queue.h"
#include "store/catalog.h"
#include "store/projection.h"
#include "store/retention.h"
#include "store/session_store.h"
#include "store/usage.h"

namespace kt::vt {
class Terminal;
}

namespace kt::session {

struct HistoryWriterOptions {
    std::filesystem::path root;          // <state>/history
    store::SessionRow session;           // id: RecoveryRecord::history_id
    store::WriterOptions fresh;          // store::fresh_options_from_config()
    store::Policy policy;                // the disk guard and the usage limits
    std::size_t queue_bytes = 16u << 20;
    std::chrono::milliseconds text_interval{100};
    std::size_t rows_per_lock = 256;
    std::chrono::milliseconds flush_interval{5000};
    std::chrono::milliseconds usage_interval{60000};
    std::function<void(const store::UsageEvent&)> on_usage;  // on the storage thread
};

struct HistoryWriterStats {
    uint64_t raw_bytes = 0, lines = 0, commands = 0;
    uint64_t dropped_records = 0, dropped_bytes = 0;  // the queue was full
    uint64_t gaps = 0;                                // jumps in the stream offsets
    uint64_t missed_rows = 0;                         // scrollback rows evicted before the tap saw them
    uint64_t lock_holds = 0;
    double max_lock_us = 0;                           // longest hold of the terminal lock
};

class HistoryWriter {
public:
    // Opens the store and starts the thread. `term` and `mu` are the host's: the tap starts
    // after the rows already in the scrollback (a restored history is in the store already).
    static Result<std::unique_ptr<HistoryWriter>> start(HistoryWriterOptions o, std::mutex& mu, const vt::Terminal& term);
    ~HistoryWriter();
    HistoryWriter(const HistoryWriter&) = delete;
    HistoryWriter& operator=(const HistoryWriter&) = delete;

    // For journal::Writer::set_tap.
    journal::Tap* tap() { return &queue_; }

    // From the host's TerminalSink callbacks (PTY thread, under the terminal lock).
    void command_started(std::string command_line, std::string cwd, uint64_t seq, uint64_t ts_ms);
    void command_finished(std::optional<int> exit_code, uint64_t ts_ms);

    // Stops the thread, stores what is queued and the scrollback rows not seen yet, then:
    // with `ended_ms` the visible screen too, seals every segment and marks the session ended;
    // without, flushes (a restore continues the series). The caller must not hold `mu`.
    void stop(std::optional<uint64_t> ended_ms);

    HistoryWriterStats stats() const;

private:
    struct Command {
        std::string line, cwd;
        uint64_t seq = 0, started = 0;
        std::optional<uint64_t> ended;
        std::optional<int> exit_code;
        bool finished = false;
    };
    class Lines;

    HistoryWriter(HistoryWriterOptions o, std::mutex& mu, const vt::Terminal& term);
    void run();
    bool pump_raw(std::size_t max_bytes);
    bool pump_text(uint64_t now_ms);
    void pump_commands();
    void check_usage(uint64_t now_ms);

    HistoryWriterOptions opt_;
    std::mutex& term_mu_;
    const vt::Terminal& term_;
    RecordQueue queue_;
    std::unique_ptr<store::Catalog> catalog_;
    std::unique_ptr<store::SessionStore> store_;
    std::unique_ptr<Lines> lines_;
    std::unique_ptr<store::TextProjector> projector_;
    std::unique_ptr<store::ScrollbackTap> tap_;
    store::UsageWatcher watcher_;

    std::mutex cmd_mu_;
    std::vector<Command> commands_;   // under cmd_mu_: started, then finished
    std::optional<Command> open_cmd_; // storage thread: the command waiting for its D mark

    uint64_t next_seq_ = UINT64_MAX;  // the raw stream offset expected next
    mutable std::mutex stats_mu_;
    HistoryWriterStats stats_;

    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool stopped_ = false;
    std::thread thread_;
};

}  // namespace kt::session
