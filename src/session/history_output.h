// The stored output of a series for the output viewer: its text lines, in the
// order they left the screen, and its commands with the line each one starts at.
//
// The viewer shows the text kind, not the raw one: the raw stream has no record of the sizes the
// terminal had, so replaying it at another width garbles what full-screen programs drew, while
// the text lines are the final text of every line, the same lines the output search finds.
//
// A long series is shown as a window of at most `max_lines` lines and `max_bytes` bytes: the
// newest, or the lines around a line to open at; the lines left out are counted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/os.h"

namespace kt::session {

struct OutputLine {
    std::string text;   // UTF-8, without controls (store/projection.h)
    uint64_t seq = 0;   // the output stream offset when it left the screen; not unique
    uint64_t ts_ms = 0;
};

struct OutputCommand {
    uint64_t started_ms = 0;
    std::optional<uint64_t> ended_ms;
    std::optional<int> exit_code;
    std::string command, cwd;
    uint64_t seq = 0;
    std::optional<std::size_t> line;   // the line of the window that shows it, if the window has it
};

// Where to open: a line found by the output search (its seq and text), or a command (its seq and
// command line).
struct OutputTarget {
    uint64_t seq = 0;
    std::string text;
};

struct OutputWindowOptions {
    std::size_t max_lines = 100'000;
    std::size_t max_bytes = 8u << 20;   // the viewer's terminal keeps about 8 bytes a character
    std::optional<OutputTarget> around;   // none: the newest lines
};

struct HistoryOutput {
    std::vector<OutputLine> lines;
    std::vector<OutputCommand> commands;   // oldest first
    uint64_t lines_before = 0, lines_after = 0;   // stored lines outside the window
    std::optional<std::size_t> target;            // the line of `around`, when found
};

// The line of `lines` a target names: among the lines with its seq, the first whose text starts
// with the target's (a search hit's text is cut); without one, the first line at or after the
// seq that contains the text, at most `scan` lines on (a command's line leaves the screen after
// the command was recorded); else none.
std::optional<std::size_t> find_output_line(const std::vector<OutputLine>& lines, const OutputTarget& t,
                                            std::size_t scan = 2000);

// Errc::unavailable: no such series, or nothing of it is stored (the store was off, a private pane).
Result<HistoryOutput> load_history_output(const os::Paths&, const std::string& id, const OutputWindowOptions& = {});

}  // namespace kt::session
