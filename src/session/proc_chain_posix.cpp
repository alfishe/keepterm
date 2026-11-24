// Process facts on macOS and Linux (public interfaces only).
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#endif

#include "session/cwd.h"
#include "session/proc_chain.h"

namespace kt::session {
namespace {

#ifndef __APPLE__
std::string slurp(const std::string& path, std::size_t max = 1u << 20) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s;
    s.resize(max);
    f.read(s.data(), static_cast<std::streamsize>(max));
    s.resize(static_cast<std::size_t>(f.gcount()));
    return s;
}

// Boot time in seconds since the epoch (/proc/stat btime), read once.
uint64_t boot_time_s() {
    static const uint64_t bt = [] {
        const std::string st = slurp("/proc/stat");
        const auto p = st.find("\nbtime ");
        return p == std::string::npos ? uint64_t{0} : std::strtoull(st.c_str() + p + 7, nullptr, 10);
    }();
    return bt;
}
#endif

}  // namespace

ProcReader make_proc_reader() {
    return [](uint32_t pid) -> std::optional<ProcFacts> {
        if (pid == 0) return std::nullopt;
        ProcFacts f;
#ifdef __APPLE__
        proc_bsdinfo info{};
        if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof info) != static_cast<int>(sizeof info)) {
            return std::nullopt;
        }
        f.ppid = info.pbi_ppid;
        f.start = static_cast<uint64_t>(info.pbi_start_tvsec) * 1000000ull + static_cast<uint64_t>(info.pbi_start_tvusec);
        f.start_unix_ns = f.start * 1000ull;
        f.comm = info.pbi_comm;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (::proc_pidpath(static_cast<int>(pid), path, sizeof path) > 0) {
            f.image = image_name(path);
        } else {
            f.image = image_name(f.comm);
        }
#else
        const std::string stat = slurp("/proc/" + std::to_string(pid) + "/stat", 4096);
        const auto lp = stat.find('(');
        const auto rp = stat.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp) return std::nullopt;
        f.comm = stat.substr(lp + 1, rp - lp - 1);
        // Fields after the command: 3 state, 4 ppid, ..., 22 starttime.
        std::size_t pos = rp + 2;
        uint64_t ticks = 0;
        for (int field = 3; field <= 22 && pos < stat.size(); ++field) {
            if (field == 4) f.ppid = static_cast<uint32_t>(std::strtoul(stat.c_str() + pos, nullptr, 10));
            if (field == 22) ticks = std::strtoull(stat.c_str() + pos, nullptr, 10);
            pos = stat.find(' ', pos);
            if (pos == std::string::npos) break;
            ++pos;
        }
        f.start = ticks;
        const long hz = ::sysconf(_SC_CLK_TCK);
        if (hz > 0 && boot_time_s()) {
            f.start_unix_ns = boot_time_s() * 1'000'000'000ull + ticks * (1'000'000'000ull / static_cast<uint64_t>(hz));
        }
        char exe[PATH_MAX];
        const ssize_t n = ::readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            std::string e(exe, static_cast<std::size_t>(n));
            if (e.size() > 10 && e.compare(e.size() - 10, 10, " (deleted)") == 0) e.resize(e.size() - 10);
            f.image = image_name(e);
        } else {
            f.image = image_name(f.comm);
        }
#endif
        return f;
    };
}

std::vector<std::string> process_argv(uint32_t pid) {
    std::vector<std::string> out;
    if (pid == 0) return out;
#ifdef __APPLE__
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    int maxarg = 0;
    std::size_t len = sizeof maxarg;
    int mib2[2] = {CTL_KERN, KERN_ARGMAX};
    if (::sysctl(mib2, 2, &maxarg, &len, nullptr, 0) != 0 || maxarg <= 0) return out;
    std::string buf(static_cast<std::size_t>(maxarg), '\0');
    len = buf.size();
    if (::sysctl(mib, 3, buf.data(), &len, nullptr, 0) != 0 || len < sizeof(int)) return out;
    int argc = 0;
    std::memcpy(&argc, buf.data(), sizeof argc);
    std::size_t p = sizeof argc;
    while (p < len && buf[p] != '\0') ++p;   // the exec path
    while (p < len && buf[p] == '\0') ++p;   // its padding
    for (int i = 0; i < argc && p < len; ++i) {
        const std::size_t start = p;
        while (p < len && buf[p] != '\0') ++p;
        out.emplace_back(buf.data() + start, p - start);
        ++p;
    }
#else
    const std::string s = slurp("/proc/" + std::to_string(pid) + "/cmdline");
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\0') {
            out.emplace_back(s.data() + start, i - start);
            start = i + 1;
        }
    }
    if (start < s.size()) out.emplace_back(s.data() + start, s.size() - start);
#endif
    return out;
}

std::string process_cwd(uint32_t pid) {
    auto c = query_process_cwd(pid);
    return c ? *c : std::string();
}

}  // namespace kt::session
