// Claude Code's statusLine JSON (code.claude.com/docs/en/statusline), as forwarded by the relay
// `keepterm --agent-statusline claude` (session/agent_statusline.h). Only numbers and ids are
// taken; the workspace folders, the output style and everything else are skipped unread. Core
// code.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/context.h"

namespace kt::agents {

struct StatusLineReport {
    std::string session_id;
    std::string transcript_path;   // a path, used only to locate the transcript
    std::string model_id;
    std::string model_name;        // model.display_name ("Opus 4.6"), for the default line
    std::string version;
    std::optional<double> cost_usd;
    std::optional<std::uint64_t> window_tokens;   // context_window.context_window_size
    std::optional<double> used_percent;           // context_window.used_percentage
    std::optional<std::uint64_t> used_tokens;     // current_usage: input + cache read + creation
    std::optional<std::uint64_t> total_input, total_output;
    std::vector<RateLimit> limits;                // five_hour, seven_day
    std::int64_t received_unix_ms = 0;
    // When the limits were current (set by the receiver, agents::stamp_limits): Claude Code
    // repeats its last values on every run, so a repeat keeps the time of the first report.
    std::int64_t limits_unix_ms = 0;
    bool limits_first = false;   // the first report with limits of this session
    bool operator==(const StatusLineReport&) const = default;
};

// nullopt when the text is not a statusLine object (no session_id and no model).
std::optional<StatusLineReport> parse_statusline(std::string_view json);

// Overlays the agent's own figures (Source::StatusLine) on the transcript's when the report is of
// the same session. The window, the percent, the cost and the limits come from the report; the
// used tokens too when the report has current_usage.
void merge_statusline(ContextUsage& u, const StatusLineReport& r);

// Dates the limits of `next` (received now): the time of `prev` while the limits and the output
// total are the same (a repeat of the last values), the receive time when either moved.
void stamp_limits(StatusLineReport& next, const StatusLineReport* prev);

// "Opus 4.6 · ctx 142k/200k 71% · $1.23" (parts without data are left out).
std::string compact_statusline(const StatusLineReport& r);

// 1234 -> "1.2k", 142000 -> "142k", 1000000 -> "1M".
std::string short_tokens(std::uint64_t n);

}  // namespace kt::agents
