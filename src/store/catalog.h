// The catalog of the store: one SQLite database,
// `history/catalog.db`, in WAL mode, with metadata only. Every process (session hosts, the
// GUI, the CLI) opens its own connection; writes are short IMMEDIATE transactions and wait
// up to 10 s for another writer. Thread-safe (one mutex per connection).
//
// Schema versions are kept in PRAGMA user_version. A newer database than this build knows
// gives Errc::not_supported; older ones are migrated forward on open.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "store/segment.h"

struct sqlite3;

namespace kt::store {

inline constexpr int kCatalogSchema = 2;
inline constexpr const char* kCatalogFile = "catalog.db";

struct SessionRow {
    std::string id;
    std::string project, host, agent, shell, cwd;
    uint64_t started_ms = 0;
    std::optional<uint64_t> ended_ms;  // nullopt: live
    bool pinned = false;
};

struct SegmentRow {
    int64_t id = 0;
    std::string session;
    std::string file;  // relative to the store root: "<session>/<name>.kts"
    uint64_t created_ms = 0;
    SegmentInfo info;
};

struct SegmentFilter {
    std::optional<uint64_t> since_ms, until_ms;  // overlap with [ts_first, ts_last]
    std::vector<std::string> sessions;           // empty: all
    std::optional<std::string> project;
    std::vector<Kind> kinds;                     // empty: all
    bool sealed_only = false;
};

struct PinRow {
    int64_t id = 0;
    std::string session;  // empty: every session
    uint64_t from_ms = 0, to_ms = 0;
    std::string note;
};

struct CommandRow {
    int64_t id = 0;
    std::string session;
    uint64_t started_ms = 0;
    std::optional<uint64_t> ended_ms;
    std::optional<int> exit_code;
    std::string cwd, command;
    int64_t segment_id = 0;
    uint32_t block = 0;
    uint64_t seq = 0;
};

class Catalog {
public:
    static Result<std::unique_ptr<Catalog>> open(const std::filesystem::path& db_file);
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    int schema_version() const { return version_; }

    // Sessions. upsert keeps started_ms and pinned of an existing row and clears ended_ms (a
    // restored series runs again).
    Result<void> upsert_session(const SessionRow&);
    Result<void> end_session(const std::string& id, uint64_t ended_ms);
    Result<void> set_pinned(const std::string& id, bool pinned);
    Result<std::vector<SessionRow>> sessions();
    Result<std::optional<SessionRow>> session(const std::string& id);

    // Segments. begin_segment allocates the next series number of (session, kind), inserts an
    // unsealed row and returns it; the caller creates the file afterwards (row first, so a
    // file never exists without a row except as an orphan of a crash).
    Result<SegmentRow> begin_segment(const std::string& session, Kind kind, uint64_t created_ms);
    Result<void> update_segment(int64_t id, const SegmentInfo&);
    Result<std::optional<SegmentRow>> open_segment(const std::string& session, Kind kind);
    // Ordered newest first (ts_last descending, then series descending).
    Result<std::vector<SegmentRow>> segments(const SegmentFilter& = {});
    Result<void> delete_segments(const std::vector<int64_t>& ids);
    // Compaction switch: inserts `replacement` and deletes `old` in one transaction.
    Result<int64_t> replace_segments(const std::vector<int64_t>& old, const SegmentRow& replacement);
    // Removes the session's rows (segments, commands, pins, session) and returns its segment files.
    Result<std::vector<std::string>> forget_session(const std::string& id);

    Result<int64_t> add_pin(const PinRow&);
    Result<void> remove_pin(int64_t id);
    Result<std::vector<PinRow>> pins();

    Result<int64_t> add_command(const CommandRow&);
    // Sets the end of a command row added when it started.
    Result<void> finish_command(int64_t id, uint64_t ended_ms, std::optional<int> exit_code);
    Result<std::vector<CommandRow>> commands(const std::string& session);

private:
    Catalog() = default;
    Result<void> exec(const char* sql);
    Result<void> migrate();
    template <class Fn>
    auto transaction(Fn&& fn) -> decltype(fn());

    std::mutex mu_;
    sqlite3* db_ = nullptr;
    int version_ = 0;
};

}  // namespace kt::store
