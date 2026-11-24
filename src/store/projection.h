// Text projection: the final text of lines as they leave the
// screen into the scrollback, without escape sequences, appended to the text kind. Indexing
// final text skips redraw noise (progress bars, spinners, TUI repaints).
//
// TextProjector is the API the session host feeds (wiring is a later step). ScrollbackTap
// pulls the rows that entered a Terminal's scrollback since the last call and joins
// soft-wrapped rows into logical lines. backfill_journal() runs a headless kt_vt Terminal
// over a history journal (session/journal.h) and stores its raw output and text projection,
// for tests and `keepterm --history backfill`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "base/error.h"
#include "store/session_store.h"
#include "vt/cell.h"
#include "vt/tables.h"

namespace kt::vt {
class Terminal;
}

namespace kt::store {

// UTF-8 text of a row: wide tails and wrap padding skipped, clusters expanded, blank cells as
// spaces. `trim` drops trailing spaces.
std::string cells_to_utf8(std::span<const vt::Cell> cells, const vt::GraphemePool& pool, bool trim = true);
// The same, appended to `out` (trimming only what it appended).
void append_cells_utf8(std::string& out, std::span<const vt::Cell> cells, const vt::GraphemePool& pool, bool trim = true);

class LineSink {
public:
    virtual ~LineSink() = default;
    // One final logical line, without the newline. `seq` is the output stream offset when it
    // left the screen, `ts_ms` the wall clock time.
    virtual Result<void> line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) = 0;
};

class TextProjector final : public LineSink {
public:
    explicit TextProjector(SessionStore& store) : store_(store) {}
    // Applied to every line before it is stored and indexed (phase 2 redaction lists).
    void set_redactor(std::function<void(std::string&)> fn) { redact_ = std::move(fn); }
    // Blank lines are skipped; C0 controls are replaced by spaces.
    Result<void> line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) override;
    uint64_t lines() const { return lines_; }

private:
    SessionStore& store_;
    std::function<void(std::string&)> redact_;
    std::string buf_;
    uint64_t lines_ = 0;
};

class ScrollbackTap {
public:
    explicit ScrollbackTap(LineSink& sink) : sink_(sink) {}
    // Starts after the rows already in `t`'s scrollback (default: from absolute row 0).
    void attach(const vt::Terminal& t);
    // Emits the logical lines completed by rows that entered the scrollback since the last call,
    // at most `max_rows` rows per call (the session host bounds the time it holds the terminal
    // lock); more() tells whether rows are left for the next call.
    Result<void> collect(const vt::Terminal& t, uint64_t seq, uint64_t ts_ms, std::size_t max_rows = SIZE_MAX);
    bool more() const { return more_; }
    // End of the session: the pending wrapped line and the rows of the visible screen.
    Result<void> finish(const vt::Terminal& t, uint64_t seq, uint64_t ts_ms);
    // Rows that were evicted from the scrollback before collect() saw them.
    uint64_t missed_rows() const { return missed_; }

private:
    Result<void> row(std::span<const vt::Cell> cells, uint16_t flags, const vt::GraphemePool& pool, uint64_t seq,
                     uint64_t ts);

    LineSink& sink_;
    uint64_t next_ = 0;
    uint64_t missed_ = 0;
    bool more_ = false;
    std::string pending_;
};

struct BackfillOptions {
    bool raw = true;   // store the output bytes in the raw kind
    bool text = true;  // store the text projection
};

struct BackfillStats {
    uint64_t records = 0, output_bytes = 0, lines = 0, missed_rows = 0;
    bool gap = false;  // the journal ring had lost older records
};

Result<BackfillStats> backfill_journal(std::span<const std::byte> journal_file, SessionStore& store,
                                       const BackfillOptions& = {});

}  // namespace kt::store
