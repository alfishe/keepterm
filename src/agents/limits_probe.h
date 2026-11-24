// Fresh rate limits from the agent itself (agents.refresh_limits).
//
// An idle agent session reports the limits of its last API response: Claude Code's status line
// repeats them on every run, so they go stale while the account is used elsewhere or a window
// resets early. A probe is a tiny request of the agent's own CLI whose output carries the
// account's current limits. The adapter of each agent describes its probe (the program, the
// arguments, the parser); the app runs it on a timer and when the agent popover opens, and the
// result wins over older session reports (ContextUsage::limits_unix_ms).
//
// Claude Code: `claude -p` with a one-line system prompt, no tools, no MCP servers, no settings
// and Haiku, about 600 tokens per run. Its stream-json output has a `rate_limit_event` line with
// `rate_limit_info.unifiedWindows.<window>.{utilization, resetsAt}` (utilization 0..1).
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/context.h"
#include "base/error.h"

namespace kt::agents {

struct LimitsProbe {
    std::string agent;                         // descriptor id ("claude")
    std::vector<std::string> program_names;    // tried in PATH, then in `known_dirs`
    std::vector<std::string> known_dirs;       // the agent's usual install folders ("~" = home)
    std::vector<std::string> args;             // after the program
    std::function<std::vector<RateLimit>(std::string_view output)> parse;
};

// The probes of the agents that have one (Claude Code).
const std::vector<LimitsProbe>& builtin_limits_probes();
const LimitsProbe* limits_probe_for(std::string_view agent);

// The limits in Claude Code's stream-json output; empty when it has none.
std::vector<RateLimit> parse_claude_limits(std::string_view output);

// OS side: finds the probe's program and runs it (stdin closed, no console window, in the home
// folder), waiting at most `timeout`; the limits it printed. An error when the program is not
// found, does not finish in time, or prints no limits.
Result<std::vector<RateLimit>> run_limits_probe(const LimitsProbe&, std::chrono::milliseconds timeout);

}  // namespace kt::agents
