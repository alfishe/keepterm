#include "resources/collector.h"

#include <algorithm>

namespace kt::resources {
namespace {

bool moved(uint64_t a, uint64_t b) {
    const uint64_t d = a > b ? a - b : b - a;
    constexpr uint64_t kMiB = 1024 * 1024;
    return d >= kMiB && static_cast<double>(d) >= 0.005 * static_cast<double>(std::max(a, b));
}

}  // namespace

bool snapshot_changed(const ResourceSnapshot& prev, const ResourceSnapshot& next) {
    if (prev.machines.size() != next.machines.size()) return true;
    for (size_t m = 0; m < prev.machines.size(); ++m) {
        const MachineSample& a = prev.machines[m];
        const MachineSample& b = next.machines[m];
        if (a.sessions.size() != b.sessions.size()) return true;
        if (moved(a.total.main, b.total.main) || moved(a.terminal.gui.main, b.terminal.gui.main)) return true;
        if (a.forest.size() != b.forest.size()) return true;
        for (size_t s = 0; s < a.sessions.size(); ++s) {
            const SessionSample& x = a.sessions[s];
            const SessionSample& y = b.sessions[s];
            if (x.id != y.id || x.total.processes != y.total.processes || x.accuracy != y.accuracy ||
                moved(x.total.main, y.total.main) || moved(x.overhead.main, y.overhead.main))
                return true;
        }
    }
    return false;
}

Collector::Collector(CollectorConfig cfg, std::unique_ptr<ProcessSource> source)
    : cfg_(std::move(cfg)), source_(std::move(source)), self_pid_(current_pid()) {
    if (!source_) source_ = ProcessSource::create(cfg_.source);
}

Collector::~Collector() { stop(); }

Result<std::shared_ptr<const ResourceSnapshot>> Collector::tick() {
    std::lock_guard tick_lock(tick_mu_);
    const auto wall0 = std::chrono::steady_clock::now();
    const uint64_t cpu0 = current_thread_cpu_ns();

    std::vector<SessionProcessInfo> sessions;
    std::chrono::milliseconds interval;
    {
        std::lock_guard lk(mu_);
        sessions = sessions_;
        interval = cfg_.interval;
    }

    auto rows = source_->take();
    if (!rows) return Unexpected(rows.error());
    std::span<RawProc> all = *rows;

    std::vector<std::optional<ContainmentReading>> readings(sessions.size());
    for (size_t s = 0; s < sessions.size(); ++s) {
        const ContainmentKind k = sessions[s].containment.kind;
        if (k != ContainmentKind::Job && k != ContainmentKind::Cgroup && k != ContainmentKind::CgroupNoMemory)
            continue;
        if (auto r = source_->read_containment(sessions[s].containment)) readings[s] = std::move(*r);
    }

    ProcGraph graph = ProcGraph::build(all);
    if (source_->last_take_partial()) {
        bool unknown = false;
        auto check = [&](uint32_t pid) {
            unknown = unknown || (!graph.by_pid.count(pid) && !missing_after_full_.count(pid));
        };
        for (size_t s = 0; s < sessions.size() && !unknown; ++s) {
            if (readings[s]) {
                for (uint32_t pid : readings[s]->pids) check(pid);
            }
            if (sessions[s].host) check(sessions[s].host->pid);
            check(sessions[s].root.pid);
            for (const auto& k : sessions[s].tracked) check(k.pid);
        }
        if (unknown) {
            source_->force_full_next();
            rows = source_->take();
            if (!rows) return Unexpected(rows.error());
            all = *rows;
            graph = ProcGraph::build(all);
        }
    }
    if (!source_->last_take_partial()) {
        // PIDs named by sessions but absent from a full snapshot (exited roots) do not force
        // another full snapshot until they show up.
        missing_after_full_.clear();
        auto note = [&](uint32_t pid) {
            if (!graph.by_pid.count(pid)) missing_after_full_.insert(pid);
        };
        for (size_t s = 0; s < sessions.size(); ++s) {
            if (readings[s]) {
                for (uint32_t pid : readings[s]->pids) note(pid);
            }
            if (sessions[s].host) note(sessions[s].host->pid);
            note(sessions[s].root.pid);
            for (const auto& k : sessions[s].tracked) note(k.pid);
        }
    }
    AttributionInput in{all, sessions, readings, self_pid_};
    const std::vector<Assignment> asg = attributor_.assign(in, graph);

    std::vector<RawProc*> candidates;
    candidates.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        if (asg[i].session >= 0 || all[i].pid == self_pid_ || (cfg_.forest && all[i].own_user == true))
            candidates.push_back(&all[i]);
    }
    source_->measure(candidates, MeasureOptions{cfg_.detail});
    source_->refine(candidates, cfg_.refine_budget);

    auto snap = std::make_shared<ResourceSnapshot>();
    snap->taken_at = std::chrono::system_clock::now();
    snap->interval = interval;
    snap->machines.push_back(attributor_.build(in, graph, asg, source_->os(), std::chrono::steady_clock::now(),
                                               BuildOptions{cfg_.exclude_page_cache, cfg_.forest}));

    const uint64_t cpu = current_thread_cpu_ns() - cpu0;
    const auto wall = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wall0).count());

    std::function<void(uint64_t, bool)> cb;
    {
        std::lock_guard lk(mu_);
        snap->tick = ++tick_no_;
        snap->changed = !latest_ || snapshot_changed(*latest_, *snap);
        latest_ = snap;
        ++stats_.ticks;
        if (snap->changed) ++stats_.changed_ticks;
        stats_.last_cpu_ns = cpu;
        stats_.last_wall_ns = wall;
        stats_.max_cpu_ns = std::max(stats_.max_cpu_ns, cpu);
        stats_.total_cpu_ns += cpu;
        stats_.last_measured = static_cast<uint32_t>(candidates.size());
        cb = on_published_;
    }
    if (cb) cb(snap->tick, snap->changed);
    return std::shared_ptr<const ResourceSnapshot>(std::move(snap));
}

void Collector::start() {
    std::lock_guard lk(mu_);
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Collector::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Collector::run() {
    lower_current_thread_priority();
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        {
            std::unique_lock lk(mu_);
            if (cv_.wait_until(lk, next, [this] { return stop_; })) return;
        }
        (void)tick();  // an error leaves the previous snapshot in place; the next tick retries
        std::chrono::milliseconds interval;
        {
            std::lock_guard lk(mu_);
            interval = cfg_.interval;
        }
        next += interval;
        const auto now = clock::now();
        // A tick that overran skips the ticks that are already due.
        while (next <= now) {
            next += interval;
            std::lock_guard lk(mu_);
            ++stats_.late_ticks;
        }
    }
}

void Collector::set_sessions(std::vector<SessionProcessInfo> s) {
    std::lock_guard lk(mu_);
    sessions_ = std::move(s);
}

void Collector::set_interval(std::chrono::milliseconds i) {
    {
        std::lock_guard lk(mu_);
        cfg_.interval = std::clamp(i, std::chrono::milliseconds(10), std::chrono::milliseconds(60000));
    }
    cv_.notify_all();
}

std::shared_ptr<const ResourceSnapshot> Collector::latest() const {
    std::lock_guard lk(mu_);
    return latest_;
}

void Collector::on_published(std::function<void(uint64_t, bool)> cb) {
    std::lock_guard lk(mu_);
    on_published_ = std::move(cb);
}

CollectorStats Collector::stats() const {
    std::lock_guard lk(mu_);
    return stats_;
}

}  // namespace kt::resources
