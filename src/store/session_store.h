// The writer side of one session: a series of segments per kind under
// `<root>/<session-id>/`, with the catalog rows kept up to date.
//
// Used by one storage thread of the session host (never the PTY thread); all
// calls are serialized by an internal mutex, so a search thread may call candidates().
// A host restarted with --restore-from opens the old id and continues its series: the newest
// unsealed segment of each kind is resumed (torn tail truncated), or a new one is started.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/error.h"
#include "store/catalog.h"
#include "store/segment.h"

namespace kt::store {

struct DiskGuard {
    uint64_t min_free_bytes = 1ull << 30;  // storage.min_free_disk
    double min_free_percent = 2.0;         // storage.min_free_disk_percent
};
// True when the volume of `root` has less free space than the guard allows: below the
// smaller of the two limits.
bool disk_below_min(const std::filesystem::path& root, const DiskGuard& g);

struct SessionStoreOptions {
    WriterOptions writer;
    std::optional<DiskGuard> disk;          // nullopt: no free-space check
    uint64_t disk_check_bytes = 8ull << 20; // check again after this many appended bytes
    uint64_t disk_retry_ms = 60'000;        // while stopped, check again after this long
    // Called (on the writer's thread) when warm writes stop or resume because of the disk guard.
    std::function<void(bool stopped)> on_writes_stopped;
};

struct SessionStoreStats {
    uint64_t appended_bytes = 0;
    uint64_t dropped_bytes = 0;   // not written: warm writes were stopped (disk low)
    uint32_t segments_sealed = 0;
    uint64_t truncated_bytes = 0; // torn tails dropped when resuming
};

class SessionStore {
public:
    static Result<std::unique_ptr<SessionStore>> open(const std::filesystem::path& root, Catalog& catalog,
                                                      const SessionRow& session, const SessionStoreOptions& = {});
    ~SessionStore();

    // Appends one record of `kind`. While warm writes are stopped the record is dropped and
    // counted (the hot journal still has it).
    Result<void> append(Kind kind, uint64_t seq, uint64_t ts_ms, std::string_view payload);
    // Writes open blocks and updates the catalog rows; `durable` also fsyncs.
    Result<void> flush(bool durable);
    // Seals every open segment. With `ended_ms` the session is marked ended in the catalog.
    Result<void> close(std::optional<uint64_t> ended_ms);

    bool writes_stopped() const { return stopped_.load(); }
    const std::string& id() const { return session_.id; }
    SessionStoreStats stats() const;
    // The catalog row id of the open segment of `kind` and its count of written blocks
    // ({0, 0} if none): a jump target for command records.
    std::pair<int64_t, uint32_t> position(Kind kind) const;
    // In-memory candidates of the open segment of `kind` and its catalog row id (0 if none).
    std::pair<int64_t, trigram::Bits> open_candidates(Kind kind, const trigram::Query& q) const;

private:
    SessionStore() = default;
    struct Open {
        std::unique_ptr<SegmentWriter> w;
        int64_t row = 0;
    };
    Result<void> resume(Kind kind);
    Result<Open*> writer(Kind kind, uint64_t now);
    Result<void> seal(Open& o);
    void check_disk(uint64_t now);

    mutable std::mutex mu_;
    std::filesystem::path root_, dir_;
    Catalog* catalog_ = nullptr;
    SessionRow session_;
    SessionStoreOptions opt_;
    std::array<Open, kKindCount> open_{};
    std::atomic<bool> stopped_{false};
    uint64_t since_check_ = 0;
    uint64_t next_retry_ms_ = 0;
    SessionStoreStats stats_;
};

}  // namespace kt::store
