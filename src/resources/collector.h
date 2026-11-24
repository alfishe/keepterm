// The sampler: one OS snapshot per tick, attribution, measurement of the
// attributed processes only, aggregation, change detection and publication. No UI.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "base/error.h"
#include "resources/attribution.h"
#include "resources/model.h"
#include "resources/source.h"

namespace kt::resources {

struct CollectorConfig {
    std::chrono::milliseconds interval{2000};          // resources.interval (1-60 s)
    std::chrono::microseconds refine_budget{5000};     // expensive reads (Linux PSS) per tick
    bool exclude_page_cache = false;                   // resources.linux.exclude_page_cache
    bool forest = false;        // also the forest of every own process (CLI --tree); measures them all
    bool detail = false;        // per-row extras (Linux VmHWM)
    SourceOptions source;
};

// Cost of the collector, measured on the thread that runs the tick.
struct CollectorStats {
    uint64_t ticks = 0;
    uint64_t late_ticks = 0;      // ticks skipped because the previous one overran
    uint64_t changed_ticks = 0;   // publishes that differ from the previous one
    uint64_t last_cpu_ns = 0, last_wall_ns = 0;
    uint64_t max_cpu_ns = 0;
    uint64_t total_cpu_ns = 0;
    uint32_t last_measured = 0;   // processes measured in the last tick
};

// True when `next` differs from `prev` enough to redraw: a session or process count changed,
// or a session, GUI or machine main value moved by at least 1 MiB and 0.5 %.
bool snapshot_changed(const ResourceSnapshot& prev, const ResourceSnapshot& next);

class Collector {
public:
    explicit Collector(CollectorConfig, std::unique_ptr<ProcessSource> source = nullptr);
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    // Runs one tick on the calling thread and publishes it (CLI, tests).
    Result<std::shared_ptr<const ResourceSnapshot>> tick();

    // Starts / stops the collector thread (below-normal priority, monotonic schedule).
    void start();
    void stop();
    // Thread: any. Takes effect at the next tick.
    void set_sessions(std::vector<SessionProcessInfo>);
    void set_interval(std::chrono::milliseconds);
    // Thread: any. The latest published snapshot (nullptr before the first tick).
    std::shared_ptr<const ResourceSnapshot> latest() const;
    // Runs on the collector thread after each publish; it must only wake the UI.
    void on_published(std::function<void(uint64_t tick, bool changed)>);
    CollectorStats stats() const;
    ProcessSource& source() { return *source_; }

private:
    void run();

    CollectorConfig cfg_;
    std::unique_ptr<ProcessSource> source_;
    Attributor attributor_;
    uint32_t self_pid_ = 0;
    std::unordered_set<uint32_t> missing_after_full_;  // see tick()

    std::mutex tick_mu_;  // one tick at a time
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<SessionProcessInfo> sessions_;
    std::shared_ptr<const ResourceSnapshot> latest_;
    std::function<void(uint64_t, bool)> on_published_;
    CollectorStats stats_;
    uint64_t tick_no_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace kt::resources
