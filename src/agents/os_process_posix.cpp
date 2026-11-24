#include <cerrno>
#include <cstdlib>
#include <limits>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include "agents/os_access.h"

namespace kt::agents {

bool OsProcessProbe::is_alive(std::uint32_t pid) const {
    if (pid == 0 || pid > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

// POSIX pids are reused slowly; no start time check (as in Macterm).
std::optional<std::uint64_t> OsProcessProbe::start_time(std::uint32_t /*pid*/) const { return std::nullopt; }

std::string environment_variable(std::string_view name) {
    const char* v = std::getenv(std::string(name).c_str());
    return v == nullptr ? std::string() : std::string(v);
}

std::string home_directory() {
    std::string home = environment_variable("HOME");
    if (!home.empty()) return home;
    if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr) return pw->pw_dir;
    return {};
}

}  // namespace kt::agents
