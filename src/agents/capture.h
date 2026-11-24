// Foreground chain recognition and the per-pane record. Port of
// Macterm's AppState.agentResume(pid:files:current:). Core code: the OS is reached only through
// the registry's session sources and the command line callback.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agents/agent_resume.h"
#include "agents/registry.h"

namespace kt::agents {

// Fetches the command line of a process whose argv is unknown (Windows: the resource monitor's
// CommandLineService, the NT process snapshot or WMI, cached per pid and creation time). nullopt: unreadable.
using CommandLineFetch = std::function<std::optional<std::vector<std::string>>(const ProcessFacts&)>;

struct ChainMatch {
    const AgentDescriptor* agent = nullptr;
    std::size_t index = 0;
    std::vector<std::string> argv;  // facts.argv, or the fetched one; empty = unknown
    bool fetched = false;           // the fetch already ran for this entry (successfully or not)
};

// The first entry from the shell side that is an agent. Launchers before it are skipped without
// a fetch; the agent's children after it are never examined. An interpreter entry with unknown
// argv is decided, cheapest first, by a session source that proves it (Claude pid file), then by
// `fetch`; without either it stays unrecognized.
std::optional<ChainMatch> recognize_chain(std::span<const ProcessFacts> chain, const AgentRegistry& registry,
                                          const CommandLineFetch& fetch = {});

// The record for a pane whose foreground chain is `chain` (shell's child first; empty = the shell
// itself). `pane_cwd` is the pane's working directory from PROCESS_INFO. `current` is the pane's
// record before this capture. Returns:
//   none     another program, a one-shot run, an icon-only agent, a Claude session of another kind;
//   current  (when it is the same agent) the agent's files are not readable yet;
//   a record otherwise. The directory is the session file's, else the process's, else pane_cwd.
std::optional<AgentResume> build_record(std::span<const ProcessFacts> chain, std::string_view pane_cwd,
                                        const AgentRegistry& registry, const std::optional<AgentResume>& current,
                                        const CommandLineFetch& fetch = {});

// Whether the restore may treat the agent as ready: the chain holds
// agent `agent_id` and its source says ready.
bool agent_is_ready(std::span<const ProcessFacts> chain, std::string_view agent_id, std::string_view pane_cwd,
                    const AgentRegistry& registry, const CommandLineFetch& fetch = {});

}  // namespace kt::agents
