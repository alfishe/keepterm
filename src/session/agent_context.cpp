#include "session/agent_context.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "agents/capture.h"
#include "agents/context_os.h"
#include "agents/os_access.h"
#include "base/log.h"
#include "platform/file_watcher.h"
#include "platform/paths.h"
#include "session/agent_keeper.h"

namespace kt::session {

namespace {

std::uint32_t x100(double v) {
    if (!(v > 0)) return 0;
    return static_cast<std::uint32_t>(std::lround(std::min(v, 1e6) * 100.0));
}

std::optional<std::uint64_t> micro_usd(std::optional<double> v) {
    if (!v || !(*v >= 0)) return std::nullopt;
    return static_cast<std::uint64_t>(std::llround(std::min(*v, 1e9) * 1e6));
}

proto::AgentLimit limit_to_proto(const agents::RateLimit& l) {
    proto::AgentLimit p;
    p.name = l.name;
    p.used_percent_x100 = x100(l.used_percent);
    p.window_minutes = l.window_minutes;
    p.resets_at_unix = l.resets_at_unix > 0 ? static_cast<std::uint64_t>(l.resets_at_unix) : 0;
    return p;
}

agents::RateLimit limit_from_proto(const proto::AgentLimit& p) {
    agents::RateLimit l;
    l.name = p.name;
    l.used_percent = static_cast<double>(p.used_percent_x100) / 100.0;
    l.window_minutes = p.window_minutes;
    l.resets_at_unix = static_cast<std::int64_t>(std::min<std::uint64_t>(p.resets_at_unix, INT64_MAX));
    return l;
}

proto::AgentContext none_msg(const AgentSettings& s, bool hidden) {
    proto::AgentContext m;
    m.status = proto::AgentContextStatus::None;
    m.warn_percent = static_cast<std::uint8_t>(s.warn_percent);
    m.alert_percent = static_cast<std::uint8_t>(s.alert_percent);
    if (hidden) m.flags = proto::kCtxPrivateHidden;
    return m;
}

}  // namespace

proto::AgentStatusline to_proto(const agents::StatusLineReport& r, std::string_view pane) {
    proto::AgentStatusline m;
    m.session_id = r.session_id;
    m.transcript_path = r.transcript_path;
    m.model_id = r.model_id;
    m.model_name = r.model_name;
    m.version = r.version;
    m.pane = std::string(pane);
    m.window_tokens = r.window_tokens;
    m.used_tokens = r.used_tokens;
    m.total_input = r.total_input;
    m.total_output = r.total_output;
    m.cost_micro_usd = micro_usd(r.cost_usd);
    if (r.used_percent) m.used_percent_x100 = x100(*r.used_percent);
    for (const auto& l : r.limits) {
        if (m.limits.size() >= proto::kMaxAgentLimits) break;
        m.limits.push_back(limit_to_proto(l));
    }
    return m;
}

agents::StatusLineReport from_proto(const proto::AgentStatusline& m) {
    agents::StatusLineReport r;
    r.session_id = m.session_id;
    r.transcript_path = m.transcript_path;
    r.model_id = m.model_id;
    r.model_name = m.model_name;
    r.version = m.version;
    r.window_tokens = m.window_tokens;
    r.used_tokens = m.used_tokens;
    r.total_input = m.total_input;
    r.total_output = m.total_output;
    if (m.cost_micro_usd) r.cost_usd = static_cast<double>(*m.cost_micro_usd) / 1e6;
    if (m.used_percent_x100) r.used_percent = static_cast<double>(*m.used_percent_x100) / 100.0;
    for (const auto& l : m.limits) r.limits.push_back(limit_from_proto(l));
    return r;
}

proto::AgentContext to_proto(const agents::ContextUsage& u, const AgentSettings& s) {
    proto::AgentContext m = none_msg(s, false);
    m.agent = u.agent;
    m.session_id = u.session_id;
    m.model = u.model;
    m.agent_version = u.agent_version;
    m.format_version = u.format_version;
    m.used_tokens = u.used_tokens;
    m.window_tokens = u.window_tokens;
    m.used_source = static_cast<std::uint8_t>(u.used_source);
    m.window_source = static_cast<std::uint8_t>(u.window_source);
    m.limits_source = static_cast<std::uint8_t>(u.limits_source);
    if (u.percent) m.percent_x100 = x100(*u.percent);
    m.input_tokens = u.input_tokens;
    m.output_tokens = u.output_tokens;
    m.cache_read_tokens = u.cache_read_tokens;
    m.cache_write_tokens = u.cache_write_tokens;
    m.cost_micro_usd = micro_usd(u.cost_usd);
    m.turns = u.turns;
    m.compactions = u.compactions;
    m.api_calls = u.api_calls;
    if (u.after_compaction) m.flags |= proto::kCtxAfterCompaction;
    if (u.totals_since_attach) m.flags |= proto::kCtxTotalsSinceAttach;
    m.activity = static_cast<std::uint8_t>(u.activity);
    m.session_name = u.session_name;
    m.session_cwd = u.session_cwd;
    m.updated_unix_ms = u.updated_unix_ms > 0 ? static_cast<std::uint64_t>(u.updated_unix_ms) : 0;
    m.limits_unix_ms = u.limits_unix_ms > 0 ? static_cast<std::uint64_t>(u.limits_unix_ms) : 0;
    for (const auto& l : u.limits) {
        if (m.limits.size() >= proto::kMaxAgentLimits) break;
        m.limits.push_back(limit_to_proto(l));
    }
    switch (u.format) {
        case agents::FormatStatus::Ok: m.status = proto::AgentContextStatus::Ok; break;
        case agents::FormatStatus::UnknownVersion: m.status = proto::AgentContextStatus::UnknownFormat; break;
        case agents::FormatStatus::NoData: m.status = proto::AgentContextStatus::NoData; break;
    }
    return m;
}

AgentContextKeeper::AgentContextKeeper(AgentSettings settings, ContextKeeperOptions o)
    : o_(std::move(o)), settings_(std::move(settings)), tracker_(o_.fs ? *o_.fs : *(own_fs_ = std::make_unique<agents::StdFileSystem>()), o_.tracker) {
    fs_ = o_.fs ? o_.fs : own_fs_.get();
    if (!o_.probe) {
        own_probe_ = std::make_unique<agents::OsProcessProbe>();
        proc_ = own_probe_.get();
    } else {
        proc_ = o_.probe;
    }
    msg_ = none_msg(settings_, false);
    enabled_ = settings_.context;
    rebuild();
}

AgentContextKeeper::~AgentContextKeeper() {
    watcher_.reset();   // joins the watcher's thread before the members it calls go
}

void AgentContextKeeper::rebuild() {
    const agents::ContextPaths paths =
        o_.paths ? *o_.paths : agents::default_context_paths(settings_.claude_config_dir, settings_.codex_home);
    registry_ = std::make_unique<agents::AgentRegistry>(agents::AgentRegistry::builtin());
    agents::SourcePaths sp = agents::default_source_paths(settings_.claude_config_dir);
    if (o_.paths) {
        sp.home = paths.home;
        sp.claude_config_dir = paths.claude_config_dir;
    }
    sp.codex_home = paths.codex_home;
    codex_index_ = std::make_unique<agents::CodexSqliteIndex>(paths.codex_sqlite_home);
    agents::attach_builtin_sources(*registry_, *fs_, *proc_, sp, codex_index_.get());
    if (!agy_store_) agy_store_ = std::make_unique<agents::AgySqliteStore>();
    probes_ = agents::builtin_context_probes(*fs_, *proc_, codex_index_.get(), paths, settings_.context_windows, agy_store_.get());
    // The old probe objects are gone: the next tick re-locates.
    probe_ = nullptr;
    (void)tracker_.follow(nullptr, std::nullopt, 0);
}

void AgentContextKeeper::set_settings(const AgentSettings& s) {
    const bool paths_changed = s.claude_config_dir != settings_.claude_config_dir || s.codex_home != settings_.codex_home ||
                               s.context_windows != settings_.context_windows;
    settings_ = s;
    {
        std::lock_guard lk(mu_);
        enabled_ = s.context;
        if (!enabled_) report_.reset();
    }
    if (paths_changed) rebuild();
}

void AgentContextKeeper::set_private(bool on) { private_ = on; }

bool AgentContextKeeper::statusline(const proto::AgentStatusline& m, std::int64_t now_ms) {
    std::lock_guard lk(mu_);
    if (!enabled_) return false;
    agents::StatusLineReport r = from_proto(m);
    r.received_unix_ms = now_ms;
    agents::stamp_limits(r, report_ ? &*report_ : nullptr);
    report_ = std::move(r);
    return true;
}

proto::AgentContext AgentContextKeeper::current() const {
    std::lock_guard lk(mu_);
    return msg_;
}

std::string AgentContextKeeper::agent() const {
    std::lock_guard lk(mu_);
    return agent_copy_;
}

void AgentContextKeeper::watch(const std::string& dir) {
    if (!o_.watch || dir == watched_dir_) return;
    if (!watcher_) {
        auto w = platform::FileWatcher::create([this](std::span<const platform::FsEvent>) {
            // Any change in the folder: the tracker's stat decides whether the file grew.
            event_ = true;
            if (on_wake) on_wake();
        });
        if (!w) {
            log::warn("agents", "context: no file watcher (" + w.error().what + "); polling every 10 s");
            o_.watch = false;
            return;
        }
        watcher_ = std::move(*w);
    }
    if (!watched_dir_.empty()) watcher_->remove_dir(platform::paths::from_utf8(watched_dir_));
    watched_dir_.clear();
    if (dir.empty()) return;
    if (auto r = watcher_->add_dir(platform::paths::from_utf8(dir)); r) watched_dir_ = dir;
}

bool AgentContextKeeper::publish(proto::AgentContext next) {
    std::lock_guard lk(mu_);
    next.seq = msg_.seq;
    if (next == msg_) return false;
    next.seq = msg_.seq + 1;
    msg_ = std::move(next);
    agent_copy_ = agent_;
    return true;
}

bool AgentContextKeeper::tick(const proto::ProcessInfo* pi, std::int64_t now_ms) {
    bool enabled = false;
    std::optional<agents::StatusLineReport> report;
    {
        std::lock_guard lk(mu_);
        enabled = enabled_;
        report = report_;
    }
    const bool hidden = private_ && !settings_.context_in_private;
    if (!enabled || hidden) {
        if (tracker_.target()) (void)tracker_.follow(nullptr, std::nullopt, now_ms);
        probe_ = nullptr;
        agent_.clear();
        watch({});
        return publish(none_msg(settings_, hidden && enabled));
    }

    if (pi && pi->foreground_pid != 0) {
        const auto facts = AgentKeeper::facts_of(*pi);
        const auto match = agents::recognize_chain(facts, *registry_);
        std::string id = match && match->agent ? std::string(match->agent->id) : std::string();
        const agents::IContextProbe* probe = id.empty() ? nullptr : probes_.find(id);
        agent_ = probe ? id : std::string();
        probe_ = probe;
        if (probe) {
            const auto& f = facts[match->index];
            agents::ContextQuery q;
            q.process = {f.pid, f.start_time, f.cwd, match->argv};
            q.argv = match->argv;
            q.pane_cwd = pi->cwd;
            if (const agents::ISessionSource* src = registry_->source(id)) q.session = src->session_of(q.process);
            if (report && id == "claude") {
                q.reported_session = report->session_id;
                q.reported_transcript = report->transcript_path;
            }
            query_ = std::move(q);
            const auto target = probe->locate(query_);
            (void)tracker_.follow(probe, target, now_ms);
            watch(target ? target->watch_dir : std::string());
        } else {
            (void)tracker_.follow(nullptr, std::nullopt, now_ms);
            watch({});
        }
    } else if (pi) {
        // The foreground is not known (a terminal that is not live): nothing runs.
        agent_.clear();
        probe_ = nullptr;
        (void)tracker_.follow(nullptr, std::nullopt, now_ms);
        watch({});
    }

    if (!probe_) return publish(none_msg(settings_, false));
    (void)tracker_.poll(now_ms, event_.exchange(false));
    activity_ = probe_->activity(query_);   // Claude: the pid file's status, small and current
    const agents::IContextProbe::SessionLabel label = probe_->session_label(query_);

    agents::ContextUsage u;
    if (const agents::ContextUsage* t = tracker_.usage()) u = *t;
    if (u.agent.empty()) u.agent = agent_;
    if (u.session_id.empty() && tracker_.target()) u.session_id = tracker_.target()->session_id;
    if (activity_ != agents::Activity::Unknown) u.activity = activity_;
    u.session_name = label.name;
    u.session_cwd = label.cwd;
    if (report && agent_ == "claude") agents::merge_statusline(u, *report);
    return publish(to_proto(u, settings_));
}

}  // namespace kt::session
