// Agent context telemetry in the session host.
//
// The host's agents thread (Host::agents_loop, next to the AgentKeeper's capture) owns one
// AgentContextKeeper per pane:
//   - on a new foreground chain (a change, or every 10 s) it recognizes the agent, asks its probe
//     (agents/context.h) for the file and follows that file with a ContextTracker: a tail of
//     256 KiB..8 MiB on attach, then the appended bytes from the saved offset;
//   - a FileWatcher on the file's folder wakes the thread; reads are at most once a second, and a
//     stat every 10 s catches events the watcher missed;
//   - the Claude statusLine relay (AGENT_STATUSLINE, any client thread) is merged over the
//     transcript's figures (Source::StatusLine wins);
//   - current() is the AGENT_CONTEXT message (seq increases with each new value).
// Private panes (SET_OPTION pane.private=1 or the host's --private) produce no values unless
// agents.context.show_in_private_panes is on; nothing of a transcript is kept or logged beyond
// the numbers and ids of ContextUsage.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "agents/context.h"
#include "agents/context_statusline.h"
#include "agents/context_tracker.h"
#include "agents/registry.h"
#include "session/pane_config.h"
#include "session/protocol.h"

namespace kt::platform {
class FileWatcher;
}

namespace kt::session {

struct ContextKeeperOptions {
    agents::TrackerOptions tracker;
    bool watch = true;   // false: no FileWatcher (tests drive poll() with events themselves)
    // nullptr = the real ones (agents/os_access.h); must outlive the keeper.
    const agents::IFileSystem* fs = nullptr;
    const agents::IProcessProbe* probe = nullptr;
    // Paths of the agents' folders; nullopt = agents::default_context_paths(settings).
    std::optional<agents::ContextPaths> paths;
};

class AgentContextKeeper {
public:
    AgentContextKeeper(AgentSettings settings, ContextKeeperOptions o = {});
    ~AgentContextKeeper();
    AgentContextKeeper(const AgentContextKeeper&) = delete;
    AgentContextKeeper& operator=(const AgentContextKeeper&) = delete;

    // Called from the watcher's thread when the followed file's folder changed.
    std::function<void()> on_wake;

    // The agents thread.
    void set_settings(const AgentSettings&);
    // `pi`: a fresh foreground chain (re-locates the file), nullptr: only follow the file.
    // Returns true when current() changed.
    bool tick(const proto::ProcessInfo* pi, std::int64_t now_ms);

    // Any thread.
    void set_private(bool on);
    bool is_private() const { return private_.load(); }
    // A relay report; applied at the next tick(). False when it was dropped (telemetry off).
    bool statusline(const proto::AgentStatusline& m, std::int64_t now_ms);
    proto::AgentContext current() const;
    // The agent recognized in the last chain ("" = none).
    std::string agent() const;

private:
    void rebuild();
    bool publish(proto::AgentContext next);
    void watch(const std::string& dir);

    ContextKeeperOptions o_;
    AgentSettings settings_;
    std::unique_ptr<agents::IFileSystem> own_fs_;
    std::unique_ptr<agents::IProcessProbe> own_probe_;
    const agents::IFileSystem* fs_ = nullptr;
    const agents::IProcessProbe* proc_ = nullptr;
    std::unique_ptr<agents::ICodexIndex> codex_index_;
    std::unique_ptr<agents::IAgyStore> agy_store_;   // stateless; made once, outlives probes and reducers
    std::unique_ptr<agents::AgentRegistry> registry_;
    agents::ContextProbes probes_;
    agents::ContextTracker tracker_;
    std::unique_ptr<platform::FileWatcher> watcher_;
    std::string watched_dir_;
    std::atomic<bool> event_{false};
    std::atomic<bool> private_{false};

    // Agents thread only.
    std::string agent_;
    const agents::IContextProbe* probe_ = nullptr;
    agents::ContextQuery query_;
    agents::Activity activity_ = agents::Activity::Unknown;

    mutable std::mutex mu_;   // msg_, report_, agent_copy_
    proto::AgentContext msg_;
    std::optional<agents::StatusLineReport> report_;
    std::string agent_copy_;
    bool enabled_ = true;
};

// agents::StatusLineReport <-> AGENT_STATUSLINE.
proto::AgentStatusline to_proto(const agents::StatusLineReport&, std::string_view pane = {});
agents::StatusLineReport from_proto(const proto::AgentStatusline&);
// agents::ContextUsage -> AGENT_CONTEXT (without seq).
proto::AgentContext to_proto(const agents::ContextUsage&, const AgentSettings&);

}  // namespace kt::session
