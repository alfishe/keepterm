// Catch-up of the warm history store from a session folder.
//
// The store lags behind the journal: the record queue, the open blocks, a crash. Whatever it
// lacks, the dead session's journal still has. HistoryCatchUp::run() replays the folder
// (session/history.h recover_session) and stores what lies beyond the store's watermarks:
//   - raw: the output bytes past the raw kind's end seq; a record across it is cut;
//   - text: the lines that left the screen after the checkpoint the replay starts from, minus
//     the ones the store has already (the writer stored everything up to the checkpoint before
//     writing it, so the first K such lines are the K stored after it);
//   - commands: OSC 133 commands the catalog does not have (by seq and command line); a D mark
//     whose A came before the checkpoint ends the open row the writer left.
// Running it twice stores nothing twice.
//
// CommandLog is the one place that turns OSC 133 marks into command rows, for the live writer
// and for the catch-up alike: a row when the command starts, its end when it finishes.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/error.h"
#include "session/history.h"
#include "store/catalog.h"
#include "store/file_io.h"
#include "store/projection.h"
#include "store/session_store.h"

namespace kt::vt {
class Terminal;
}

namespace kt::session {

class CommandLog {
public:
    CommandLog(store::SessionStore& store, store::Catalog& catalog, std::string session)
        : store_(store), catalog_(catalog), session_(std::move(session)) {}

    // Catch-up: loads what the store has after `from_seq` (the replay's checkpoint), so that a
    // replayed command is stored only when it is missing, and remembers the newest row without
    // an end that started before it.
    Result<void> load(uint64_t from_seq);

    void started(std::string_view line, std::string_view cwd, uint64_t seq, uint64_t ts_ms);
    void finished(std::optional<int> exit_code, uint64_t ts_ms);
    uint64_t added() const { return added_; }
    uint64_t skipped() const { return skipped_; }

private:
    store::SessionStore& store_;
    store::Catalog& catalog_;
    std::string session_;
    std::set<std::pair<uint64_t, std::string>> rows_;          // catalog rows by (seq, command)
    std::set<std::pair<uint64_t, std::string>> records_;       // records of the commands kind
    std::vector<std::pair<std::pair<uint64_t, std::string>, int64_t>> open_rows_;  // without an end
    std::optional<int64_t> carried_;  // a row without an end from before from_seq
    std::optional<int64_t> open_;     // the row waiting for its D mark (0: known and ended)
    uint64_t added_ = 0, skipped_ = 0;
};

// Waits up to `limit` for the lock file at `path` (store::LockFile). Errc::busy when another
// process keeps it.
Result<std::unique_ptr<store::LockFile>> wait_lock(const std::filesystem::path& path, std::chrono::milliseconds limit);

// The series writer lock of a history id: history/<id>/writer.lock.
inline constexpr const char* kSeriesLockFile = "writer.lock";

struct CatchUpStats {
    uint64_t raw_bytes = 0;         // output bytes stored
    uint64_t raw_gap = 0;           // bytes between the store's end and the journal's oldest
    uint64_t lines = 0;             // text lines stored
    uint64_t lines_skipped = 0;     // text lines the store had
    bool text_misaligned = false;   // the stored lines did not match the replay
    uint64_t commands = 0;          // command rows added
};

struct CatchUpOptions {
    vt::TerminalOptions term;
    // Store the visible screen as the last text lines (an archive: the session is over).
    bool finish_screen = false;
    std::string cwd;                // for replayed commands
};

// Replays a session folder and stores what the store lacks. The caller holds the series lock.
// The object is the replayed terminal's sink unless run() gets another one; that
// sink forwards OSC 133 command callbacks to command_started() and command_finished() while
// run() runs. The terminal keeps its sink: keep the object while the terminal is used.
class HistoryCatchUp final : public store::LineSink, public vt::TerminalSink, private ReplayObserver {
public:
    HistoryCatchUp(store::SessionStore& store, store::Catalog& catalog, CatchUpOptions o);
    Result<Recovered> run(const std::string& dir, vt::TerminalSink* sink = nullptr);
    const CatchUpStats& stats() const { return stats_; }

    // vt::TerminalSink: the replay's replies go nowhere.
    void reply(std::span<const std::byte>) override {}
    void command_started(std::u8string_view line) override;
    void command_finished(std::optional<int> exit_code) override;
    // store::LineSink: the replayed lines, filtered as TextProjector stores them.
    Result<void> line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) override;

private:
    void journal(const journal::Contents& jc) override;
    void start(vt::Terminal& t, uint64_t from_seq, bool checkpoint) override;
    void record(uint64_t end_seq, uint64_t ts_ms) override;
    std::size_t piece() const override;
    void fed(vt::Terminal& t, uint64_t ts_ms) override;
    void fail(const Error& e);
    void store_events(const journal::Contents& jc);

    store::SessionStore& store_;
    CommandLog commands_;
    CatchUpOptions opt_;
    store::ScrollbackTap tap_;
    CatchUpStats stats_;
    vt::Terminal* term_ = nullptr;
    uint64_t last_ts_ = 0, skip_ = 0, record_end_ = 0;
    std::string last_stored_, buf_;
    std::optional<Error> err_;
};

}  // namespace kt::session
