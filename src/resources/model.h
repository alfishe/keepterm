// Resource monitor data model. Core code: no OS headers.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resources/containment.h"

namespace kt::resources {

// Identity across PID reuse. start_time is in OS-native units: Linux clock ticks since boot,
// macOS microseconds since the epoch, Windows FILETIME. 0 means "unknown" (no access).
struct ProcessKey {
    uint32_t pid = 0;
    uint64_t start_time = 0;
    auto operator<=>(const ProcessKey&) const = default;
};

struct ProcessKeyHash {
    size_t operator()(const ProcessKey& k) const noexcept {
        return std::hash<uint64_t>{}(k.start_time * 0x9E3779B97F4A7C15ull ^ k.pid);
    }
};

enum class Role : uint8_t {
    Gui,           // the keepterm process that collects (machine-level overhead)
    SessionHost,   // keepterm --session-host
    ConsoleAgent,  // keepterm --console-agent (Windows 1607)
    ConsoleHost,   // conhost.exe / OpenConsole.exe serving the session's terminal
    Workload,      // the shell and everything below it
};
inline bool is_overhead(Role r) { return r != Role::Workload; }

enum class Attribution : uint8_t {
    Seed,          // reported by the host: host, agent, console host, root
    Job,           // in the session's job object
    Cgroup,        // in the session's cgroup
    Subreaper,     // descendant of the host on Linux with PR_SET_CHILD_SUBREAPER
    ParentWalk,    // parent chain reaches a session member
    Tracked,       // in the host's tracked set or sticky from an earlier tick
    SessionOrTty,  // same POSIX session id, process group or controlling tty as the shell
};

struct Containment {
    ContainmentKind kind = ContainmentKind::None;
    std::string handle;  // job name, cgroup path relative to the cgroup2 mount, or empty
    Accuracy accuracy = Accuracy::BestEffort;
};

// All sizes in bytes. An optional field is nullopt when the OS cannot provide it.
struct Metrics {
    uint64_t main = 0;                         // private working set / footprint / memory.current or PSS
    std::optional<uint64_t> resident;          // working set / RSS
    std::optional<uint64_t> private_resident;  // Windows private working set; Linux RssAnon
    std::optional<uint64_t> peak;              // OS peak where available, else sampled max
    std::optional<uint64_t> committed;         // Windows private commit; not memory in use
    std::optional<uint64_t> file_cache;        // Linux cgroup memory.stat "file"
    uint64_t cpu_ns = 0;                       // user + kernel, cumulative
    std::optional<float> cpu_percent;          // of one core, over the last tick
    uint32_t processes = 0;
    uint32_t threads = 0;
    uint32_t no_access = 0;                    // processes the OS refused to measure
    uint32_t estimated = 0;                    // processes whose main value is a stand-in (Linux)

    // Adds `o` (a disjoint set of processes). Optional fields stay nullopt only when both are.
    void add(const Metrics& o);
};

struct ProcNode {
    ProcessKey key;
    uint32_t ppid = 0;
    std::string name;  // image name; full path and argv on demand
    Role role = Role::Workload;
    Attribution how = Attribution::ParentWalk;
    bool reparented = false;  // parent is gone or outside the session
    bool escaped = false;     // outside the job while its parent is in it (Windows breakaway)
    bool no_access = false;
    bool synthetic = false;   // "other" node of a collapsed tree
    Metrics self, subtree;
    std::vector<ProcNode> children;
};

struct SessionSample {
    std::string id;
    Containment containment;
    Accuracy accuracy = Accuracy::BestEffort;  // min over containment and attributions used
    Metrics overhead, workload, total;
    std::optional<uint64_t> lifetime_cpu_ns;  // job / cgroup accounting, includes exited processes
    bool peak_sampled = false;                // total.peak is the collector's sampled maximum
    bool collapsed = false;  // more than kCollapseThreshold processes: top nodes + "other"
    ProcNode root;           // the session host, or the root process when no host is known
};

struct TerminalSample {
    Metrics gui;
    Metrics hosts_and_agents;
};

enum class OsKind : uint8_t { Windows, Linux, MacOS };

struct MachineSample {
    std::string display_name = "This computer";
    OsKind os = OsKind::Linux;
    TerminalSample terminal;
    std::vector<SessionSample> sessions;
    Metrics total;                 // GUI + all sessions
    uint32_t visible_processes = 0;
    std::vector<ProcNode> forest;  // every own process; filled only on request (CLI --tree)
};

struct ResourceSnapshot {
    uint64_t tick = 0;
    std::chrono::system_clock::time_point taken_at;
    std::chrono::milliseconds interval{0};
    bool changed = true;  // differs from the previous publish (see change detection in collector.h)
    std::vector<MachineSample> machines;
};

// Input from the session hosts: what each host reported in PROCESS_INFO.
struct SessionProcessInfo {
    std::string id;
    std::optional<ProcessKey> host;  // absent: a bare root (CLI --pid, tests)
    ProcessKey root;
    std::optional<ProcessKey> agent, console_host;
    Containment containment;
    std::vector<ProcessKey> tracked;  // macOS: every descendant the host saw
    std::optional<int32_t> posix_sid;
    std::optional<uint64_t> tty_dev;
};

// Above this many processes a session tree is collapsed.
inline constexpr size_t kCollapseThreshold = 5000;
inline constexpr size_t kCollapseKeep = 200;

std::string_view to_string(Role);
std::string_view to_string(Attribution);
std::string_view to_string(ContainmentKind);
std::string_view to_string(Accuracy);
std::string_view to_string(OsKind);

}  // namespace kt::resources
