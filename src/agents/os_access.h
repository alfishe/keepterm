// Real implementations of the agents module's platform access (kt_agents_os): the file system
// through std::filesystem, process liveness through the OS. Stand-ins until the platform layer provides the
// platform interfaces.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agents/session_files.h"

namespace kt::agents {

class StdFileSystem final : public IFileSystem {
public:
    std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes) const override;
    std::optional<std::vector<DirEntry>> list_dir(const std::string& path) const override;
    std::optional<std::int64_t> mtime_ns(const std::string& path) const override;
    std::optional<FileStat> stat(const std::string& path) const override;
    std::optional<std::string> read_range(const std::string& path, std::uint64_t offset,
                                          std::size_t max_bytes) const override;
};

// POSIX: kill(pid, 0) succeeds or fails with EPERM. Windows: OpenProcess with
// PROCESS_QUERY_LIMITED_INFORMATION and STILL_ACTIVE (access denied counts as alive), and
// GetProcessTimes for the start time.
class OsProcessProbe final : public IProcessProbe {
public:
    bool is_alive(std::uint32_t pid) const override;
    std::optional<std::uint64_t> start_time(std::uint32_t pid) const override;
};

// An environment variable as UTF-8; empty when unset.
std::string environment_variable(std::string_view name);
// HOME (POSIX) or USERPROFILE (Windows).
std::string home_directory();
// Paths of the built-in sources: `configured_claude_dir` (agents.claude.config_dir), else
// CLAUDE_CONFIG_DIR, else <home>/.claude.
SourcePaths default_source_paths(std::string_view configured_claude_dir = {});

}  // namespace kt::agents
