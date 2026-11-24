// Per-OS folders of keepterm.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::platform::paths {

enum class Kind : std::uint8_t { config, state, runtime, logs, cache };

// The folder of `kind` for `profile` (empty = the default profile). A named profile
// lives in `<folder>/profiles/<name>/`. Computes the path only; use
// make_private_dir() to create it.
//   config   Windows %APPDATA%\keepterm, macOS ~/Library/Application Support/keepterm,
//            Linux $XDG_CONFIG_HOME/keepterm (default ~/.config/keepterm)
//   state    %LOCALAPPDATA%\keepterm\state, ~/Library/Application Support/keepterm/state,
//            $XDG_STATE_HOME/keepterm
//   runtime  %LOCALAPPDATA%\keepterm\run, $TMPDIR/keepterm-<uid>, $XDG_RUNTIME_DIR/keepterm
//            (fallback /tmp/keepterm-<uid>)
//   logs     %LOCALAPPDATA%\keepterm\logs, ~/Library/Logs/keepterm, $XDG_STATE_HOME/keepterm/logs
//   cache    %LOCALAPPDATA%\keepterm\cache, ~/Library/Caches/keepterm, $XDG_CACHE_HOME/keepterm
Result<std::filesystem::path> get(Kind kind, std::string_view profile = {});

// Extra config folders read at lower precedence than get(Kind::config):
// on macOS $XDG_CONFIG_HOME/keepterm (default ~/.config/keepterm); empty elsewhere.
std::vector<std::filesystem::path> extra_config_dirs(std::string_view profile = {});

// The user's home folder (HOME / the passwd record; %USERPROFILE% on Windows).
std::filesystem::path home();

// True for a valid profile name: [a-z0-9-]{1,32}.
bool valid_profile_name(std::string_view name);

// Creates `dir` and missing parents; the last component gets mode 0700 on POSIX
// (Windows: the inherited user ACL of %APPDATA% / %LOCALAPPDATA%).
Result<void> make_private_dir(const std::filesystem::path& dir);

// An environment variable as UTF-8 (GetEnvironmentVariableW on Windows); nullopt when unset.
std::optional<std::string> env_utf8(std::string_view name);

// UTF-8 form of a path (std::filesystem::path::u8string without the char8_t type).
std::string to_utf8(const std::filesystem::path& p);
std::filesystem::path from_utf8(std::string_view s);

}  // namespace kt::platform::paths
