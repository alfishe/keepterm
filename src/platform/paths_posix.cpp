// platform::paths on macOS and Linux.
#include <cerrno>
#include <cstdlib>
#include <string>
#include <system_error>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/paths.h"

namespace kt::platform::paths {
namespace fs = std::filesystem;

namespace {

// An XDG variable counts only when it is set to an absolute path (XDG base directory spec).
fs::path xdg(const char* var, const fs::path& fallback) {
    const char* v = std::getenv(var);
    if (v && v[0] == '/') return fs::path(v);
    return fallback;
}

fs::path with_profile(fs::path base, std::string_view profile) {
    if (!profile.empty()) base = base / "profiles" / std::string(profile);
    return base;
}

std::string uid_suffix() { return "keepterm-" + std::to_string(static_cast<unsigned long>(::getuid())); }

}  // namespace

fs::path home() {
    if (const char* h = std::getenv("HOME"); h && h[0]) return fs::path(h);
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return fs::path(pw->pw_dir);
    return fs::path("/");
}

Result<fs::path> get(Kind kind, std::string_view profile) {
    if (!profile.empty() && !valid_profile_name(profile))
        return make_error(Errc::invalid_argument, "invalid profile name");
    const fs::path h = home();
#ifdef __APPLE__
    const fs::path support = h / "Library" / "Application Support" / "keepterm";
    switch (kind) {
        case Kind::config: return with_profile(support, profile);
        case Kind::state: return with_profile(support / "state", profile);
        case Kind::runtime: {
            const char* tmp = std::getenv("TMPDIR");
            const fs::path base = (tmp && tmp[0] == '/') ? fs::path(tmp) : fs::path("/tmp");
            return with_profile(base / uid_suffix(), profile);
        }
        case Kind::logs: return with_profile(h / "Library" / "Logs" / "keepterm", profile);
        case Kind::cache: return with_profile(h / "Library" / "Caches" / "keepterm", profile);
    }
#else
    switch (kind) {
        case Kind::config: return with_profile(xdg("XDG_CONFIG_HOME", h / ".config") / "keepterm", profile);
        case Kind::state: return with_profile(xdg("XDG_STATE_HOME", h / ".local" / "state") / "keepterm", profile);
        case Kind::runtime: {
            const char* rt = std::getenv("XDG_RUNTIME_DIR");
            const fs::path base = (rt && rt[0] == '/') ? fs::path(rt) / "keepterm" : fs::path("/tmp") / uid_suffix();
            return with_profile(base, profile);
        }
        case Kind::logs:
            return with_profile(xdg("XDG_STATE_HOME", h / ".local" / "state") / "keepterm" / "logs", profile);
        case Kind::cache: return with_profile(xdg("XDG_CACHE_HOME", h / ".cache") / "keepterm", profile);
    }
#endif
    return make_error(Errc::invalid_argument, "unknown folder kind");
}

std::vector<fs::path> extra_config_dirs(std::string_view profile) {
#ifdef __APPLE__
    if (!profile.empty() && !valid_profile_name(profile)) return {};
    return {with_profile(xdg("XDG_CONFIG_HOME", home() / ".config") / "keepterm", profile)};
#else
    (void)profile;
    return {};
#endif
}

std::optional<std::string> env_utf8(std::string_view name) {
    const std::string n(name);
    if (const char* v = std::getenv(n.c_str())) return std::string(v);
    return std::nullopt;
}

Result<void> make_private_dir(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return {};
    if (dir.has_parent_path() && dir.parent_path() != dir) {
        fs::create_directories(dir.parent_path(), ec);
        if (ec) return make_error(Errc::os_error, "create_directories: " + ec.message(), ec.value());
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return last_os_error("mkdir");
    return {};
}

}  // namespace kt::platform::paths
