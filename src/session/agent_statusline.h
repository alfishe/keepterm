// `keepterm --agent-statusline claude` and `keepterm --agent-hook install|uninstall|status [claude]`
// (agents/context_hook.h describes the hook itself).
//
// The relay runs as Claude Code's statusLine command, once per status update:
//   1. reads the statusLine JSON from stdin (at most 1 MiB);
//   2. with KEEPTERM_SESSION set (a keepterm pane), sends its numbers and ids as AGENT_STATUSLINE
//      to that pane's session host (KEEPTERM_PANE goes along), giving up after 50 ms;
//   3. prints the status line: the output of the chained previous statusLine command (run with
//      the same stdin, its output passed through unchanged), else the compact default
//      "Model · ctx 142k/200k 71% · $1.23", or nothing with agents.claude.statusline_default =
//      "none".
// It never fails loudly: every error ends in an exit code of 0 and, at worst, an empty line.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "agents/context_hook.h"

namespace kt::session {

int agent_statusline_main(int argc, char** argv, int i);
int agent_hook_main(int argc, char** argv, int i);

// The hook for the current keepterm executable, the configured Claude folder and the session
// state folder of this profile (os::paths()).
std::unique_ptr<agents::IStatusLineHook> claude_statusline_hook(std::string_view configured_claude_dir);
// The GUI's start-up check: repairs a stale hook of ours, silently; never touches a foreign
// statusLine. Logs one line when it repaired something.
void statusline_self_check(std::string_view configured_claude_dir);

}  // namespace kt::session
