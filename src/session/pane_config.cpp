#include "session/pane_config.h"

#include <algorithm>

#include "agents/context.h"
#include "base/log.h"
#include "platform/paths.h"

namespace kt::session {

PaneSettings pane_settings(const config::Config& c) {
    PaneSettings s;
    s.shell = c.terminal_shell();
    s.shell_args = c.terminal_shell_args();
    for (const auto& [k, v] : c.terminal_env()) s.env.push_back({k, v});
    s.scrollback = static_cast<uint32_t>(std::clamp<int64_t>(c.terminal_scrollback_lines(), 0, 1'000'000));
    s.scrollback_bytes = static_cast<uint32_t>(std::clamp<int64_t>(c.terminal_scrollback_max_bytes(), 1, int64_t{1} << 30));
    s.history = c.session_history();
    s.history_ring = static_cast<uint32_t>(std::clamp<int64_t>(c.session_history_size(), 0, int64_t{1} << 30));
    const std::string& p = c.terminal_pty();
    if (p == "conpty" || p == "agent") {
        s.pty = p;
    } else if (p == "openconsole") {
        log::warn("config", "terminal.pty = openconsole is not available in this build; using auto");
    }
    s.shell_integration = c.terminal_shell_integration();
    return s;
}

AgentSettings agent_settings(const config::Config& c) {
    AgentSettings a;
    a.restore = c.agents_restore();
    a.capture_interval = std::chrono::milliseconds(std::clamp<int64_t>(c.agents_capture_interval(), 1000, 600'000));
    a.claude_config_dir = c.agents_claude_config_dir();
    a.restore_timeout_scale = std::clamp(c.agents_restore_timeout_scale(), 0.01, 10.0);
    a.restore_skip = c.agents_restore_skip();
    a.context = c.agents_context_enabled();
    a.warn_percent = static_cast<int>(std::clamp<int64_t>(c.agents_context_warn_percent(), 1, 100));
    a.alert_percent = static_cast<int>(std::clamp<int64_t>(c.agents_context_alert_percent(), 1, 100));
    a.context_in_private = c.agents_context_show_in_private_panes();
    for (const auto& [glob, size] : c.agents_context_windows()) {
        if (auto n = agents::parse_token_count(size)) a.context_windows.emplace_back(glob, *n);
        else log::warn("config", "agents.context.windows: \"" + glob + "\" is not a token count; ignored");
    }
    a.codex_home = c.agents_codex_home();
    return a;
}

void apply_shell(const PaneSettings& s, pty::SpawnSpec& spec, bool custom_program) {
    if (!custom_program && !s.shell.empty()) {
        spec.argv.assign(1, s.shell);
        spec.argv.insert(spec.argv.end(), s.shell_args.begin(), s.shell_args.end());
    } else if (!custom_program && !s.shell_args.empty() && !spec.argv.empty()) {
        spec.argv.resize(1);
        spec.argv.insert(spec.argv.end(), s.shell_args.begin(), s.shell_args.end());
    }
    for (const auto& e : s.env) {
        std::erase_if(spec.env_edits, [&](const pty::EnvEdit& x) { return x.name == e.name; });
        spec.env_edits.push_back(e);
    }
}

config::LoadOptions load_options(const std::optional<std::string>& file, const std::vector<std::string>& sets) {
    config::LoadOptions o;
    if (auto prof = platform::paths::env_utf8("KEEPTERM_PROFILE"); prof && platform::paths::valid_profile_name(*prof)) {
        o.profile = *prof;
    }
    if (file && !file->empty()) o.file = platform::paths::from_utf8(*file);
    o.cli_sets = sets;
    return o;
}

}  // namespace kt::session
