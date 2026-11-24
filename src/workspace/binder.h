// The pane -> session binding. Core code.
//
// For every pane the binder makes sure a session host exists and tells the view where it is:
//   - a saved session whose host is alive: Ready with its endpoint (the view attaches);
//   - a saved session whose host is gone (NotFound): a NEW id, spawn with restore_from = old id
//     in the pane's last cwd, swap the id (saved at once), Respawning -> HistoryRestored when the
//     view reports the first snapshot, then agent_resume_pending for panes with a record;
//   - a hung host (Unreachable): a new id without restore_from; the old host stays an orphan;
//   - a new pane: spawn with the id the model generated.
// At most 8 probes or spawns run at once, in Workspace::panes_in_attach_order().
//
// The view owns the session links. It attaches when a pane reaches BindState::Ready and reports
// back with on_first_snapshot(), on_exit() and on_host_lost().
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "workspace/workspace.h"

namespace kt::ws {

// What the session registry answers for a saved session id.
enum class ProbeResult : std::uint8_t { Live, NotFound, Unreachable, VersionTooNew };
std::string_view to_string(ProbeResult);

struct GridSize {
    std::uint16_t cols = 80, rows = 24;
};

// SpawnSpec, the fields the workspace decides.
struct SpawnSpec {
    session::SessionId id;
    std::string cwd;                                  // the pane's last cwd (respawn) or start folder
    std::vector<std::string> argv;                    // empty = the user's shell (config)
    std::vector<std::pair<std::string, std::string>> env;   // KEEPTERM_PANE, KEEPTERM_SESSION, ...
    GridSize size;
    std::optional<session::SessionId> restore_from;
    bool history = true;                              // session.history && !pane.is_private
};

// The GUI-side session facade (the workspace's view of the session manager). Every callback
// runs on the UI thread, never inside the call that started it... except in test fakes that say
// otherwise; the binder copes with both.
class ISessionManager {
public:
    virtual ~ISessionManager() = default;
    // Is the host of `id` there? Live comes with its endpoint.
    virtual void probe(const session::SessionId& id,
                       std::function<void(ProbeResult, std::string endpoint)> done) = 0;
    // Starts a host with spec.id; done gets the endpoint once it accepts clients.
    virtual void spawn(const SpawnSpec& spec, std::function<void(WsResult<std::string> endpoint)> done) = 0;
    // Ends the host (the shell and its children) and waits for it to exit (2 s, then hard kill).
    virtual void kill(const session::SessionId& id, std::function<void(bool ok)> done) = 0;
    // Like kill, but the host keeps its history folder (pane.restart_session: the new host
    // restores from it). Default: kill.
    virtual void end_keep_history(const session::SessionId& id, std::function<void(bool ok)> done) { kill(id, std::move(done)); }
};

struct BinderOptions {
    std::size_t max_in_flight = 8;
    bool restart_dead_sessions = true;     // sessions.restart_dead_sessions; false: Failed instead
    std::string control_endpoint;          // KEEPTERM_CONTROL hint, empty = not set
    std::string profile;                   // KEEPTERM_PROFILE when a profile is active
    GridSize default_size;
    // session.history for a project (global or its [[projects]] override). Unset: true.
    std::function<bool(const Project* project)> history_enabled;
    // The view forwards a host option over its attached link (SET_OPTION), e.g.
    // "history.enabled" = "false" when a pane turns private.
    std::function<void(PaneId, const std::string& key, const std::string& value)> send_option;
    // Log lines (category "workspace"); unset = base log.
    std::function<void(const std::string&)> log;
};

class SessionBinder {
public:
    SessionBinder(Workspace& ws, ISessionManager& sm, IRandom& rng, BinderOptions opts = {});
    ~SessionBinder();
    SessionBinder(const SessionBinder&) = delete;
    SessionBinder& operator=(const SessionBinder&) = delete;

    // Binds every pane of the loaded model in attach order, then follows the model: new panes
    // are spawned, removed panes are killed. Call once after Workspace::load.
    void start();

    // The view's reports for a pane's attached link.
    void on_first_snapshot(PaneId);      // Respawning -> HistoryRestored (+ agent flag)
    void on_exit(PaneId, int code);      // the shell ended: the pane closes
    void on_host_lost(PaneId);           // the link died without EXIT_STATUS: respawn
    void set_size_hint(PaneId, GridSize);   // cells of the pane's view, for the next spawn

    // Retry button of a Failed pane.
    void retry(PaneId);
    // pane.restart_session: kill, then a new id with restore_from = old id; no agent resume.
    void restart(PaneId);

    // The spawn spec the binder would use (tests, pane inspect).
    SpawnSpec spawn_spec(const Pane& pane, std::optional<session::SessionId> restore_from,
                         const session::SessionId& id) const;
    void set_restart_dead_sessions(bool on) { opts_.restart_dead_sessions = on; }
    std::size_t in_flight() const { return in_flight_; }
    std::size_t queued() const { return queue_.size(); }
    bool started() const { return started_; }
    // keepterm exit / restart: the hosts are being ended on purpose. Nothing respawns, attaches
    // or closes from now on (the panes stay as saved for the next start).
    void stop() {
        stopped_ = true;
        queue_.clear();
    }
    bool stopped() const { return stopped_; }

private:
    enum class Job : std::uint8_t { Attach, Spawn, Respawn, Fresh, Restart };
    struct Item {
        PaneId pane;
        Job job;
    };
    void on_changes(std::span<const Change> changes);
    void enqueue(PaneId pane, Job job, bool front = false);
    void pump();
    void run(const Item& item);
    void do_spawn(PaneId pane, std::optional<session::SessionId> restore_from, bool respawn, bool agent_flag);
    void done_one(PaneId pane);
    void fail(PaneId pane, const std::string& why, bool restoring);
    void log(const std::string& line) const;
    std::string resolve_cwd(const Pane& pane) const;

    Workspace& ws_;
    ISessionManager& sm_;
    IRandom& rng_;
    BinderOptions opts_;
    Subscription sub_;
    bool started_ = false;
    bool stopped_ = false;
    std::vector<Item> queue_;
    std::set<PaneId> busy_;            // panes with a job queued or in flight
    std::size_t in_flight_ = 0;
    std::map<PaneId, GridSize> sizes_;
    std::set<PaneId> agent_after_restore_;   // respawned panes waiting for their first snapshot
    std::set<PaneId> restarting_;            // restart() running: a lost link is expected
    std::set<PaneId> retrying_;              // retry(): respawn even with restart_dead_sessions off
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool pumping_ = false;
};

}  // namespace kt::ws
