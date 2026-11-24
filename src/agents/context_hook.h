// The Claude Code statusLine relay hook:
// `keepterm --agent-hook install|uninstall|status [claude]` and Settings → Agents.
//
// Install sets "statusLine" in <claude config>/settings.json to
//   {"type": "command", "command": "<relay>"}
// where <relay> is
//   POSIX    "<state>/agent-hooks/keepterm-agent-statusline", a two-line sh shim that execs
//            the current keepterm with `--agent-statusline claude`. The shim's path never moves,
//            so Claude's file is written once; the GUI's start-up self-check rewrites the shim
//            when keepterm moved (a new build, a new install location).
//   Windows  "<keepterm.exe>" --agent-statusline claude (forward slashes; a .cmd shim would add
//            the cost of cmd.exe to every status update). The self-check rewrites the command
//            when that executable is gone or another one is running.
// Every other byte of settings.json stays as it was: the member is replaced or inserted in
// place, the file is backed up to settings.json.keepterm-bak and replaced atomically. A
// statusLine that is not keepterm's is kept, byte for byte, in keepterm's own state
// (<state>/agent-hooks/claude.json), never in Claude's file; the relay runs it with the same
// input and prints its output (chaining), and uninstall puts it back. Uninstall of an untouched
// file restores the original bytes exactly. Foreign statusLines are never replaced without an
// explicit install.
//
// Types and the text edits are core code (kt_agents); the file operations are in kt_agents_os.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/error.h"

namespace kt::agents {

enum class HookState : std::uint8_t {
    NotInstalled = 0,
    Installed = 1,
    Stale = 2,      // ours, but the relay path is gone or names another keepterm
    Foreign = 3,    // a statusLine that is not keepterm's
    Repaired = 4,   // was stale; the self-check or install fixed it just now
};
std::string_view to_string(HookState);

struct HookStatus {
    HookState state = HookState::NotInstalled;
    bool chained = false;        // a previous statusLine runs behind the relay
    std::string settings_file;   // the settings.json this edits
    std::string command;         // the statusLine command found there ("" = none)
    std::string error;           // the last operation's error ("" = none)
};
// "Installed, chains the previous status line", "Not installed", ...
std::string describe(const HookStatus&);

struct HookPaths {
    std::string claude_config_dir;   // where settings.json is
    std::string state_dir;           // keepterm's state folder (agent-hooks/ is created in it)
    std::string exe;                 // the keepterm executable the relay runs
};

class IStatusLineHook {
public:
    virtual ~IStatusLineHook() = default;
    virtual HookStatus status() const = 0;
    // Idempotent. Chains a foreign statusLine.
    virtual Result<HookStatus> install() = 0;
    // Restores the previous statusLine (or none). A foreign one is left alone.
    virtual Result<HookStatus> uninstall() = 0;
    // Repairs a stale hook of ours; leaves everything else as it is.
    virtual Result<HookStatus> self_check() = 0;
};

// kt_agents_os.
std::unique_ptr<IStatusLineHook> make_claude_statusline_hook(HookPaths paths);
// Claude's config folder (agents.claude.config_dir, CLAUDE_CONFIG_DIR, ~/.claude), keepterm's
// default-profile state folder (KEEPTERM_STATE_DIR overrides, as for the session host) and `exe`.
HookPaths default_hook_paths(std::string_view configured_claude_dir, std::string exe);
// The chained command saved at install (the "command" of the previous statusLine), if any.
std::optional<std::string> chained_statusline_command(const std::string& state_dir);
// The relay's own command: contains "--agent-statusline" or the shim's name.
bool is_keepterm_statusline(std::string_view command);

// ---- Text edits of a JSON object file (core; exposed for tests)

// The raw bytes of top-level member `key`; nullopt when absent or the text is not an object.
std::optional<std::string> json_member_raw(std::string_view text, std::string_view key);
// `text` with member `key` set to `value_json`: the value bytes replaced in place, or the member
// appended after the last one with the file's indentation. Empty text is "{}". nullopt when the
// text is not a JSON object.
std::optional<std::string> json_set_member(std::string_view text, std::string_view key, std::string_view value_json);
// `text` without member `key` (and the comma that joined it); unchanged when absent.
std::optional<std::string> json_remove_member(std::string_view text, std::string_view key);
// The statusLine value keepterm writes for `command`.
// The statusLine value keepterm writes: the relay command, re-run by Claude Code every
// kStatusLineRefreshSeconds while the session is idle.
inline constexpr int kStatusLineRefreshSeconds = 60;
std::string statusline_value(std::string_view command);

}  // namespace kt::agents
