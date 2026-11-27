// Archive of a closed session.
//
// When a pane closes, its session folder (journal, checkpoints, record.json) is not deleted:
// archive_session() moves everything into the history store first:
//   1. takes the folder's host lock and the series writer lock (busy: another process has it);
//   2. catches the store up from the journal (session/history_catchup.h), the visible screen
//      included, seals the series and marks the session ended;
//   3. writes history/<history_id>/final.ktsn (the final screen and scrollback) and
//      manifest.json, each with temp + rename;
//   4. removes the folder.
// A crash at any step leaves the folder; the next archive repeats every step and stores
// nothing twice. Folders of private panes and of sessions without the store are removed.
#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "base/error.h"
#include "session/history.h"
#include "session/json.h"
#include "session/os.h"
#include "vt/terminal.h"

namespace kt::session {

inline constexpr const char* kManifestFile = "manifest.json";
inline constexpr const char* kFinalSnapshotFile = "final.ktsn";

enum class ArchiveOutcome : uint8_t {
    archived,  // in the store; the folder is gone
    removed,   // nothing to keep (no store, private, empty): the folder is gone
    busy,      // a host or another archiver holds the folder or the series; nothing changed
};
std::string_view to_string(ArchiveOutcome);

struct ArchiveOptions {
    std::string state;               // os::Paths::state: history/ is under it
    vt::TerminalOptions term;        // scrollback limits of the replay
    std::string close_reason = "closed";  // closed, exited, killed, orphaned
    // The caller is the session's own host: it holds the folder's host lock and the series lock.
    bool locks_held = false;
    // How long a busy lock is retried. A host known to be dead (killed, `session kill`) may still
    // hold its locks for a moment while the OS tears it down.
    std::chrono::milliseconds busy_wait{0};
    // Called before the folder is removed (the host closes its journal and drops its lock).
    std::function<void()> before_remove;
};

struct ArchiveResult {
    ArchiveOutcome outcome = ArchiveOutcome::removed;
    std::string history_id;
    uint64_t raw_bytes = 0, lines = 0, commands = 0;  // caught up from the journal
    bool text_misaligned = false;
    uint64_t raw_gap = 0;
};

Result<ArchiveResult> archive_session(const std::string& dir, const ArchiveOptions& o);

// The manifest of a series from the newest record and the store's figures,
// merged into `previous` (an earlier archive of the series): the first cwd and the creation time
// stay, restores and agent runs are joined.
struct ManifestFacts {
    RecoveryRecord record;
    std::string history_id;
    uint64_t last_used_ms = 0, ended_ms = 0;
    std::string close_reason;
    uint64_t commands = 0, failed_commands = 0;
    uint64_t raw_bytes = 0, text_lines = 0, stored_bytes = 0;
};
json::Value build_manifest(const ManifestFacts& f, const json::Value* previous);
// "~" for paths under the home folder.
std::string tilde_path(const std::string& path, const std::string& home);

}  // namespace kt::session
