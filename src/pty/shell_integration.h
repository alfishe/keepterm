// Shell classification and shell integration.
//
// The integration scripts are compiled in as string constants. The host installs them once per
// content hash under <state>/shell-integration/<hash>/ and injects them through the shell's
// arguments or environment only; user files are never written or changed.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "base/error.h"
#include "pty/pty.h"

namespace kt::pty {

enum class ShellKind : uint8_t { Pwsh, PowerShell, Cmd, Bash, Zsh, Fish, Wsl, Other };

struct ShellInfo {
    ShellKind kind = ShellKind::Other;
    bool msys = false;  // Windows: a bash with msys-2.0.dll next to it (Git Bash, MSYS2)
};

// From the basename without ".exe" (case-insensitive on Windows). A leading '-' (login argv[0])
// is ignored.
ShellInfo classify_shell(std::string_view program);
std::string_view to_string(ShellKind);

// Writes the embedded scripts into <state_dir>/shell-integration/<content-hash>/ atomically
// (a temporary folder, then rename). An existing folder for the same hash is kept. User-only
// permissions on POSIX. Returns the folder (UTF-8).
Result<std::string> install_integration_scripts(const std::string& state_dir);

// Injects the integration into `spec` through arguments and environment only. Nothing is
// injected when the arguments run a command or a script (-c, -Command, -File, /c, /k, a script
// path), for unknown shells and for WSL. A non-empty `integration_endpoint` is also put into
// KEEPTERM_INTEGRATION (the side channel). Returns true if anything was injected.
bool apply_integration(SpawnSpec& spec, const std::string& scripts_dir, const std::string& integration_endpoint);

// The script texts, for tests and for hashing.
struct IntegrationFile {
    std::string_view relative_path;  // '/'-separated
    std::string_view content;
};
std::span<const IntegrationFile> integration_files();

}  // namespace kt::pty
