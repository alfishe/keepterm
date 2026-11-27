// Export of a series' stored output: `keepterm --history output`, Export… in
// Session History and in the output viewer.
//   - text: the text kind, one line each, as the output search and the viewer show it;
//   - raw: the raw kind as the programs wrote it, escape sequences and all (`cat` it in a
//     terminal of the same size, or `less -R`);
//   - asciicast: asciicast v2 (asciinema play, the asciinema web player), the raw kind at its
//     times, with the sizes of the events kind as the header's size and "r" events. A series
//     stored before the sizes were has none: its header says 80x24 and `sized` is false.
// The output streams through `sink` in pieces; nothing is held whole.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/error.h"
#include "session/history_events.h"
#include "session/os.h"

namespace kt::session {

enum class ExportFormat : uint8_t { text, raw, asciicast };
// "text", "raw", "asciicast" (also "cast").
std::optional<ExportFormat> export_format_from_string(std::string_view);
std::string_view to_string(ExportFormat);
// ".txt", ".log", ".cast".
std::string_view export_extension(ExportFormat);

struct ExportInfo {
    std::string title;          // asciicast: the header's title (empty: none)
    uint64_t created_ms = 0;    // asciicast: the header's timestamp
};

struct ExportStats {
    uint64_t bytes = 0;     // written
    uint64_t records = 0;   // lines (text) or output records (raw, asciicast)
    uint64_t resizes = 0;   // asciicast: "r" events
    bool sized = false;     // asciicast: the header's size came from the store
    TermSize size{80, 24};  // asciicast: the header's size
};

// `sink` gets the export in pieces; false from it stops the export with an error.
using ExportSink = std::function<bool(std::string_view)>;
Result<ExportStats> export_history(const os::Paths& p, const std::string& id, ExportFormat f, const ExportInfo& info,
                                   const ExportSink& sink);
// The same into a file (UTF-8 path), written beside it first and renamed over it at the end.
Result<ExportStats> export_history_to_file(const os::Paths& p, const std::string& id, ExportFormat f,
                                           const ExportInfo& info, const std::string& path);

// The file name an export suggests: the session's name with what no file system takes replaced
// by "-" (at most 80 bytes, whole characters), its local start time and the format's extension:
// "make check-2026-11-27-1405.cast". Empty names give "session".
std::string export_file_name(std::string_view name, uint64_t created_ms, ExportFormat f);

// A JSON string literal of `bytes` with every invalid UTF-8 byte as U+FFFD. `carry` holds an
// incomplete sequence at the end of the bytes for the next call (raw records cut characters);
// pass nullptr to replace it too. Public for the tests.
void append_json_string(std::string& out, std::string_view bytes, std::string* carry);

}  // namespace kt::session
