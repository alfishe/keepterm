// Follows one context target (agents/context.h): tail read on attach, incremental reads from the
// saved offset, rotation and replacement, and the read rate. Core
// code: files through IFileSystem, time from the caller. One tracker per pane, one thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "agents/context.h"

namespace kt::agents {

struct TrackerOptions {
    std::size_t tail_min = 256u * 1024;        // first tail read on attach
    std::size_t tail_max = 8u * 1024 * 1024;   // doubled up to this while no usage record is found
    std::int64_t min_interval_ms = 1000;       // reads at most this often
    std::int64_t safety_interval_ms = 10'000;  // stat without an event (missed watcher events)
    std::size_t max_read = 64u * 1024 * 1024;  // a larger jump re-attaches from the tail
};

class ContextTracker {
public:
    ContextTracker(const IFileSystem& fs, TrackerOptions o = {}) : fs_(fs), o_(o) {}

    // Follows `t` with a reducer from `probe`. The same target keeps its state; another target
    // (or nullopt) drops it. Returns true when the usage changed.
    bool follow(const IContextProbe* probe, const std::optional<ContextTarget>& t, std::int64_t now_ms);
    // `event`: the watcher saw a change of the file's folder. Reads when an event is pending and
    // the rate allows, or when the safety check is due. Returns true when the usage changed.
    bool poll(std::int64_t now_ms, bool event);

    const std::optional<ContextTarget>& target() const { return target_; }
    // nullptr while nothing is followed.
    const ContextUsage* usage() const { return reducer_ ? &reducer_->usage() : nullptr; }
    std::uint64_t offset() const { return offset_; }
    // Bytes read since the target was attached (tests check the tail bound).
    std::uint64_t bytes_read() const { return bytes_read_; }

private:
    bool attach();
    bool read_more(const IFileSystem::FileStat& st);
    std::string head_of() const;
    std::string db_signature() const;

    const IFileSystem& fs_;
    TrackerOptions o_;
    std::optional<ContextTarget> target_;
    std::unique_ptr<IContextReducer> reducer_;
    std::uint64_t offset_ = 0;
    std::uint64_t file_id_ = 0;
    std::string head_;   // the first bytes of the file: a replacement changes them
    std::int64_t last_read_ms_ = 0;
    std::int64_t last_check_ms_ = 0;
    bool pending_ = false;
    std::uint64_t bytes_read_ = 0;
    std::string db_sig_;   // rereads(): sizes and times of the database and its -wal
};

}  // namespace kt::agents
