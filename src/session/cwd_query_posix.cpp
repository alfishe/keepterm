// Working directory OS query and host facts, POSIX (os_query).

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

#include <pwd.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#endif

#include "session/cwd.h"

namespace kt::session {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string first_label(const std::string& s) { return s.substr(0, s.find('.')); }

}  // namespace

Result<std::string> query_process_cwd(uint32_t pid) {
    if (pid == 0) return make_error(Errc::invalid_argument, "cwd query: pid 0");
#ifdef __APPLE__
    proc_vnodepathinfo vpi{};
    const int n = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi);
    if (n != static_cast<int>(sizeof vpi)) {
        if (errno == ESRCH) return make_error(Errc::lost, "cwd query: no such process", errno);
        return last_os_error("proc_pidinfo PROC_PIDVNODEPATHINFO");
    }
    std::string path(vpi.pvi_cdir.vip_path);
    if (path.empty()) return make_error(Errc::unavailable, "cwd query: empty path");
    return path;
#else
    const std::string link = "/proc/" + std::to_string(pid) + "/cwd";
    std::string buf(PATH_MAX, '\0');
    const ssize_t n = ::readlink(link.c_str(), buf.data(), buf.size());
    if (n < 0) {
        if (errno == ENOENT || errno == ESRCH) return make_error(Errc::lost, "cwd query: no such process", errno);
        return last_os_error("readlink /proc/<pid>/cwd");
    }
    buf.resize(static_cast<std::size_t>(n));
    constexpr std::string_view kDeleted = " (deleted)";
    if (buf.ends_with(kDeleted)) return make_error(Errc::unavailable, "cwd query: directory deleted");
    return buf;
#endif
}

bool is_local_host(std::string_view host) {
    if (host.empty()) return true;
    const std::string h = lower(host);
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;
    char name[256] = {};
    if (::gethostname(name, sizeof name - 1) != 0) return false;
    const std::string self = lower(name);
    return h == self || first_label(h) == first_label(self);
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/";
}

}  // namespace kt::session
