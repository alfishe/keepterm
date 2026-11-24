// The session's state folder: the recovery record, checkpoints and recovery.
//
//   <state>/sessions/<id>/record.json          RecoveryRecord, temp + rename, debounced
//   <state>/sessions/<id>/journal.ktj          journal ring (session/journal.h)
//   <state>/sessions/<id>/checkpoint-<seq>.ktsn  KTSN checkpoints, newest two kept
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/json.h"
#include "vt/terminal.h"

namespace kt::session {

struct RecoveryRecord {
    uint16_t schema = 1;
    SessionId id;
    uint64_t created_unix_ms = 0;
    uint64_t updated_unix_ms = 0;
    std::string cwd;
    std::string cwd_source;   // session/cwd.h to_string(CwdSource)
    std::string title;        // empty with history off
    std::string shell;        // argv[0]
    std::string backend;
    uint16_t cols = 80, rows = 24;
    bool history = true;
    std::optional<SessionId> restored_from;
    // The id the warm history store files this session under: the first session of a
    // restore chain, so restored sessions continue one series. Empty: the session's own id.
    std::string history_id;
    std::optional<int32_t> exit_code;   // set at Ended
    // The agent the pane ran, resumed after a respawn. Kept as given.
    json::Value agent;
    // False: the user turned off the agent resume of this pane (SET_OPTION agent.restore);
    // carried over to the respawned session.
    bool agent_restore = true;
    // Theme colors as "#rrggbb" (SET_OPTION theme.*).
    std::string theme_fg, theme_bg, theme_cursor;
};

json::Value to_json(const RecoveryRecord&);
Result<RecoveryRecord> recovery_record_from_json(const json::Value&);

inline constexpr const char* kRecordFile = "record.json";
inline constexpr const char* kJournalFile = "journal.ktj";

std::string checkpoint_name(uint64_t seq);
// Checkpoint files in `dir`, newest first.
std::vector<std::pair<uint64_t, std::string>> list_checkpoints(const std::string& dir);

Result<RecoveryRecord> read_recovery_record(const std::string& dir);
Result<void> write_recovery_record(const std::string& dir, const RecoveryRecord&);

struct Recovered {
    std::unique_ptr<vt::Terminal> term;
    std::optional<RecoveryRecord> record;
    uint64_t last_unix_ms = 0;      // newest of the journal's last write and record.updated
    std::optional<uint64_t> checkpoint_seq;
    std::size_t replayed_records = 0;
    uint64_t replayed_bytes = 0;
    bool ring_only = false;         // the ring did not reach back to a checkpoint
    bool gap = false;
    std::string note;               // what happened, for the log
};

// Recovers the history of a dead session from its folder: the newest valid checkpoint that the
// journal chain reaches, then the journal records from its seq; otherwise the ring alone.
// Damaged files are skipped (and renamed *.corrupt-<time>). Ends with reset_program_modes(), so
// the result shows the primary screen. An empty or missing folder gives a blank terminal.
Result<Recovered> recover_session(const std::string& dir, const vt::TerminalOptions&, vt::TerminalSink*);

// The separator line between an old history and the new session, dim, with <time> in local time.
std::string restored_separator(uint64_t unix_ms);

}  // namespace kt::session
