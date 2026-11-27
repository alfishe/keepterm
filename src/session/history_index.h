// The session history as the user sees it: one entry per series (history_id), from
// the archived manifests under <state>/history/<id>/ and the records of the session folders
// under <state>/sessions/ (running hosts, and dead ones not archived yet).
//
// The manifest is the series' metadata. The archive writes it when a series
// closes; the user's edits (title, tags, pin) write it any time, also while the series runs
// (then it holds only those fields until the archive merges it). Both read, change and write it
// under history/<id>/manifest.lock.
//
// Deleting a series renames its manifest to manifest.deleting first, then drops the catalog
// rows, the files and the folder; a crash in between leaves a folder the janitor finishes.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "config/config.h"
#include "session/history.h"
#include "session/json.h"
#include "session/os.h"
#include "store/retention.h"

namespace kt::session {

inline constexpr const char* kManifestLockFile = "manifest.lock";

enum class HistoryState : uint8_t {
    running,  // a host of the series runs (the window tells live from background)
    closed,   // archived, or its host died and the folder waits for a restore or the sweep
};
std::string_view to_string(HistoryState);

struct HistoryStats {
    uint64_t commands = 0, failed_commands = 0;
    uint64_t raw_bytes = 0, text_lines = 0, stored_bytes = 0;
};

struct HistoryEntry {
    std::string id;                // history_id
    HistoryState state = HistoryState::closed;
    // The session folder of the series (a running host's, or a dead one not archived yet);
    // empty when the series is archived.
    std::string session;
    std::string title, title_user;
    std::string tab_title;         // the tab's own name while it ran
    json::Value project;           // {"id","name","color","path"} or null
    std::string cwd_first, cwd_last, shell;  // "~" for the home folder
    uint64_t created_ms = 0, last_used_ms = 0;
    std::optional<uint64_t> ended_ms;
    std::optional<int> exit_code;
    std::string close_reason;
    std::vector<uint64_t> restores;
    std::vector<std::string> tags;
    bool pinned = false;
    std::vector<AgentRun> agents;
    HistoryStats stats;
    bool final_screen = false;     // history/<id>/final.ktsn exists

    // The user's title, else the tab's name, else the program's (OSC 2).
    const std::string& display_title() const {
        return !title_user.empty() ? title_user : !tab_title.empty() ? tab_title : title;
    }
    std::string project_name() const;
};

// Every series, last used first. Damaged manifests are skipped.
std::vector<HistoryEntry> list_history(const os::Paths&);
// One series by its id or a unique prefix of it; with the command count from the catalog.
Result<HistoryEntry> find_history(const os::Paths&, std::string_view id_or_prefix);
// The commands of a series (OSC 133), oldest first.
struct HistoryCommand {
    uint64_t started_ms = 0;
    std::optional<uint64_t> ended_ms;
    std::optional<int> exit_code;
    std::string cwd, command;
    uint64_t seq = 0;   // the output stream offset it was recorded at
};
std::vector<HistoryCommand> history_commands(const os::Paths&, const std::string& id);

// Lines of stored output with `pattern` (literal, case-insensitive), newest first, with their
// series' title and project. No store: none. `cancel` stops it between blocks.
struct HistoryOutputHit {
    std::string id, title, project, text;
    uint64_t ts_ms = 0;
    std::size_t match_offset = 0, match_len = 0;   // the match in text
    uint64_t seq = 0;                              // with text: the line in the output viewer
};
std::vector<HistoryOutputHit> search_history_output(const os::Paths&, const std::string& pattern, std::size_t limit,
                                                    const std::atomic<bool>* cancel = nullptr);

// The user's edits, kept in the manifest (and the pin in the catalog, for retention).
Result<void> set_history_title(const os::Paths&, const std::string& id, std::string title);
Result<void> tag_history(const os::Paths&, const std::string& id, const std::vector<std::string>& add,
                         const std::vector<std::string>& remove);
Result<void> pin_history(const os::Paths&, const std::string& id, bool pinned);

// Deletes a series: its manifest, final screen, segments and catalog rows, and the session
// folders of the series no host runs in. A running series gives Errc::busy.
Result<uint64_t> delete_history(const os::Paths&, const std::string& id);

// How a closed series comes back: from its session folder when a dead one
// is left (the journal has more than the archive), else from the archive. Errc::busy when the
// series runs.
struct RestorePlan {
    std::optional<SessionId> restore_from;  // the dead folder
    std::string restore_history;            // the archived series
    std::string cwd;                        // the last folder, expanded
    // The session host options: --restore-from or --restore-history, and --resume-run.
    std::vector<std::string> host_args(std::optional<std::size_t> resume_run) const;
};
Result<RestorePlan> plan_restore(const HistoryEntry&);

// Retention: the store policy of a series is the config's with the overrides of
// its project ([[projects]], matched by the name and folder in its manifest or record).
store::Policy history_policy(const config::Config&, const json::Value& project);
// The policy per catalog row (a series), for the janitor and usage(); read once per call, so
// build one per pass. Empty when the config has no project overrides (one policy for all).
store::Janitor::PolicyFor history_policy_for(const config::Config&, const os::Paths&);

// The closed series storage.keep_closed deletes at `now_ms`, as the janitor would: archived,
// ended, not pinned. `keep_closed_ms` stands in for the global value (Settings shows what a new
// value deletes; 0 = for good); project overrides still win.
struct RetentionPreview {
    std::vector<HistoryEntry> due;   // oldest end first
    uint64_t stored_bytes = 0;
};
RetentionPreview retention_preview(const os::Paths&, const config::Config&, uint64_t now_ms,
                                   std::optional<int64_t> keep_closed_ms = std::nullopt);

// `keepterm --history list|show|restore|rename|tag|untag|pin|unpin|delete ...`; argv[first] is
// the command word. True from is_history_index_command for those words.
bool is_history_index_command(std::string_view cmd);
int history_index_cli(int argc, char** argv, int first);

// Reads history/<id>/manifest.json; Errc::not_found without one.
Result<json::Value> read_manifest(const std::string& state, const std::string& id);
// Changes the manifest under its lock: `fn` gets the current one (an empty object when there
// is none) and returns the new one.
Result<void> update_manifest(const std::string& state, const std::string& id,
                             const std::function<json::Value(json::Value)>& fn);
// "~/x" -> "<home>/x".
std::string expand_tilde(const std::string& path, const std::string& home);
// The entry of a manifest (no session folder applied).
HistoryEntry entry_from_manifest(const json::Value& m);

}  // namespace kt::session
