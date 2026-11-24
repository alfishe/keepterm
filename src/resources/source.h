// The OS side of the resource monitor: one implementation per OS in
// source_linux.cpp, source_macos.cpp and source_win.cpp. Public, documented APIs only.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/error.h"
#include "resources/model.h"

namespace kt::resources {

// One row of the OS snapshot. take() fills the identity part for every process; measure()
// fills the metrics for the processes the collector asks for.
struct RawProc {
    // Identity (take).
    uint32_t pid = 0, ppid = 0;
    uint64_t start_time = 0;  // ProcessKey units; 0 = unknown (no access)
    std::string name;
    std::optional<uint32_t> threads;
    std::optional<int32_t> pgid, sid;
    std::optional<uint64_t> tty_dev;  // 0 or absent: no controlling terminal
    std::optional<bool> own_user;     // same user as the collector; nullopt = unknown

    // Metrics (measure, refine).
    bool measured = false;
    bool access_denied = false;
    bool main_estimated = false;  // Linux: RssAnon stands in for a PSS that was never read
    uint64_t cpu_ns = 0;
    std::optional<uint64_t> main, resident, private_resident, peak;
    std::optional<uint64_t> committed;  // Windows private commit (address space backed by the page file)

    ProcessKey key() const { return {pid, start_time}; }
};

// Membership and totals of one containment (job object or cgroup).
struct ContainmentReading {
    std::vector<uint32_t> pids;
    // Processes created after this instant (ProcessKey units) are dropped from `pids` for this
    // tick: the PID list has no creation times, so a reused PID would be wrong.
    std::optional<uint64_t> read_at;
    std::optional<uint64_t> main;        // Linux memory.current
    std::optional<uint64_t> peak;        // Windows PeakJobMemoryUsed, Linux memory.peak
    std::optional<uint64_t> file_cache;  // Linux memory.stat "file"
    std::optional<uint64_t> shmem;       // Linux memory.stat "shmem"
    std::optional<uint64_t> cpu_ns;      // lifetime CPU including exited processes
};

struct MeasureOptions {
    bool detail = false;  // also the per-row extras (Linux VmHWM from /proc/<pid>/status)
};

struct ProcessDetails {
    std::optional<std::string> path;               // full image path
    std::optional<std::vector<std::string>> argv;  // nullopt when not readable on the default path
};

struct SourceOptions {
    // Extended feature (Windows, opt-in, extended.nt_process_info): NtQuerySystemInformation and
    // NtQueryInformationProcess from src/ext/win/procinfo/. Not implemented yet: when set, the
    // source records why it fell back and uses the documented path (Toolhelp + PSAPI).
    bool extended_nt_process_info = false;
};

class ProcessSource {
public:
    static std::unique_ptr<ProcessSource> create(const SourceOptions& = {});
    virtual ~ProcessSource() = default;

    virtual OsKind os() const = 0;
    // Names of the main and resident metrics for the JSON "metric" object.
    virtual std::string_view main_metric_name() const = 0;
    virtual std::string_view resident_metric_name() const = 0;
    // Why an extended feature that was asked for is not active; empty when none was asked for.
    virtual std::string_view extended_status() const { return {}; }

    // One OS snapshot of all processes, identity fields only. The rows stay valid until the next
    // take(); measure() and refine() write into them.
    virtual Result<std::span<RawProc>> take() = 0;
    // A source may answer take() from its cache between full OS snapshots (Windows: Toolhelp is
    // expensive and parents never change there). Then the snapshot can miss new processes:
    // the collector forces a full one when a containment or a seed names a PID it lacks.
    virtual bool last_take_partial() const { return false; }
    virtual void force_full_next() {}
    // Cheap per-process metrics of the given rows (rows of the last take()).
    virtual void measure(std::span<RawProc* const> rows, const MeasureOptions& = {}) = 0;
    // Expensive refinement (Linux PSS) within the budget. Returns false when the budget ran out.
    virtual bool refine(std::span<RawProc* const> rows, std::chrono::microseconds budget) = 0;
    // Membership and totals of a Job or Cgroup containment; not_supported for other kinds.
    virtual Result<ContainmentReading> read_containment(const Containment&) = 0;
    // Full path and argv of one process, on demand. Checked against the start time.
    virtual ProcessDetails details(const ProcessKey&) = 0;
};

// CPU time of the calling thread in ns (the cost meter of the sampler).
uint64_t current_thread_cpu_ns();
// Lowers the calling thread's priority (below normal / nice 5 / QoS utility).
void lower_current_thread_priority();
uint32_t current_pid();

}  // namespace kt::resources
