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
#include "session/journal.h"
#include "session/json.h"
#include "vt/terminal.h"

namespace kt::session {

// One agent process the pane ran, from the host's agent capture.
struct AgentRun {
    std::string agent;          // descriptor id
    std::string conversation;   // the agent's own session id
    std::string cwd;
    std::vector<std::string> args;
    uint64_t started_ms = 0;
    std::optional<uint64_t> ended_ms;
    bool operator==(const AgentRun&) const = default;
};
json::Value to_json(const AgentRun&);
std::optional<AgentRun> agent_run_from_json(const json::Value&);

struct RecoveryRecord {
    uint16_t schema = 1;
    SessionId id;
    uint64_t created_unix_ms = 0;
    uint64_t updated_unix_ms = 0;
    std::string cwd;
    std::string cwd_source;   // session/cwd.h to_string(CwdSource)
    std::string title;        // empty with history off
    std::string tab_title;    // the tab's own name in the GUI (SET_OPTION history.tab_title)
    std::string shell;        // argv[0]
    std::string backend;
    uint16_t cols = 80, rows = 24;
    bool history = true;
    bool store = false;       // the host wrote the warm history store: close archives it there
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
    // The history of the series, carried over by restores: every agent run, the restore times,
    // and the project by value ({"id","name","color","path"}; null without one; the GUI sends it
    // with SET_OPTION history.project).
    std::vector<AgentRun> agent_runs;
    std::vector<uint64_t> restores;
    json::Value project;
};

json::Value to_json(const RecoveryRecord&);
Result<RecoveryRecord> recovery_record_from_json(const json::Value&);

inline constexpr const char* kRecordFile = "record.json";
inline constexpr const char* kJournalFile = "journal.ktj";

std::string checkpoint_name(uint64_t seq);
// Checkpoint files in `dir`, newest first.
std::vector<std::pair<uint64_t, std::string>> list_checkpoints(const std::string& dir);

Result<RecoveryRecord> read_recovery_record(const std::string& dir);

// The host of a session holds this lock file in its folder while it runs (the OS drops it when
// the process dies): a folder whose lock is free belongs to no running host. A restoring host
// and an archiver hold the lock of the folder they read.
inline constexpr const char* kHostLockFile = "host.lock";
// True when `dir` has a journal, a record or a checkpoint: something to restore.
bool has_session_files(const std::string& dir);
// The folder to restore `id` from when its own folder has nothing: the newest
// session folder under `sessions_dir` whose record names `id` as restored_from, followed the
// same way while such a folder has nothing either. nullopt when there is none.
std::optional<std::string> find_successor(const std::string& sessions_dir, const SessionId& id);
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

// Observer of a recovery replay: the history store catch-up (session/history_catchup.h).
class ReplayObserver {
public:
    virtual ~ReplayObserver() = default;
    // The journal as read, before the replay (its payloads live until recover_session returns).
    virtual void journal(const journal::Contents&) {}
    // The terminal the replay feeds and the seq it starts from: the checkpoint's, or the oldest
    // record's when there is no usable checkpoint (`checkpoint` false).
    virtual void start(vt::Terminal&, uint64_t /*from_seq*/, bool /*checkpoint*/) {}
    // Before an output record is fed: the stream offset of its end and its time.
    virtual void record(uint64_t /*end_seq*/, uint64_t /*ts_ms*/) {}
    // The most bytes fed between two fed() calls (an output record is fed in pieces of this size).
    virtual std::size_t piece() const { return SIZE_MAX; }
    // After each replayed piece of output or resize record, with the record's time.
    virtual void fed(vt::Terminal&, uint64_t /*ts_ms*/) {}
};

// Recovers the history of a dead session from its folder: the newest valid checkpoint that the
// journal chain reaches, then the journal records from its seq; otherwise the ring alone.
// Damaged files are skipped (and renamed *.corrupt-<time>). Ends with reset_program_modes(), so
// the result shows the primary screen. An empty or missing folder gives a blank terminal.
Result<Recovered> recover_session(const std::string& dir, const vt::TerminalOptions&, vt::TerminalSink*,
                                  ReplayObserver* observer = nullptr);

// The separator line between an old history and the new session, dim, with <time> in local time.
std::string restored_separator(uint64_t unix_ms);

}  // namespace kt::session
