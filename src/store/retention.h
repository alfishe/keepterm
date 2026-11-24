// Retention, the janitor and compaction.
//
// A janitor pass, in order:
//   1. orphans: segment files without catalog rows, rows without files, stale temp files,
//      empty session folders;
//   2. expiry by age per kind (max_age; raw_max_age for the raw stream; events_max_age) and
//      closed sessions (keep_closed);
//   3. caps: max_per_session, then max_total (closed sessions' oldest segments first, then
//      live sessions'); unsealed segments are never evicted;
//   4. compaction: trim partly expired segments, merge small ones, and repack segments older
//      than cold_after with the cold codec and block size (the index is rebuilt in the same
//      pass). Repacking stops at the pass budget and continues at the next pass.
// Pinned sessions and segments overlapping a pin range are exempt but count toward the caps.
// Deletion removes the catalog rows first, then the file, so a crash leaves only an orphan.
//
// One janitor runs at a time across processes: JanitorThread holds `history/janitor.lock`.
#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/error.h"
#include "store/catalog.h"
#include "store/file_io.h"
#include "store/session_store.h"

namespace kt::config {
class Config;
}

namespace kt::store {

inline constexpr uint64_t kHourMs = 3'600'000;
inline constexpr uint64_t kDayMs = 24 * kHourMs;

struct Policy {
    uint64_t max_total = 5ull << 30;
    uint64_t max_per_session = 1ull << 30;
    uint64_t max_age_ms = 90 * kDayMs;
    uint64_t keep_closed_ms = 30 * kDayMs;
    uint64_t events_max_age_ms = 30 * kDayMs;
    uint64_t raw_max_age_ms = 7 * kDayMs;     // the raw kind; pinned sessions keep it
    DiskGuard disk;
    // The cold tier (storage.cold_*): what aged segments are repacked with.
    WriterOptions cold = cold_defaults();
    uint64_t cold_after_ms = kDayMs;
    uint64_t repack_budget_ms = 10 * 60'000;  // repacking per pass; the rest waits for the next
    // Compaction (fixed in this version).
    uint64_t small_segment_raw = 8ull << 20;  // merge candidates
    uint64_t merge_min_age_ms = kHourMs;      // or the session ended
    uint64_t orphan_grace_ms = 10 * 60'000;   // unsealed rows without files, temp files

    static WriterOptions cold_defaults() {
        WriterOptions o;
        o.codec = Codec::zstd;
        o.level = 19;
        o.block_raw = 1u << 20;
        o.index = IndexMode::on_append;
        return o;
    }
    // The age limit of a kind: events_max_age, the smaller of raw_max_age and max_age for raw,
    // else max_age.
    uint64_t max_age_of(Kind k) const {
        if (k == Kind::events) return events_max_age_ms;
        if (k == Kind::raw) return std::min(raw_max_age_ms, max_age_ms);
        return max_age_ms;
    }
};

// storage.* (schema.inc). Per-project keys come from config::resolve_for_project.
Policy policy_from_config(const config::Config& cfg);
// The fresh tier of storage.codec, codec_level and block_size: the session host's writer
// options. An unavailable codec falls back to zstd at the default level.
WriterOptions fresh_options_from_config(const config::Config& cfg);
// True when `info` was written with the codec, level and block size of `o` (nothing to repack).
bool same_tier(const SegmentInfo& info, const WriterOptions& o);

struct JanitorReport {
    uint32_t orphan_files = 0, orphan_rows = 0, temp_files = 0, empty_dirs = 0;
    uint32_t expired = 0, closed_sessions = 0, over_session_cap = 0, over_total_cap = 0;
    uint32_t merged_inputs = 0, merged_outputs = 0, recompressed = 0, trimmed = 0;  // recompressed: repacked
    uint32_t raw_expired = 0;     // raw segments past raw_max_age (included in expired)
    bool repack_pending = false;  // the budget ran out; more segments wait for the cold tier
    uint64_t freed_bytes = 0;
    uint64_t total_bytes = 0;     // after the pass
    uint64_t pinned_bytes = 0;
    bool pins_over_cap = false;   // pinned data alone exceeds max_total: tell the user
    uint32_t delete_failures = 0; // files a reader still holds (Windows) stay as orphans
};

class Janitor {
public:
    using PolicyFor = std::function<Policy(const SessionRow&)>;
    Janitor(Catalog& catalog, std::filesystem::path root, Policy policy, PolicyFor per_session = {});
    // Polled between segments of the compaction step; true stops the pass early.
    void set_stop(const std::atomic<bool>* stop) { stop_ = stop; }

    Result<JanitorReport> run_pass(uint64_t now_ms);
    // The steps of run_pass, for tests.
    Result<void> clean_orphans(uint64_t now_ms, JanitorReport& rep);
    Result<void> apply_retention(uint64_t now_ms, JanitorReport& rep);
    Result<void> compact(uint64_t now_ms, JanitorReport& rep);

private:
    Policy policy_for(const SessionRow& s) const { return per_session_ ? per_session_(s) : policy_; }
    Result<void> remove_segments(const std::vector<SegmentRow>& rows, JanitorReport& rep);

    Catalog& catalog_;
    std::filesystem::path root_;
    bool stopping() const { return stop_ && stop_->load(); }

    Policy policy_;
    PolicyFor per_session_;
    const std::atomic<bool>* stop_ = nullptr;
};

// Rewrites `inputs` (one session and kind, in series order) into one new sealed segment with
// the codec, level and block size of `out` (its index is built while the records pass), keeping
// the blocks for which `keep` is true, and switches the catalog atomically. Returns the new row,
// or nullopt when nothing was kept (the inputs are then deleted). The old files are removed
// after the switch.
Result<std::optional<SegmentRow>> rewrite_segments(Catalog& catalog, const std::filesystem::path& root,
                                                   const std::vector<SegmentRow>& inputs, const WriterOptions& out,
                                                   const std::function<bool(const BlockEntry&)>& keep,
                                                   uint64_t now_ms);
// The writer options a segment was written with (merge and trim keep its tier).
WriterOptions options_of(const SegmentInfo& info);

// Forgets a session: catalog rows first, then its files and folder.
Result<uint64_t> forget_session(Catalog& catalog, const std::filesystem::path& root, const std::string& id);

inline constexpr const char* kJanitorLock = "janitor.lock";

// Runs the janitor at start and then every `interval` on a low-priority thread, while it holds
// the leader lock; without the lock it retries every interval.
class JanitorThread {
public:
    struct Options {
        std::chrono::milliseconds interval{std::chrono::hours(1)};
        std::function<Policy()> policy;   // read at every pass (live config)
        std::function<void(const JanitorReport&)> on_report;
        // After every pass, with the catalog the pass used (store/usage.h watchers).
        std::function<void(Catalog&, const Policy&)> after_pass;
    };
    JanitorThread(std::filesystem::path root, Options o);
    ~JanitorThread();
    void stop();
    bool leader() const { return leader_.load(); }
    uint32_t passes() const { return passes_.load(); }

private:
    void run();

    std::filesystem::path root_;
    Options opt_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<bool> stop_flag_{false};  // seen by a running pass
    std::atomic<bool> leader_{false};
    std::atomic<uint32_t> passes_{0};
    std::thread thread_;
};

}  // namespace kt::store
