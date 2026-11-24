// The workspace state file: serializer, loader, validation, migration,
// and the import of the old per-window pane files (session/gui_state.h).
// Core code.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/error.h"
#include "workspace/fs.h"
#include "workspace/model.h"

namespace kt::ws {

inline constexpr int kSchemaVersion = 1;
inline constexpr std::string_view kStateFileName = "workspace.json";
inline constexpr std::size_t kMaxCorruptFiles = 5;

// "2026-11-20T09:15:02Z" (UTC, seconds) and back; and "20261120-091502" for file names.
std::string iso8601(std::int64_t unix_ms);
std::optional<std::int64_t> parse_iso8601(std::string_view text);
std::string file_stamp(std::int64_t unix_ms);

struct WriterInfo {
    std::string app = "keepterm";
    std::string version;
    std::string build;
};

// What load validation fixed. Logged by the caller.
struct RepairLog {
    std::vector<std::string> notes;   // English, no terminal text
    bool any() const { return !notes.empty(); }
};

// The state file text (pretty, sorted keys, stable array order). `saved_unix_ms` < 0 omits
// "saved_at", which gives the content form used for the skip-identical compare.
std::string serialize_state(const StateSnapshot&, const WriterInfo&, std::int64_t saved_unix_ms);

// A migration step: rewrites a version-N document into version N+1.
struct Migration {
    int from = 0;
    std::function<bool(json::Value&)> apply;
};
// The steps of this build, in order (empty while only schema 1 exists).
std::span<const Migration> migrations();
// Applies the steps from `from` up to `to`. false when a step is missing or fails.
bool migrate(json::Value& doc, int from, int to, std::span<const Migration> steps);

struct ParseOutcome {
    StateSnapshot state;
    int file_schema = 0;
    bool newer_schema = false;   // loaded what this build understands; the caller goes read-only
    RepairLog repairs;
};

// Parse + migrate + validate + repair. Structural errors give StateCorrupt.
WsResult<ParseOutcome> parse_state(std::string_view text, IRandom& rng);

// Repairs in place (also used after building a state by hand): duplicate entity ids, duplicate
// session ids (first keeps it), dangling active_tab / focused / zoomed, windows on unknown
// projects, a missing scratch workspace, unsorted sidebar lists.
void repair_state(StateSnapshot& s, IRandom& rng, RepairLog& log);

// The default first-launch state: scratch with one tab (one pane in `home`), one window.
StateSnapshot default_state(IRandom& rng, std::string_view home);

// One pane of the old per-window files (<state>/windows/<session-id>.json).
struct LegacyPane {
    session::SessionId session;
    std::string cwd;
    std::string cwd_source;
    std::string title;
    std::uint64_t updated_unix_ms = 0;
};
// One scratch tab and one window per old pane, oldest first, the newest window key.
StateSnapshot state_from_legacy(std::span<const LegacyPane> panes, IRandom& rng, std::string_view home);

// ------------------------------------------------------------------------------ the store

enum class LoadStatus : std::uint8_t {
    Loaded,              // workspace.json loaded (maybe repaired)
    Empty,               // no file (or an empty one): default state
    ImportedLegacy,      // no file; the old per-window files were imported
    RestoredFromBackup,  // the file was corrupt; workspace.json.bak loaded
    ResetAfterCorrupt,   // the file was corrupt and no backup loaded: default state
    NewerSchema,         // a newer build wrote it: loaded, read-only until "Save anyway"
    ReadOnlyIo,          // unreadable after 3 tries: default state, never saved this run
};
std::string_view to_string(LoadStatus);   // "loaded", "restored_from_backup", ... (CLI status)

struct LoadOutcome {
    StateSnapshot state;
    LoadStatus status = LoadStatus::Empty;
    bool read_only = false;
    int file_schema = 0;
    std::string corrupt_file;   // the renamed corrupt file, when any
    std::string notice;         // English text for the GUI notice / banner; empty = none
    RepairLog repairs;
};

// The state folder's files: load procedure, backup, corrupt handling, atomic writes.
class StateStore {
public:
    StateStore(IFileSystem& fs, IClock& clock, IRandom& rng, std::string state_dir);

    // The load procedure. `legacy` is asked for old panes when there is no state file.
    // `sleep_ms` waits between read retries (tests pass a no-op).
    LoadOutcome load(std::string_view home, const std::function<std::vector<LegacyPane>()>& legacy = {},
                     const std::function<void(int)>& sleep_ms = {});

    WsResult<void> write(std::string_view bytes);
    // "Save anyway" after NewerSchema: copies the file to workspace.json.v<N>-backup.
    WsResult<void> backup_newer(int file_schema);
    // Deletes workspace.json.tmp-* older than one minute (crash during a write).
    void clean_temp_files();

    const std::string& dir() const { return dir_; }
    std::string file() const { return path_join(dir_, kStateFileName); }
    std::string backup_file() const { return file() + ".bak"; }

private:
    std::string quarantine(const std::string& path);   // rename aside, prune to 5, returns new name
    std::string timestamp() const;

    IFileSystem& fs_;
    IClock& clock_;
    IRandom& rng_;
    std::string dir_;
};

}  // namespace kt::ws
