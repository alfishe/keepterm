// Store usage against its limits.
//
// usage() reads the catalog and the volume once and reports, for every limit, what is used,
// the limit and what remains:
//   - max_total: the file bytes of every segment;
//   - max_per_session: per session (live and closed), sorted by size;
//   - min_free_disk: the free space of the volume against the threshold at which warm writes
//     stop. Its percent is the fill of the space writes may use: 100 % means writes stop.
// Plus the bytes per kind (raw, text, commands, events), the index, the catalog files and the
// pinned data, and what the janitor removes next: the nearest age expiry and, when a cap is
// exceeded, the segments a pass would evict first.
//
// UsageWatcher turns successive usage() results into threshold events (80 % and 95 % of any
// limit, writes stopped and resumed) for the GUI's notifications. It keeps no thread; the owner
// feeds it.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "store/catalog.h"
#include "store/retention.h"

namespace kt::store {

struct LimitUsage {
    uint64_t used = 0, limit = 0, remaining = 0;  // remaining = limit - used, 0 when over
    double percent = 0;                            // used / limit * 100 (may exceed 100)
};

struct SessionUsage {
    std::string id, project;
    bool live = false, pinned = false;
    LimitUsage cap;                                // against max_per_session (per-project policy)
    std::array<uint64_t, kKindCount> kind_bytes{};  // file bytes per Kind without the index
    uint64_t index_bytes = 0;
    uint32_t segments = 0;
};

struct Eviction {
    std::string session;
    int64_t segment_id = 0;   // 0: the whole closed session (keep_closed)
    Kind kind = Kind::raw;
    uint64_t bytes = 0;
    uint64_t at_ms = 0;       // when it becomes due; <= now: the next pass removes it
    std::string reason;       // "max_age", "raw_max_age", "events_max_age", "keep_closed", "max_total", "max_per_session"
};

struct DiskUsage {
    uint64_t capacity = 0, available = 0;
    uint64_t threshold = 0;   // writes stop below this much free space
    LimitUsage fill;          // used = capacity - available, limit = capacity - threshold
    bool below = false;       // writes stopped
};

struct Usage {
    uint64_t now_ms = 0;
    LimitUsage total;                               // against max_total
    DiskUsage disk;
    std::vector<SessionUsage> sessions;             // largest first
    std::array<uint64_t, kKindCount> kind_bytes{};  // file bytes per Kind without the index
    uint64_t index_bytes = 0;                       // kind_bytes + index_bytes = total.used
    uint64_t catalog_bytes = 0;                     // catalog.db plus its -wal and -shm files
    uint64_t pinned_bytes = 0;
    uint64_t raw_payload_bytes = 0, text_payload_bytes = 0;  // uncompressed, for ratios
    std::optional<Eviction> next_expiry;            // the nearest age-based removal
    std::vector<Eviction> over_cap;                 // what a pass would evict now for the caps (at most 16)
    Policy policy;
};

// `now_ms` decides ages; the disk is read from the volume of `root`.
Result<Usage> usage(Catalog& catalog, const std::filesystem::path& root, const Policy& policy, uint64_t now_ms,
                    const Janitor::PolicyFor& per_session = {});

struct UsageEvent {
    enum class Type : uint8_t { threshold, writes_stopped, writes_resumed };
    Type type = Type::threshold;
    std::string limit;        // "max_total", "max_per_session", "min_free_disk"
    std::string session;      // max_per_session only
    int level = 0;            // threshold: 80 or 95 (rising); 0 when it fell back below 80
    LimitUsage at;
};

// Emits an event when a limit rises past 80 % or 95 %, when it falls back below 80 % after
// that, and when warm writes stop or resume (min_free_disk). The first update() reports the
// levels already reached.
class UsageWatcher {
public:
    explicit UsageWatcher(std::function<void(const UsageEvent&)> on_event) : on_event_(std::move(on_event)) {}
    void update(const Usage& u);
    // From SessionStore when its own disk check stops or resumes writes.
    void writes_stopped(bool stopped);

private:
    void level(const std::string& key, const std::string& limit, const std::string& session, const LimitUsage& at);

    std::function<void(const UsageEvent&)> on_event_;
    std::map<std::string, int> levels_;
    std::optional<bool> stopped_;
};

}  // namespace kt::store
