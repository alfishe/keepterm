#include "resources/containment_setup.h"

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#endif

namespace kt::resources {

#ifdef __linux__
namespace {

constexpr long kCgroup2Magic = 0x63677270;

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text(const std::string& path, std::string_view text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, text.data(), text.size());
    ::close(fd);
    return n == static_cast<ssize_t>(text.size());
}

bool has_word(std::string_view list, std::string_view word) {
    std::size_t p = 0;
    while (p < list.size()) {
        while (p < list.size() && (list[p] == ' ' || list[p] == '\n')) ++p;
        std::size_t e = p;
        while (e < list.size() && list[e] != ' ' && list[e] != '\n') ++e;
        if (list.substr(p, e - p) == word) return true;
        p = e;
    }
    return false;
}

std::string prefix_of(std::string_view profile) {
    return "keepterm-" + std::string(profile.empty() ? "default" : profile) + "-";
}

}  // namespace

std::string unified_cgroup_path(std::string_view text) {
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        const std::string_view line = text.substr(p, e - p);
        if (line.substr(0, 3) == "0::") return std::string(line.substr(3));
        p = e + 1;
    }
    return {};
}

bool cgroup_delegated(const std::string& dir, bool systemd) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) return false;
    if (::access((dir + "/cgroup.procs").c_str(), W_OK) != 0) return false;
    if (::access((dir + "/cgroup.subtree_control").c_str(), W_OK) != 0) return false;
    if (!systemd) return true;
    // systemd 251+ marks delegated subtrees; writing into a managed one would break its view.
    for (const char* name : {"trusted.delegate", "user.delegate"}) {
        if (::getxattr(dir.c_str(), name, nullptr, 0) >= 0) return true;
    }
    return false;
}

void sweep_session_cgroups(const std::string& parent, std::string_view profile) {
    const std::string prefix = prefix_of(profile);
    DIR* d = ::opendir(parent.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0) names.emplace_back(e->d_name);
    }
    ::closedir(d);
    for (const auto& n : names) {
        const std::string dir = parent + "/" + n;
        bool empty = true;
        for (const char* sub : {"/host", "/work"}) {
            const std::string procs = read_text(dir + sub + "/cgroup.procs");
            if (procs.find_first_not_of(" \n") != std::string::npos) empty = false;
        }
        if (read_text(dir + "/cgroup.procs").find_first_not_of(" \n") != std::string::npos) empty = false;
        if (!empty) continue;
        (void)::rmdir((dir + "/work").c_str());
        (void)::rmdir((dir + "/host").c_str());
        (void)::rmdir(dir.c_str());
    }
}

HostContainment setup_cgroup(const CgroupEnv& env, std::string_view session_id, std::string_view profile) {
    HostContainment c;
    if (env.self_cgroup.empty()) {
        c.note = "no cgroup v2 entry for this process";
        return c;
    }
    std::string rel = env.self_cgroup;
    if (rel == "/") rel.clear();
    const std::string parent = env.mount + rel;
    if (!cgroup_delegated(parent, env.systemd)) {
        c.note = "cgroup " + env.self_cgroup + " is not delegated";
        return c;
    }
    sweep_session_cgroups(parent, profile);
    const std::string name = prefix_of(profile) + std::string(session_id);
    const std::string dir = parent + "/" + name;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        c.note = "mkdir " + dir + ": " + std::strerror(errno);
        return c;
    }
    auto undo = [&] {
        (void)::rmdir((dir + "/work").c_str());
        (void)::rmdir((dir + "/host").c_str());
        (void)::rmdir(dir.c_str());
    };
    if ((::mkdir((dir + "/host").c_str(), 0755) != 0 && errno != EEXIST) ||
        (::mkdir((dir + "/work").c_str(), 0755) != 0 && errno != EEXIST)) {
        c.note = "mkdir in " + dir + ": " + std::strerror(errno);
        undo();
        return c;
    }
    // The host first moves into host/ (no internal processes in the session cgroup).
    if (env.move_self && !write_text(dir + "/host/cgroup.procs", std::to_string(::getpid()))) {
        c.note = "cannot move the host into " + dir + "/host: " + std::strerror(errno);
        undo();
        return c;
    }
    // memory and pids for host/ and work/: the parent must pass them down first. That fails when
    // the parent itself holds processes (the GUI); the session is then CgroupNoMemory.
    std::string avail = read_text(dir + "/cgroup.controllers");
    for (const char* ctl : {"memory", "pids"}) {
        if (!has_word(avail, ctl)) (void)write_text(parent + "/cgroup.subtree_control", std::string("+") + ctl);
    }
    avail = read_text(dir + "/cgroup.controllers");
    bool memory = false;
    for (const char* ctl : {"memory", "pids"}) {
        if (!has_word(avail, ctl)) continue;
        const bool ok = write_text(dir + "/cgroup.subtree_control", std::string("+") + ctl);
        if (ok && std::strcmp(ctl, "memory") == 0) memory = true;
    }
    c.procs_fd = ::open((dir + "/work/cgroup.procs").c_str(), O_WRONLY | O_CLOEXEC | (env.move_self ? 0 : O_CREAT), 0600);
    if (c.procs_fd < 0) {
        c.note = "cannot open " + dir + "/work/cgroup.procs: " + std::strerror(errno);
        if (env.move_self) (void)write_text(parent + "/cgroup.procs", std::to_string(::getpid()));
        undo();
        return c;
    }
    c.kind = memory ? ContainmentKind::Cgroup : ContainmentKind::CgroupNoMemory;
    c.handle = rel + "/" + name;
    c.note = std::string(memory ? "cgroup " : "cgroup without the memory controller ") + c.handle;
    return c;
}

HostContainment setup_host_containment(std::string_view session_id, std::string_view profile, bool subreaper) {
    CgroupEnv env;
    env.mount = "/sys/fs/cgroup";
    struct statfs sf{};
    HostContainment c;
    if (::statfs(env.mount.c_str(), &sf) == 0 && static_cast<long>(sf.f_type) == kCgroup2Magic) {
        env.self_cgroup = unified_cgroup_path(read_text("/proc/self/cgroup"));
        struct stat st{};
        env.systemd = ::stat("/run/systemd/system", &st) == 0;
        c = setup_cgroup(env, session_id, profile);
    } else {
        c.note = "no cgroup v2 at /sys/fs/cgroup";
    }
    if (c.kind == ContainmentKind::None && subreaper) {
        c.kind = ContainmentKind::Subreaper;
        c.note += c.note.empty() ? "subreaper" : "; subreaper";
    }
    return c;
}

void release_host_containment(HostContainment& c) {
    if (c.procs_fd >= 0) ::close(c.procs_fd);
    c.procs_fd = -1;
}

#else

HostContainment setup_host_containment(std::string_view, std::string_view, bool) { return {}; }
void release_host_containment(HostContainment&) {}

#endif

}  // namespace kt::resources
