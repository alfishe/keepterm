// The workspace wired to the real system (kt_workspace_os): the state folder on disk, real
// session hosts, the import of the old per-window files. The UI creates one WorkspaceRuntime at launch.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "workspace/binder.h"
#include "workspace/persist.h"
#include "workspace/state.h"
#include "workspace/usage.h"
#include "workspace/workspace.h"

namespace kt::config {
class Config;
}

namespace kt::ws {

// The real file system (UTF-8 paths; atomic write = temp + flush + rename + folder fsync).
std::unique_ptr<IFileSystem> make_os_file_system();

struct HostManagerOptions {
    std::string exe;                       // keepterm executable; empty = this process's image
    std::vector<std::string> host_args;    // extra host options (--config, --set, --pty=, tests)
    int probe_timeout_ms = 300;
    int spawn_timeout_ms = 10'000;
};

// ISessionManager over the session registry and `keepterm --session-host`. Blocking
// work runs on worker threads; every callback is delivered through `post` on the UI thread.
// Destroying the manager waits for running workers; their callbacks are dropped.
std::unique_ptr<ISessionManager> make_host_session_manager(Poster post, HostManagerOptions opts = {});

// The old per-window files (<state>/windows/*.json) as legacy panes, dropping those the old start
// logic would drop (shell ended, nothing left). Empty when there are none.
std::vector<LegacyPane> read_legacy_panes();
// After the first successful save of an imported state: <state>/windows -> windows.imported.
void retire_legacy_panes();

struct RuntimeOptions {
    Poster post;                           // the UI loop's post; required
    const config::Config* config = nullptr;   // defaults for the binder and the model; may be null
    std::string state_dir;                 // empty = session::os::paths().state (honors KEEPTERM_STATE_DIR)
    std::string home_dir;                  // empty = the user's home
    WriterInfo writer;
    HostManagerOptions hosts;
    std::string control_endpoint;          // KEEPTERM_CONTROL for new shells; empty = unset
    std::unique_ptr<ISessionManager> session_manager;   // override (tests); default: host manager
    bool import_legacy = true;
};

// Owns the model, the save scheduler, the binder and the palette usage store.
class WorkspaceRuntime {
public:
    // Loads the state (never fails: problems become LoadOutcome notices). Does not bind yet.
    static std::unique_ptr<WorkspaceRuntime> create(RuntimeOptions opts);
    ~WorkspaceRuntime();

    Workspace& model() { return *ws_; }
    SessionBinder& binder() { return *binder_; }
    PersistScheduler& persistence() { return *persist_; }
    UsageStore& usage() { return *usage_; }
    StateStore& store() { return *store_; }
    const LoadOutcome& load_outcome() const { return outcome_; }

    // Starts binding every pane (after the first window exists, so visible panes go first).
    void start();
    // Quit: flushes the state and palette usage synchronously.
    void flush();
    // Periodic tick from the UI (once a second is enough): palette usage save.
    void tick();

    // Applies a changed config (splits.auto_tile, session.history, ...).
    void apply_config(const config::Config& cfg);

private:
    WorkspaceRuntime() = default;
    RuntimeOptions opts_;
    std::unique_ptr<IFileSystem> fs_;
    std::unique_ptr<Workspace> ws_;
    std::unique_ptr<StateStore> store_;
    std::unique_ptr<PersistScheduler> persist_;
    std::unique_ptr<ISessionManager> sm_;
    std::unique_ptr<SessionBinder> binder_;
    std::unique_ptr<UsageStore> usage_;
    std::shared_ptr<const config::Config> config_;
    LoadOutcome outcome_;
    bool legacy_imported_ = false;
    Subscription legacy_sub_;
};

}  // namespace kt::ws
