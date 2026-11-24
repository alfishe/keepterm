// Agent session capture and restore in the session host.
//
// The design places capture and restore in the GUI's AgentService over the workspace.
// Until the workspace exists, each host does it for its own pane: it captures the agent resume
// record of its foreground chain into record.json (periodically and when the host is told to
// end), and a host started with --restore-from types the resume command into its new shell
// once that shell is idle, with the restore steps, timeouts and user-input guard of the restore.
// The host's history replay is finished before the child starts, so step 2 always holds.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "agents/agent_resume.h"
#include "agents/registry.h"
#include "agents/session_files.h"
#include "session/json.h"
#include "session/pane_config.h"
#include "session/protocol.h"

namespace kt::session {

// What the host gives the keeper. Every function may be called from the keeper's thread.
struct AgentPane {
    // The current PROCESS_INFO with the foreground chain and the shell image; nullopt when the
    // terminal is not live.
    std::function<std::optional<proto::ProcessInfo>()> process_info;
    // Types bytes into the PTY as input; false when the terminal is not live.
    std::function<bool(std::string_view)> type;
    // Monotonic time of the last INPUT from any client (the user-input guard).
    std::function<uint64_t()> last_input_ns;
    // True when the shell has stopped: no more waiting.
    std::function<bool()> cancelled;
    // Clock and sleep (tests replace them).
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
};

// The restore step timings (tests shorten them).
struct RestoreTimings {
    std::chrono::milliseconds foreground{30'000}, idle{20'000}, settle{1'500}, poll{500}, ready_poll{1'000};
};

class AgentKeeper {
public:
    // fs and probe: nullptr = the real file system and process probe (agents/os_access.h).
    AgentKeeper(AgentSettings settings, AgentPane pane, const agents::IFileSystem* fs = nullptr,
                const agents::IProcessProbe* probe = nullptr);
    ~AgentKeeper();

    // agents.claude.config_dir and the other agents keys, live.
    void set_settings(const AgentSettings&);
    AgentSettings settings() const;
    void set_timings(const RestoreTimings& t) { timings_ = t; }

    // the record for this process info. nullopt = none (another program, an idle shell).
    // No foreground known: `current` stays.
    std::optional<agents::AgentResume> capture(const proto::ProcessInfo&,
                                               const std::optional<agents::AgentResume>& current) const;

    // The restore of one pane, blocking. Returns the outcome of the log line ("ready",
    // "not ready in time", "the user took over", ...). The line is logged (category agents).
    std::string restore(const agents::AgentResume& record, std::string_view session_id);

    // The facts of the chain entries for the agents module.
    static std::vector<agents::ProcessFacts> facts_of(const proto::ProcessInfo&);
    // The idle-shell test on the host's facts.
    static bool idle_shell(const proto::ProcessInfo&);

private:
    void rebuild_registry();
    bool wait_until(std::chrono::milliseconds timeout, std::chrono::milliseconds poll,
                    const std::function<bool()>& pred);

    AgentSettings settings_;
    AgentPane pane_;
    RestoreTimings timings_;
    std::unique_ptr<agents::IFileSystem> own_fs_;
    std::unique_ptr<agents::IProcessProbe> own_probe_;
    const agents::IFileSystem* fs_;
    const agents::IProcessProbe* probe_;
    std::unique_ptr<agents::ICodexIndex> codex_index_;   // made once: registries point to it
    std::shared_ptr<agents::AgentRegistry> registry_;
    mutable std::mutex mu_;   // settings_ and registry_
};

// record.json's "agent" value <-> the record.
std::optional<agents::AgentResume> agent_record_from_json(const json::Value&);
json::Value agent_record_to_json(const std::optional<agents::AgentResume>&);

}  // namespace kt::session
