// The config keys a session uses: new-pane keys, read when a session or a local shell
// starts (Apply::NewPanes), and the agents keys the host applies live.
//
// The host loads the config itself, with the same profile, file and --set values as the window
// that started it (`--config <file>`, `--set key=value` on the host command line), so a session
// started by the CLI follows the same settings. Explicit host options (--pty=, --no-history,
// --scrollback, -- <argv>) win over the config.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.h"
#include "pty/pty.h"

namespace kt::session {

struct PaneSettings {
    std::string shell;                     // terminal.shell; empty = the platform default
    std::vector<std::string> shell_args;   // terminal.shell_args
    std::vector<pty::EnvEdit> env;         // terminal.env
    uint32_t scrollback = 10'000;          // terminal.scrollback_lines
    uint32_t scrollback_bytes = 8u << 20;  // terminal.scrollback_max_bytes
    bool history = true;                   // session.history
    uint32_t history_ring = 0;             // session.history_size (journal ring bytes); 0 = default
    std::string pty;                       // terminal.pty: "", "conpty", "agent" ("auto" -> "")
    bool shell_integration = true;         // terminal.shell_integration
};

struct AgentSettings {
    bool restore = true;                                  // agents.restore
    std::chrono::milliseconds capture_interval{60'000};   // agents.capture_interval
    std::string claude_config_dir;                        // agents.claude.config_dir
    double restore_timeout_scale = 1.0;                   // agents.restore_timeout_scale
    std::vector<std::string> restore_skip;                // agents.restore_skip: agent ids left alone
    // Context telemetry (agents.context.*, agents.codex.home; agents/context.h).
    bool context = true;                                  // agents.context.enabled
    int warn_percent = 70, alert_percent = 90;            // agents.context.warn_percent, alert_percent
    bool context_in_private = false;                      // agents.context.show_in_private_panes
    std::vector<std::pair<std::string, std::uint64_t>> context_windows;   // agents.context.windows
    std::string codex_home;                               // agents.codex.home
    bool restores(std::string_view agent) const {
        for (const auto& s : restore_skip) {
            if (s == agent) return false;
        }
        return restore;
    }
};

PaneSettings pane_settings(const config::Config&);
AgentSettings agent_settings(const config::Config&);

// The shell of `s` (program plus arguments) and its environment edits on `spec`. With a custom
// program (`-- <argv>` on a command line) only the environment applies.
void apply_shell(const PaneSettings& s, pty::SpawnSpec& spec, bool custom_program);

// How a session process loads its config: the profile of KEEPTERM_PROFILE (when it is a valid
// profile name), an optional --config file and --set values.
config::LoadOptions load_options(const std::optional<std::string>& file, const std::vector<std::string>& sets);

}  // namespace kt::session
