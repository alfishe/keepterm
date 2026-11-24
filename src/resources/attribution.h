// Attribution and aggregation (steps 3 and 5 of the tick). Core code: it
// works on RawProc rows and has no OS headers, so the unit tests feed it synthetic snapshots.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/model.h"
#include "resources/source.h"

namespace kt::resources {

// Validated parent links of one snapshot: a link counts only when the parent
// exists and did not start after the child. Indices are row indices; -1 = none.
struct ProcGraph {
    std::vector<int32_t> parent;
    std::vector<uint32_t> child_begin;  // CSR: children of row i are child_list[child_begin[i] .. [i+1])
    std::vector<uint32_t> child_list;
    std::unordered_map<uint32_t, uint32_t> by_pid;

    static ProcGraph build(std::span<const RawProc> rows);
    std::span<const uint32_t> children(size_t i) const {
        return {child_list.data() + child_begin[i], child_list.data() + child_begin[i + 1]};
    }
    // The row of `key`; the start time must match when both sides know it.
    std::optional<uint32_t> find(std::span<const RawProc> rows, const ProcessKey& key) const;
};

struct Assignment {
    int32_t session = -1;  // index into the session list; -1 = not attributed
    Role role = Role::Workload;
    Attribution how = Attribution::ParentWalk;
    uint8_t rank = 0;      // priority class of the winning rule (conflict resolution)
    bool escaped = false;
};

struct AttributionInput {
    std::span<const RawProc> rows;
    std::span<const SessionProcessInfo> sessions;
    // Per session (same index): the containment reading, when the kind has one and it worked.
    std::span<const std::optional<ContainmentReading>> readings;
    uint32_t self_pid = 0;  // the collecting process (Role::Gui); never part of a session
};

struct BuildOptions {
    bool exclude_page_cache = false;  // resources.linux.exclude_page_cache
    bool forest = false;              // also the machine forest of own processes (CLI --tree)
};

class Attributor {
public:
    // Step 3: at most one session per row, with the reason. Reads the sticky map.
    std::vector<Assignment> assign(const AttributionInput&, const ProcGraph&) const;

    // Step 5: trees, subtree sums, session/machine totals, CPU %. Updates the sticky map, the
    // previous CPU times and the sampled session peaks. `rows` must be measured.
    MachineSample build(const AttributionInput&, const ProcGraph&, const std::vector<Assignment>&, OsKind,
                        std::chrono::steady_clock::time_point now, const BuildOptions& = {});

    // Every process of our user as a forest by validated parent links, with subtree sums.
    static std::vector<ProcNode> build_forest(std::span<const RawProc> rows, const ProcGraph&,
                                              uint32_t self_pid);

    size_t sticky_size() const { return sticky_.size(); }

private:
    std::unordered_map<ProcessKey, std::string, ProcessKeyHash> sticky_;  // key -> session id
    std::unordered_map<ProcessKey, uint64_t, ProcessKeyHash> prev_cpu_;
    std::optional<std::chrono::steady_clock::time_point> prev_time_;
    std::unordered_map<std::string, uint64_t> sampled_peak_;
};

// Metrics of one measured row.
Metrics self_metrics(const RawProc&);

// Collapses a tree above kCollapseThreshold processes: the root keeps the kCollapseKeep largest
// processes (by main, flattened) and one synthetic "other" node; totals stay exact.
void collapse_tree(ProcNode& root);

}  // namespace kt::resources
