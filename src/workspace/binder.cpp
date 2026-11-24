#include "workspace/binder.h"

#include <algorithm>

#include "base/log.h"

namespace kt::ws {

std::string_view to_string(ProbeResult r) {
    switch (r) {
        case ProbeResult::Live: return "live";
        case ProbeResult::NotFound: return "not_found";
        case ProbeResult::Unreachable: return "unreachable";
        case ProbeResult::VersionTooNew: return "version_too_new";
    }
    return "not_found";
}

SessionBinder::SessionBinder(Workspace& ws, ISessionManager& sm, IRandom& rng, BinderOptions opts)
    : ws_(ws), sm_(sm), rng_(rng), opts_(std::move(opts)) {
    if (opts_.max_in_flight == 0) opts_.max_in_flight = 1;
}

SessionBinder::~SessionBinder() { *alive_ = false; }

void SessionBinder::log(const std::string& line) const {
    if (opts_.log) opts_.log(line);
    else kt::log::info("workspace", line);
}

void SessionBinder::start() {
    if (started_) return;
    started_ = true;
    sub_ = ws_.subscribe([this](std::span<const Change> c) { on_changes(c); });
    for (PaneId p : ws_.panes_in_attach_order()) {
        const Pane* pane = ws_.pane(p);
        if (pane && pane->bind == BindState::Unbound) enqueue(p, pane->never_started ? Job::Spawn : Job::Attach);
    }
    pump();
}

void SessionBinder::on_changes(std::span<const Change> changes) {
    for (const Change& c : changes) {
        switch (c.kind) {
            case ChangeKind::Reset: {
                queue_.clear();
                for (PaneId p : ws_.panes_in_attach_order()) {
                    const Pane* pane = ws_.pane(p);
                    if (pane && pane->bind == BindState::Unbound && !busy_.count(p))
                        enqueue(p, pane->never_started ? Job::Spawn : Job::Attach);
                }
                break;
            }
            case ChangeKind::PaneAdded:
                if (const Pane* p = ws_.pane(c.pane); p && p->bind == BindState::Unbound)
                    enqueue(c.pane, p->never_started ? Job::Spawn : Job::Attach);
                break;
            case ChangeKind::PaneRemoved: {
                queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Item& i) { return i.pane == c.pane; }),
                             queue_.end());
                sizes_.erase(c.pane);
                agent_after_restore_.erase(c.pane);
                restarting_.erase(c.pane);
                if (!(c.fields & (field::kExited | field::kDetached)) && c.session) {
                    const session::SessionId sid = *c.session;
                    sm_.kill(sid, [this, sid, alive = std::weak_ptr<bool>(alive_)](bool ok) {
                        if (alive.expired() || !*alive.lock()) return;
                        if (!ok) log("could not end session " + sid.str());
                    });
                }
                break;
            }
            case ChangeKind::PaneChanged:
                if (c.fields & field::kPrivate) {
                    const Pane* p = ws_.pane(c.pane);
                    if (p && opts_.send_option) {
                        bool on = !p->is_private;
                        if (on && opts_.history_enabled) {
                            auto loc = ws_.locate(c.pane);
                            on = opts_.history_enabled(loc ? ws_.project(loc->project) : nullptr);
                        }
                        opts_.send_option(c.pane, "history.enabled", on ? "true" : "false");
                    }
                }
                break;
            default: break;
        }
    }
    pump();
}

void SessionBinder::enqueue(PaneId pane, Job job, bool front) {
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Item& i) { return i.pane == pane; });
    if (it != queue_.end()) {
        if (job == Job::Restart || job == Job::Respawn) it->job = job;
        return;
    }
    if (busy_.count(pane)) {
        // A job is in flight for the pane; a restart waits for it.
        if (job != Job::Restart) return;
    }
    if (front) queue_.insert(queue_.begin(), Item{pane, job});
    else queue_.push_back(Item{pane, job});
}

void SessionBinder::pump() {
    if (pumping_ || !started_ || stopped_) return;
    pumping_ = true;
    while (in_flight_ < opts_.max_in_flight) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Item& i) { return !busy_.count(i.pane); });
        if (it == queue_.end()) break;
        const Item item = *it;
        queue_.erase(it);
        if (!ws_.pane(item.pane)) continue;
        busy_.insert(item.pane);
        ++in_flight_;
        run(item);
    }
    pumping_ = false;
}

void SessionBinder::done_one(PaneId pane) {
    retrying_.erase(pane);
    if (busy_.erase(pane)) --in_flight_;
    pump();
}

void SessionBinder::fail(PaneId pane, const std::string& why, bool restoring) {
    ws_.binder_set_bind(pane, BindState::Failed, {}, why);
    if (restoring) ws_.binder_set_restore(pane, RestoreStep::Failed);
}

void SessionBinder::run(const Item& item) {
    const PaneId pane = item.pane;
    const Pane* p = ws_.pane(pane);
    switch (item.job) {
        case Job::Spawn: do_spawn(pane, std::nullopt, false, false); return;
        case Job::Fresh: do_spawn(pane, std::nullopt, true, false); return;
        case Job::Respawn: do_spawn(pane, p->session, true, p->agent_resume.has_value()); return;
        case Job::Restart: {
            restarting_.insert(pane);
            const session::SessionId old = p->session;
            ws_.binder_set_bind(pane, BindState::Spawning);
            sm_.end_keep_history(old, [this, pane, old, alive = std::weak_ptr<bool>(alive_)](bool ok) {
                if (alive.expired() || !*alive.lock()) return;
                if (!ok) log("restart: session " + old.str() + " did not end; starting a new one anyway");
                const Pane* cur = ws_.pane(pane);
                if (!cur || cur->session != old) {
                    restarting_.erase(pane);
                    done_one(pane);
                    return;
                }
                do_spawn(pane, old, true, false);   // history stays; no agent resume (rule 5)
            });
            return;
        }
        case Job::Attach: {
            const session::SessionId sid = p->session;
            ws_.binder_set_bind(pane, BindState::Probing);
            sm_.probe(sid, [this, pane, sid, alive = std::weak_ptr<bool>(alive_)](ProbeResult r, std::string endpoint) {
                if (alive.expired() || !*alive.lock()) return;
                const Pane* cur = ws_.pane(pane);
                if (!cur || cur->session != sid) {
                    done_one(pane);
                    return;
                }
                switch (r) {
                    case ProbeResult::Live:
                        ws_.binder_set_restore(pane, RestoreStep::None);
                        ws_.binder_set_bind(pane, BindState::Ready, std::move(endpoint));
                        done_one(pane);
                        return;
                    case ProbeResult::NotFound:
                        if (!opts_.restart_dead_sessions && !retrying_.count(pane)) {
                            fail(pane, "The session ended while keepterm was not running.", false);
                            done_one(pane);
                            return;
                        }
                        do_spawn(pane, sid, true, cur->agent_resume.has_value());
                        return;
                    case ProbeResult::Unreachable:
                        log("session " + sid.str() + " does not answer; a new session replaces it (the old host stays)");
                        do_spawn(pane, std::nullopt, true, false);
                        return;
                    case ProbeResult::VersionTooNew:
                        fail(pane, "A newer keepterm runs this session. Quit it or update this keepterm.", false);
                        done_one(pane);
                        return;
                }
                done_one(pane);
            });
            return;
        }
    }
}

std::string SessionBinder::resolve_cwd(const Pane& pane) const {
    if (!pane.last_cwd.empty()) return pane.last_cwd;
    if (auto loc = ws_.locate(pane.id))
        if (const Project* pr = ws_.project(loc->project); pr && !pr->path.empty()) return pr->path;
    return ws_.options().home_dir;
}

SpawnSpec SessionBinder::spawn_spec(const Pane& pane, std::optional<session::SessionId> restore_from,
                                    const session::SessionId& id) const {
    SpawnSpec s;
    s.id = id;
    s.cwd = resolve_cwd(pane);
    const Project* project = nullptr;
    if (auto loc = ws_.locate(pane.id)) project = ws_.project(loc->project);
    const std::optional<ShellSpec>& shell = pane.shell ? pane.shell : (project ? project->shell : pane.shell);
    if (shell) {
        s.argv.push_back(shell->program);
        s.argv.insert(s.argv.end(), shell->args.begin(), shell->args.end());
    }
    s.env.emplace_back("KEEPTERM_PANE", pane.id.to_string());
    s.env.emplace_back("KEEPTERM_SESSION", id.str());
    if (!opts_.control_endpoint.empty()) s.env.emplace_back("KEEPTERM_CONTROL", opts_.control_endpoint);
    if (!opts_.profile.empty()) s.env.emplace_back("KEEPTERM_PROFILE", opts_.profile);
    auto sz = sizes_.find(pane.id);
    s.size = sz != sizes_.end() ? sz->second : opts_.default_size;
    s.restore_from = std::move(restore_from);
    s.history = !pane.is_private && (!opts_.history_enabled || opts_.history_enabled(project));
    return s;
}

void SessionBinder::do_spawn(PaneId pane, std::optional<session::SessionId> restore_from, bool respawn,
                             bool agent_flag) {
    const Pane* p = ws_.pane(pane);
    if (!p) {
        done_one(pane);
        return;
    }
    const session::SessionId old = p->session;
    const session::SessionId id = respawn ? new_session_id(rng_) : old;
    const bool restoring = restore_from.has_value();
    SpawnSpec spec = spawn_spec(*p, restore_from, id);
    if (respawn) {
        // the source of the cwd goes into the log so a guess is never mistaken for a fact.
        log("respawn pane " + pane.to_string() + ": session " + old.str() + " -> " + id.str() +
            (restoring ? " (restore_from " + restore_from->str() + ")" : " (fresh)") + " in " + spec.cwd +
            " [cwd_source " + std::string(to_string(p->last_cwd_source)) + "]");
    }
    ws_.binder_set_bind(pane, BindState::Spawning);
    if (restoring) ws_.binder_set_restore(pane, RestoreStep::Respawning);
    sm_.spawn(spec, [this, pane, old, id, restoring, agent_flag,
                     alive = std::weak_ptr<bool>(alive_)](WsResult<std::string> endpoint) {
        if (alive.expired() || !*alive.lock()) return;
        restarting_.erase(pane);
        const Pane* cur = ws_.pane(pane);
        if (!cur || cur->session != old) {
            // The pane closed (or changed hands) while the host started: end the new host.
            if (endpoint) sm_.kill(id, [](bool) {});
            done_one(pane);
            return;
        }
        if (!endpoint) {
            // The pane keeps its old id and record; the next launch tries again.
            log("spawn for pane " + pane.to_string() + " failed: " + endpoint.error().message +
                (endpoint.error().detail.empty() ? "" : " (" + endpoint.error().detail + ")"));
            fail(pane, endpoint.error().message, restoring);
            done_one(pane);
            return;
        }
        if (id != old) ws_.binder_set_session(pane, id);   // saved at once
        if (restoring && agent_flag) agent_after_restore_.insert(pane);
        if (!restoring) ws_.binder_set_restore(pane, RestoreStep::None);
        ws_.binder_set_bind(pane, BindState::Ready, std::move(*endpoint));
        done_one(pane);
    });
}

void SessionBinder::on_first_snapshot(PaneId pane) {
    const Pane* p = ws_.pane(pane);
    if (!p) return;
    if (p->restore == RestoreStep::Respawning) {
        Transaction tx = ws_.begin();
        ws_.binder_set_restore(pane, RestoreStep::HistoryRestored);
        if (agent_after_restore_.erase(pane) && p->agent_resume) ws_.binder_set_agent_pending(pane, true);
    }
}

void SessionBinder::on_exit(PaneId pane, int code) {
    // A restart ends the old shell on purpose; the pane stays for the new one.
    if (stopped_ || !ws_.pane(pane) || restarting_.count(pane)) return;
    log("pane " + pane.to_string() + ": shell exited with " + std::to_string(code));
    Transaction tx = ws_.begin();
    ws_.binder_set_bind(pane, BindState::Ended);
    (void)ws_.pane_exited(pane);
}

void SessionBinder::on_host_lost(PaneId pane) {
    const Pane* p = ws_.pane(pane);
    if (stopped_ || !p || restarting_.count(pane) || busy_.count(pane)) return;
    if (p->bind != BindState::Ready) return;
    ws_.binder_set_bind(pane, BindState::Unbound);
    // Probe first: the link may have died while the host lives (attach again), else respawn.
    enqueue(pane, Job::Attach, true);
    pump();
}

void SessionBinder::set_size_hint(PaneId pane, GridSize size) {
    if (size.cols == 0 || size.rows == 0) return;
    sizes_[pane] = size;
}

void SessionBinder::retry(PaneId pane) {
    const Pane* p = ws_.pane(pane);
    if (!p || p->bind != BindState::Failed) return;
    retrying_.insert(pane);   // the user asked: a dead session respawns even when restarts are off
    enqueue(pane, Job::Attach, true);
    pump();
}

void SessionBinder::restart(PaneId pane) {
    if (!ws_.pane(pane)) return;
    enqueue(pane, Job::Restart, true);
    pump();
}

}  // namespace kt::ws
