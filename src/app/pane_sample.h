// What the GUI's monitor learns about one pane from its session host (PROCESS_INFO, and
// AGENT_CONTEXT for an agent pane), and how that lands in the view's pane item. One path for the
// header, the sidebar, the resource indicator and the tests that drive it with real hosts.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "session/protocol.h"
#include "ui/workspace_view.h"

namespace kt::app {

struct PaneSample {
    std::string process;   // foreground program; empty = the shell
    std::string agent;     // agent id when one runs
    uint32_t pid = 0;      // the foreground process (the shell when none runs)
    // The pane's folder (pane_cwd): the agent session's registered folder, else the shell's.
    std::string cwd;
    std::string saved_agent, saved_agent_session;   // the agent session the host would resume
    bool agent_restore = true;
    bool at_prompt = false;
    // The foreground command line (the shell's child: argv joined by spaces, a login '-' dropped),
    // empty at the prompt; and the shell's lowercase image. Apply Layout matches panes by them.
    std::string command;
    std::string shell;
    // AGENT_CONTEXT of an agent pane, status Ok or NoData (NoData: no transcript yet, the model
    // and the agent's own state may still be known).
    std::optional<session::proto::AgentContext> context;
};

// The command line of a foreground chain's first process (what the shell started), empty when
// the chain is empty.
std::string command_of_chain(const std::vector<session::proto::ChainEntry>& chain);

// The agent of a foreground chain, by the capture's own recognition.
std::string agent_of_chain(const std::vector<session::proto::ChainEntry>& chain);

// The folder a pane stands for, one rule for the header, presets and the Sessions view:
//   - an agent runs: the folder registered for its session (the cwd of Claude's session
//     metadata / transcript, Codex's session meta, as the AgentKeeper saved it for resume), not
//     the live process folder that drifts while tools cd around; before the session is
//     registered, the shell's folder;
//   - no agent: the shell's $PWD as the host reports it (shell integration, else the OS query:
//     the PEB on Windows, else the prompt); the foreground chain's live folders last.
std::string pane_cwd(const session::proto::ProcessInfo& pi, const std::string& agent);

// One host's answer. `endpoint` non-empty: AGENT_CONTEXT is asked of an agent pane.
PaneSample sample_pane(const session::proto::ProcessInfo& pi, const std::string& endpoint, int context_timeout_ms = 300);

// The sample into the view's pane item (process, agent, pid, cwd, context, agent state).
void apply_sample(const PaneSample& s, ui::PaneItem& out);

}  // namespace kt::app
