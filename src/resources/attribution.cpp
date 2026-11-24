#include "resources/attribution.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace kt::resources {
namespace {

// Priority classes of the attribution rules (higher wins).
enum Rank : uint8_t {
    kRankNone = 0,
    kRankSessionOrTty = 1,
    kRankParentWalk = 2,
    kRankTracked = 3,
    kRankExact = 4,
    kRankSeed = 5,
};

bool starts_after(const RawProc& parent, const RawProc& child) {
    return parent.start_time != 0 && child.start_time != 0 && parent.start_time > child.start_time;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool is_console_host_image(std::string_view name) {
    return iequals(name, "conhost.exe") || iequals(name, "OpenConsole.exe");
}

void add_opt(std::optional<uint64_t>& a, const std::optional<uint64_t>& b) {
    if (b) a = a.value_or(0) + *b;
}

}  // namespace

void Metrics::add(const Metrics& o) {
    main += o.main;
    add_opt(resident, o.resident);
    add_opt(private_resident, o.private_resident);
    add_opt(peak, o.peak);
    add_opt(committed, o.committed);
    add_opt(file_cache, o.file_cache);
    cpu_ns += o.cpu_ns;
    if (o.cpu_percent) cpu_percent = cpu_percent.value_or(0.0f) + *o.cpu_percent;
    processes += o.processes;
    threads += o.threads;
    no_access += o.no_access;
    estimated += o.estimated;
}

std::string_view to_string(Role r) {
    switch (r) {
        case Role::Gui: return "gui";
        case Role::SessionHost: return "session_host";
        case Role::ConsoleAgent: return "console_agent";
        case Role::ConsoleHost: return "console_host";
        case Role::Workload: return "workload";
    }
    return "workload";
}

std::string_view to_string(Attribution a) {
    switch (a) {
        case Attribution::Seed: return "seed";
        case Attribution::Job: return "job";
        case Attribution::Cgroup: return "cgroup";
        case Attribution::Subreaper: return "subreaper";
        case Attribution::ParentWalk: return "parent_walk";
        case Attribution::Tracked: return "tracked";
        case Attribution::SessionOrTty: return "session_or_tty";
    }
    return "parent_walk";
}

std::string_view to_string(ContainmentKind k) {
    switch (k) {
        case ContainmentKind::Job: return "job";
        case ContainmentKind::Cgroup: return "cgroup";
        case ContainmentKind::CgroupNoMemory: return "cgroup_no_memory";
        case ContainmentKind::Subreaper: return "subreaper";
        case ContainmentKind::Tracked: return "tracked";
        case ContainmentKind::None: return "none";
    }
    return "none";
}

std::string_view to_string(Accuracy a) { return a == Accuracy::Exact ? "exact" : "best_effort"; }

std::string_view to_string(OsKind o) {
    switch (o) {
        case OsKind::Windows: return "windows";
        case OsKind::Linux: return "linux";
        case OsKind::MacOS: return "macos";
    }
    return "linux";
}

// ---------------------------------------------------------------------------------------------

ProcGraph ProcGraph::build(std::span<const RawProc> rows) {
    ProcGraph g;
    const size_t n = rows.size();
    g.by_pid.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) g.by_pid.emplace(rows[i].pid, static_cast<uint32_t>(i));
    g.parent.assign(n, -1);
    std::vector<uint32_t> count(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        const RawProc& c = rows[i];
        if (c.ppid == c.pid) continue;
        auto it = g.by_pid.find(c.ppid);
        if (it == g.by_pid.end() || it->second == i) continue;
        if (starts_after(rows[it->second], c)) continue;  // the parent PID was reused
        g.parent[i] = static_cast<int32_t>(it->second);
        ++count[it->second];
    }
    g.child_begin.assign(n + 1, 0);
    for (size_t i = 0; i < n; ++i) g.child_begin[i + 1] = g.child_begin[i] + count[i];
    g.child_list.assign(g.child_begin[n], 0);
    std::vector<uint32_t> fill(g.child_begin.begin(), g.child_begin.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        if (g.parent[i] >= 0) g.child_list[fill[static_cast<size_t>(g.parent[i])]++] = static_cast<uint32_t>(i);
    }
    return g;
}

std::optional<uint32_t> ProcGraph::find(std::span<const RawProc> rows, const ProcessKey& key) const {
    auto it = by_pid.find(key.pid);
    if (it == by_pid.end()) return std::nullopt;
    const RawProc& r = rows[it->second];
    if (key.start_time != 0 && r.start_time != 0 && key.start_time != r.start_time) return std::nullopt;
    return it->second;
}

Metrics self_metrics(const RawProc& r) {
    Metrics m;
    m.processes = 1;
    m.threads = r.threads.value_or(0);
    if (r.access_denied) m.no_access = 1;
    if (!r.measured) return m;
    m.main = r.main.value_or(0);
    m.resident = r.resident;
    m.private_resident = r.private_resident;
    m.peak = r.peak;
    m.committed = r.committed;
    m.cpu_ns = r.cpu_ns;
    if (r.main_estimated) m.estimated = 1;
    return m;
}

// ---------------------------------------------------------------------------------------------

std::vector<Assignment> Attributor::assign(const AttributionInput& in, const ProcGraph& g) const {
    const auto rows = in.rows;
    const size_t n = rows.size();
    std::vector<Assignment> best(n);
    std::vector<uint64_t> root_start(in.sessions.size(), 0);
    for (size_t s = 0; s < in.sessions.size(); ++s) root_start[s] = in.sessions[s].root.start_time;

    auto offer = [&](uint32_t i, size_t s, uint8_t rank, Attribution how, Role role) {
        if (rows[i].pid == in.self_pid) return false;
        Assignment& a = best[i];
        const bool wins = a.session < 0 || rank > a.rank ||
                          (rank == a.rank && a.session != static_cast<int32_t>(s) &&
                           root_start[s] > root_start[static_cast<size_t>(a.session)]);
        if (!wins) return false;
        a = Assignment{static_cast<int32_t>(s), role, how, rank, false};
        return true;
    };

    // Session members of the strong rules, per session, as the frontier of the expansion.
    std::vector<std::vector<uint32_t>> members(in.sessions.size());
    std::vector<std::unordered_set<uint32_t>> job_pids(in.sessions.size());

    for (size_t s = 0; s < in.sessions.size(); ++s) {
        const SessionProcessInfo& si = in.sessions[s];
        auto seed = [&](const std::optional<ProcessKey>& k, Role role) {
            if (!k) return;
            if (auto i = g.find(rows, *k)) {
                offer(*i, s, kRankSeed, Attribution::Seed, role);
                members[s].push_back(*i);
            }
        };
        seed(si.host, Role::SessionHost);
        seed(si.agent, Role::ConsoleAgent);
        seed(si.console_host, Role::ConsoleHost);
        seed(si.root, Role::Workload);

        if (s < in.readings.size() && in.readings[s]) {
            const ContainmentReading& rd = *in.readings[s];
            const Attribution how = si.containment.kind == ContainmentKind::Job ? Attribution::Job
                                                                                 : Attribution::Cgroup;
            for (uint32_t pid : rd.pids) {
                auto it = g.by_pid.find(pid);
                if (it == g.by_pid.end()) continue;
                const RawProc& r = rows[it->second];
                // A PID that started after the containment read may be a reused one (rule 2).
                if (rd.read_at && r.start_time != 0 && r.start_time > *rd.read_at) continue;
                job_pids[s].insert(pid);
                offer(it->second, s, kRankExact, how, Role::Workload);
                members[s].push_back(it->second);
            }
        }
        for (const ProcessKey& k : si.tracked) {
            if (auto i = g.find(rows, k)) {
                offer(*i, s, kRankTracked, Attribution::Tracked, Role::Workload);
                members[s].push_back(*i);
            }
        }
    }
    // Sticky map (rule 4): once attributed, a process stays until it exits.
    if (!sticky_.empty()) {
        std::unordered_map<std::string_view, size_t> by_id;
        for (size_t s = 0; s < in.sessions.size(); ++s) by_id.emplace(in.sessions[s].id, s);
        for (size_t i = 0; i < n; ++i) {
            if (rows[i].start_time == 0) continue;
            auto it = sticky_.find(rows[i].key());
            if (it == sticky_.end()) continue;
            auto s = by_id.find(it->second);
            if (s == by_id.end()) continue;
            offer(static_cast<uint32_t>(i), s->second, kRankTracked, Attribution::Tracked, Role::Workload);
            members[s->second].push_back(static_cast<uint32_t>(i));
        }
    }

    // Expansion: descendants of the members by validated parent links.
    std::vector<uint32_t> stack;
    std::vector<uint8_t> seen(n, 0);
    auto expand = [&](size_t s, std::vector<uint32_t> frontier, uint8_t rank, Attribution how) {
        std::fill(seen.begin(), seen.end(), uint8_t{0});
        stack = std::move(frontier);
        for (uint32_t i : stack) seen[i] = 1;
        const bool job = in.sessions[s].containment.kind == ContainmentKind::Job && s < in.readings.size() &&
                         in.readings[s].has_value();
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            for (uint32_t c : g.children(i)) {
                if (seen[c]) continue;
                seen[c] = 1;
                if (offer(c, s, rank, how, Role::Workload) && job && !job_pids[s].count(rows[c].pid) &&
                    job_pids[s].count(rows[i].pid))
                {
                    best[c].escaped = true;  // the parent is in the job, the child is not (rule 3)
                }
                stack.push_back(c);
            }
        }
    };
    for (size_t s = 0; s < in.sessions.size(); ++s) {
        const Attribution how = in.sessions[s].containment.kind == ContainmentKind::Subreaper
                                    ? Attribution::Subreaper
                                    : Attribution::ParentWalk;
        expand(s, members[s], kRankParentWalk, how);
    }

    // POSIX nets: same session id, controlling tty or a process group the workload used, our uid.
    // The collector's own process group, session and tty are never a net: a workload that
    // shares them (started without a PTY) would pull in unrelated processes.
    std::optional<int32_t> self_pgid, self_sid;
    std::optional<uint64_t> self_tty;
    if (auto it = g.by_pid.find(in.self_pid); it != g.by_pid.end()) {
        self_pgid = rows[it->second].pgid;
        self_sid = rows[it->second].sid;
        self_tty = rows[it->second].tty_dev;
    }
    for (size_t s = 0; s < in.sessions.size(); ++s) {
        const SessionProcessInfo& si = in.sessions[s];
        std::unordered_set<int32_t> pgids;
        for (size_t i = 0; i < n; ++i) {
            const Assignment& a = best[i];
            if (a.session == static_cast<int32_t>(s) && a.role == Role::Workload && rows[i].pgid &&
                *rows[i].pgid > 1 && rows[i].pgid != self_pgid)
                pgids.insert(*rows[i].pgid);
        }
        // Plain values instead of optionals (gcc 14 -O2 reports a false maybe-uninitialized on
        // them): tty 0 is "no terminal" anyway.
        const bool use_sid = si.posix_sid && si.posix_sid != self_sid;
        const int32_t sid = use_sid ? *si.posix_sid : 0;
        const uint64_t tty = si.tty_dev && *si.tty_dev != 0 && si.tty_dev != self_tty ? *si.tty_dev : 0;
        if (!use_sid && tty == 0 && pgids.empty()) continue;
        std::vector<uint32_t> caught;
        for (size_t i = 0; i < n; ++i) {
            const RawProc& r = rows[i];
            if (best[i].session >= 0 || r.own_user != true || r.pid == in.self_pid) continue;
            const bool match = (use_sid && r.sid && *r.sid == sid) || (tty != 0 && r.tty_dev && *r.tty_dev == tty) ||
                               (r.pgid && pgids.count(*r.pgid));
            if (match && offer(static_cast<uint32_t>(i), s, kRankSessionOrTty, Attribution::SessionOrTty,
                               Role::Workload))
                caught.push_back(static_cast<uint32_t>(i));
        }
        if (!caught.empty()) expand(s, std::move(caught), kRankSessionOrTty, Attribution::SessionOrTty);
    }

    // Console hosts found by image name below the session host or the console agent.
    for (size_t i = 0; i < n; ++i) {
        Assignment& a = best[i];
        if (a.session < 0 || a.role != Role::Workload || !is_console_host_image(rows[i].name)) continue;
        const int32_t p = g.parent[i];
        if (p < 0) continue;
        const Assignment& pa = best[static_cast<size_t>(p)];
        if (pa.session == a.session && (pa.role == Role::SessionHost || pa.role == Role::ConsoleAgent))
            a.role = Role::ConsoleHost;
    }
    return best;
}

// ---------------------------------------------------------------------------------------------

namespace {

struct TreeBuilder {
    std::span<const RawProc> rows;
    const std::vector<Assignment>* assign = nullptr;
    const std::vector<std::vector<uint32_t>>* kids = nullptr;
    const std::vector<Metrics>* self = nullptr;
    const std::vector<uint8_t>* reparented = nullptr;

    ProcNode make(uint32_t i) const {
        const RawProc& r = rows[i];
        ProcNode node;
        node.key = r.key();
        node.ppid = r.ppid;
        node.name = r.name;
        node.no_access = r.access_denied;
        if (assign) {
            const Assignment& a = (*assign)[i];
            node.role = a.role;
            node.how = a.how;
            node.escaped = a.escaped;
        }
        if (reparented) node.reparented = (*reparented)[i] != 0;
        node.self = (*self)[i];
        node.subtree = node.self;
        for (uint32_t c : (*kids)[i]) {
            node.children.push_back(make(c));
            node.subtree.add(node.children.back().subtree);
        }
        return node;
    }
};

void sort_children(ProcNode& n) {
    std::sort(n.children.begin(), n.children.end(),
              [](const ProcNode& a, const ProcNode& b) { return a.key < b.key; });
    for (auto& c : n.children) sort_children(c);
}

void flatten(ProcNode&& n, std::vector<ProcNode>& out) {
    std::vector<ProcNode> kids = std::move(n.children);
    n.children.clear();
    n.subtree = n.self;
    out.push_back(std::move(n));
    for (auto& k : kids) flatten(std::move(k), out);
}

}  // namespace

void collapse_tree(ProcNode& root) {
    if (root.subtree.processes <= kCollapseThreshold) return;
    std::vector<ProcNode> all;
    for (auto& c : root.children) flatten(std::move(c), all);
    root.children.clear();
    std::sort(all.begin(), all.end(), [](const ProcNode& a, const ProcNode& b) { return a.self.main > b.self.main; });
    ProcNode other;
    other.synthetic = true;
    other.name = "other";
    other.reparented = false;
    for (size_t i = 0; i < all.size(); ++i) {
        if (i < kCollapseKeep) {
            root.children.push_back(std::move(all[i]));
        } else {
            other.self.add(all[i].self);
        }
    }
    other.subtree = other.self;
    root.children.push_back(std::move(other));
}

MachineSample Attributor::build(const AttributionInput& in, const ProcGraph& g, const std::vector<Assignment>& asg,
                                OsKind os, std::chrono::steady_clock::time_point now, const BuildOptions& opt) {
    const auto rows = in.rows;
    const size_t n = rows.size();
    MachineSample m;
    m.os = os;
    m.visible_processes = static_cast<uint32_t>(n);

    // Self metrics with CPU % from the previous tick (same ProcessKey only).
    std::vector<Metrics> self(n);
    std::unordered_map<ProcessKey, uint64_t, ProcessKeyHash> cpu_now;
    const double dt_ns = prev_time_ ? static_cast<double>(
                                          std::chrono::duration_cast<std::chrono::nanoseconds>(now - *prev_time_).count())
                                    : 0.0;
    for (size_t i = 0; i < n; ++i) {
        self[i] = self_metrics(rows[i]);
        if (!rows[i].measured || rows[i].access_denied || rows[i].start_time == 0) continue;
        cpu_now.emplace(rows[i].key(), rows[i].cpu_ns);
        if (dt_ns <= 0) continue;
        auto it = prev_cpu_.find(rows[i].key());
        if (it == prev_cpu_.end() || rows[i].cpu_ns < it->second) continue;
        self[i].cpu_percent = static_cast<float>(static_cast<double>(rows[i].cpu_ns - it->second) * 100.0 / dt_ns);
    }

    // The GUI (the collector's own process), machine-level.
    if (auto it = g.by_pid.find(in.self_pid); it != g.by_pid.end()) {
        m.terminal.gui = self[it->second];
    }

    std::unordered_map<ProcessKey, std::string, ProcessKeyHash> sticky_next;
    std::vector<std::vector<uint32_t>> kids(n);
    std::vector<uint8_t> reparented(n, 0);
    std::vector<std::vector<uint32_t>> per_session(in.sessions.size());
    for (size_t i = 0; i < n; ++i) {
        if (asg[i].session >= 0) per_session[static_cast<size_t>(asg[i].session)].push_back(static_cast<uint32_t>(i));
    }

    TreeBuilder tb{rows, &asg, &kids, &self, &reparented};
    for (size_t s = 0; s < in.sessions.size(); ++s) {
        const SessionProcessInfo& si = in.sessions[s];
        const auto& mem = per_session[s];
        SessionSample ss;
        ss.id = si.id;
        ss.containment = si.containment;

        auto member_row = [&](const std::optional<ProcessKey>& k) -> std::optional<uint32_t> {
            if (!k) return std::nullopt;
            auto i = g.find(rows, *k);
            if (i && asg[*i].session == static_cast<int32_t>(s)) return i;
            return std::nullopt;
        };
        std::optional<uint32_t> root = member_row(si.host);
        if (!root) root = member_row(si.root);

        // Parent of each member inside the session; others hang under the root (rule 6).
        for (uint32_t i : mem) {
            if (root && i == *root) continue;
            const int32_t p = g.parent[i];
            if (p >= 0 && asg[static_cast<size_t>(p)].session == static_cast<int32_t>(s)) {
                kids[static_cast<size_t>(p)].push_back(i);
            } else {
                reparented[i] = 1;
            }
        }
        // Members not reachable from the root (cycles through unknown start times, orphans) are
        // attached to the root as reparented.
        std::vector<uint8_t> reach(n, 0);
        std::vector<uint32_t> st;
        if (root) {
            st.push_back(*root);
            reach[*root] = 1;
        }
        for (uint32_t i : mem) {
            if (reparented[i] && !(root && i == *root)) {
                st.push_back(i);
                reach[i] = 1;
            }
        }
        while (!st.empty()) {
            const uint32_t i = st.back();
            st.pop_back();
            for (uint32_t c : kids[i]) {
                if (!reach[c]) {
                    reach[c] = 1;
                    st.push_back(c);
                }
            }
        }
        std::vector<uint32_t> top;
        for (uint32_t i : mem) {
            if (root && i == *root) continue;
            if (!reach[i]) {
                // In a cycle: cut the link into it.
                const int32_t p = g.parent[i];
                if (p >= 0) {
                    auto& pk = kids[static_cast<size_t>(p)];
                    pk.erase(std::remove(pk.begin(), pk.end(), i), pk.end());
                }
                reparented[i] = 1;
                reach[i] = 1;
            }
            if (reparented[i]) top.push_back(i);
        }

        if (root) {
            for (uint32_t i : top) kids[*root].push_back(i);
            ss.root = tb.make(*root);
        } else {
            // The root process is gone; its leftovers stay visible under a synthetic node.
            ss.root.synthetic = true;
            ss.root.name = "(exited)";
            ss.root.key = si.root;
            for (uint32_t i : top) {
                ss.root.children.push_back(tb.make(i));
                ss.root.subtree.add(ss.root.children.back().subtree);
            }
        }
        sort_children(ss.root);

        bool escaped = false;
        for (uint32_t i : mem) {
            const Metrics& sm = self[i];
            if (is_overhead(asg[i].role)) {
                ss.overhead.add(sm);
            } else {
                ss.workload.add(sm);
            }
            escaped = escaped || asg[i].escaped;
            if (rows[i].start_time != 0) sticky_next.emplace(rows[i].key(), si.id);
        }
        ss.total = ss.overhead;
        ss.total.add(ss.workload);

        const ContainmentReading* rd =
            s < in.readings.size() && in.readings[s] ? &*in.readings[s] : nullptr;
        if (rd && rd->main) {
            uint64_t main = *rd->main;
            if (opt.exclude_page_cache && rd->file_cache) {
                const uint64_t reclaim = *rd->file_cache - std::min(*rd->file_cache, rd->shmem.value_or(0));
                main -= std::min(main, reclaim);
            }
            ss.total.main = main;
        }
        if (rd) {
            ss.total.file_cache = rd->file_cache;
            ss.lifetime_cpu_ns = rd->cpu_ns;
        }
        uint64_t& sampled = sampled_peak_[si.id];
        sampled = std::max(sampled, ss.total.main);
        if (rd && rd->peak) {
            ss.total.peak = std::max(*rd->peak, ss.total.main);
        } else {
            ss.total.peak = sampled;
            ss.peak_sampled = true;
        }
        ss.accuracy = si.containment.accuracy == Accuracy::Exact && !escaped && ss.total.no_access == 0
                          ? Accuracy::Exact
                          : Accuracy::BestEffort;
        if (ss.root.subtree.processes > kCollapseThreshold) {
            collapse_tree(ss.root);
            ss.collapsed = true;
        }
        m.terminal.hosts_and_agents.add(ss.overhead);
        m.sessions.push_back(std::move(ss));
    }

    m.total = m.terminal.gui;
    for (const auto& ss : m.sessions) m.total.add(ss.total);
    if (opt.forest) m.forest = build_forest(rows, g, in.self_pid);
    if (opt.forest) {
        // build_forest has no CPU history: copy the CPU % computed above.
        std::unordered_map<uint32_t, float> pct;
        for (size_t i = 0; i < n; ++i) {
            if (self[i].cpu_percent) pct.emplace(rows[i].pid, *self[i].cpu_percent);
        }
        auto fill = [&](auto&& me, ProcNode& node) -> void {
            if (auto it = pct.find(node.key.pid); it != pct.end()) node.self.cpu_percent = it->second;
            node.subtree = node.self;
            for (auto& c : node.children) {
                me(me, c);
                node.subtree.add(c.subtree);
            }
        };
        for (auto& r : m.forest) fill(fill, r);
    }

    // Keep the per-session sampled peak only for live sessions.
    for (auto it = sampled_peak_.begin(); it != sampled_peak_.end();) {
        const bool live = std::any_of(in.sessions.begin(), in.sessions.end(),
                                      [&](const SessionProcessInfo& si) { return si.id == it->first; });
        it = live ? std::next(it) : sampled_peak_.erase(it);
    }
    sticky_ = std::move(sticky_next);
    prev_cpu_ = std::move(cpu_now);
    prev_time_ = now;
    return m;
}

std::vector<ProcNode> Attributor::build_forest(std::span<const RawProc> rows, const ProcGraph& g, uint32_t self_pid) {
    const size_t n = rows.size();
    std::vector<uint8_t> in(n, 0);
    for (size_t i = 0; i < n; ++i) in[i] = rows[i].own_user == true || rows[i].pid == self_pid;
    std::vector<std::vector<uint32_t>> kids(n);
    std::vector<Metrics> self(n);
    std::vector<uint32_t> roots;
    for (size_t i = 0; i < n; ++i) {
        if (!in[i]) continue;
        self[i] = self_metrics(rows[i]);
        const int32_t p = g.parent[i];
        if (p >= 0 && in[static_cast<size_t>(p)]) {
            kids[static_cast<size_t>(p)].push_back(static_cast<uint32_t>(i));
        } else {
            roots.push_back(static_cast<uint32_t>(i));
        }
    }
    // Rows in a parent cycle (possible only through unknown start times) are not reachable from
    // any root: cut one link of each cycle and make that row a root.
    std::vector<uint8_t> reach(n, 0);
    std::vector<uint32_t> st;
    auto mark = [&](uint32_t r) {
        st.push_back(r);
        reach[r] = 1;
        while (!st.empty()) {
            const uint32_t i = st.back();
            st.pop_back();
            for (uint32_t c : kids[i]) {
                if (!reach[c]) {
                    reach[c] = 1;
                    st.push_back(c);
                }
            }
        }
    };
    for (uint32_t r : roots) mark(r);
    for (size_t i = 0; i < n; ++i) {
        if (!in[i] || reach[i]) continue;
        auto& pk = kids[static_cast<size_t>(g.parent[i])];
        pk.erase(std::remove(pk.begin(), pk.end(), static_cast<uint32_t>(i)), pk.end());
        roots.push_back(static_cast<uint32_t>(i));
        mark(static_cast<uint32_t>(i));
    }
    TreeBuilder tb{rows, nullptr, &kids, &self, nullptr};
    std::vector<ProcNode> out;
    for (uint32_t r : roots) {
        out.push_back(tb.make(r));
        if (rows[r].pid == self_pid) out.back().role = Role::Gui;
        sort_children(out.back());
    }
    std::sort(out.begin(), out.end(), [](const ProcNode& a, const ProcNode& b) { return a.key < b.key; });
    return out;
}

}  // namespace kt::resources
