// Linux process source: a /proc scan with cached stat fds, statm for the
// measured rows, PSS from smaps_rollup within a budget, and cgroup v2 readings.
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/procfs_parse.h"
#include "resources/source.h"

namespace kt::resources {
namespace {

using steady = std::chrono::steady_clock;

constexpr uint64_t kMiB = 1024 * 1024;
constexpr uint64_t kPssMinAnon = 4 * kMiB;           // PSS only for RssAnon >= 4 MiB
constexpr auto kPssMaxAge = std::chrono::seconds(10);
constexpr uint64_t kKthreadRecheck = 16;             // ticks between stat reads of a kernel thread

// pread of a whole small file from offset 0. Returns false on error (ESRCH for a dead process).
bool pread_all(int fd, std::string& out, size_t cap) {
    out.resize(cap);
    size_t got = 0;
    for (;;) {
        const ssize_t n = ::pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
        if (n == 0 || got < out.size()) break;
        out.resize(out.size() * 2);
    }
    out.resize(got);
    return true;
}

bool read_file(int dirfd, const char* path, std::string& out, size_t cap = 4096) {
    const int fd = ::openat(dirfd, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = pread_all(fd, out, cap);
    ::close(fd);
    return ok;
}

bool read_path(const std::string& path, std::string& out, size_t cap = 4096) {
    return read_file(AT_FDCWD, path.c_str(), out, cap);
}

class LinuxSource final : public ProcessSource {
public:
    explicit LinuxSource(const SourceOptions&) {
        const long tck = ::sysconf(_SC_CLK_TCK);
        ns_per_tick_ = tck > 0 ? 1000000000ull / static_cast<uint64_t>(tck) : 10000000ull;
        const long ps = ::sysconf(_SC_PAGESIZE);
        page_ = ps > 0 ? static_cast<uint64_t>(ps) : 4096;
        uid_ = ::getuid();
        proc_fd_ = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
            // Half of the soft limit at most: the rest of the process needs fds too.
            fd_cap_ = std::min<size_t>(2048, static_cast<size_t>(rl.rlim_cur) / 2);
        }
    }

    ~LinuxSource() override {
        for (auto& [pid, e] : cache_) close_entry(e);
        if (proc_fd_ >= 0) ::close(proc_fd_);
    }

    OsKind os() const override { return OsKind::Linux; }
    std::string_view main_metric_name() const override { return "pss"; }
    std::string_view resident_metric_name() const override { return "rss"; }

    Result<std::span<RawProc>> take() override {
        if (proc_fd_ < 0) return make_error(Errc::unavailable, "/proc is not mounted");
        const int dup = ::dup(proc_fd_);
        if (dup < 0) return last_os_error("dup /proc");
        DIR* dir = ::fdopendir(dup);
        if (!dir) {
            ::close(dup);
            return last_os_error("opendir /proc");
        }
        ::rewinddir(dir);
        rows_.clear();
        ++generation_;
        char path[64];
        while (dirent* de = ::readdir(dir)) {
            const char* nm = de->d_name;
            if (nm[0] < '1' || nm[0] > '9') continue;
            uint32_t pid = 0;
            bool numeric = true;
            for (const char* p = nm; *p; ++p) {
                if (*p < '0' || *p > '9') {
                    numeric = false;
                    break;
                }
                pid = pid * 10 + static_cast<uint32_t>(*p - '0');
            }
            if (!numeric) continue;

            Entry& e = cache_[pid];
            if (e.kthread && generation_ - e.verified < kKthreadRecheck) {
                // Kernel threads never belong to a session and never change parents: their row
                // comes from the cache, and their stat is read again every few ticks only.
                e.generation = generation_;
                RawProc r;
                r.pid = pid;
                r.ppid = e.ppid;
                r.start_time = e.start;
                r.name = e.name;
                r.own_user = false;
                rows_.push_back(std::move(r));
                continue;
            }
            std::snprintf(path, sizeof path, "%u/stat", pid);
            std::optional<procfs::Stat> st;
            // A cached fd of a process that exited fails with ESRCH even when the PID is reused:
            // then the second attempt opens the new process.
            for (int attempt = 0; attempt < 2 && !st; ++attempt) {
                bool temp = false;
                if (e.stat_fd < 0) {
                    const int fd = ::openat(proc_fd_, path, O_RDONLY | O_CLOEXEC);
                    if (fd < 0) break;
                    struct stat sb{};
                    if (::fstat(fd, &sb) == 0) e.uid = sb.st_uid;
                    e.stat_fd = fd;
                    if (open_fds_ < fd_cap_) {
                        ++open_fds_;
                    } else {
                        temp = true;
                    }
                }
                if (pread_all(e.stat_fd, buf_, 1024)) st = procfs::parse_stat(buf_);
                if (temp) {
                    ::close(e.stat_fd);
                    e.stat_fd = -1;
                } else if (!st) {
                    close_entry(e);
                }
            }
            if (!st) {
                close_entry(e);
                cache_.erase(pid);
                continue;
            }
            if (e.start != 0 && e.start != st->starttime) {
                // A cached fd of an exited process fails with ESRCH; a reused PID that got here
                // through a fresh open has a new start time: drop the old caches.
                if (e.statm_fd >= 0) {
                    ::close(e.statm_fd);
                    e.statm_fd = -1;
                    --open_fds_;
                }
            }
            e.start = st->starttime;
            e.generation = generation_;
            e.kthread = pid == 2 || st->ppid == 2;
            if (e.kthread) {
                e.verified = generation_;
                e.ppid = st->ppid;
                e.name = st->comm;
                close_entry(e);  // no fd budget for kernel threads
            }

            RawProc r;
            r.pid = pid;
            r.ppid = st->ppid;
            r.start_time = st->starttime;
            r.name = std::move(st->comm);
            r.threads = st->num_threads;
            r.pgid = st->pgrp;
            r.sid = st->session;
            if (st->tty_nr != 0) r.tty_dev = static_cast<uint64_t>(static_cast<uint32_t>(st->tty_nr));
            r.own_user = e.uid == uid_;
            r.cpu_ns = (st->utime + st->stime) * ns_per_tick_;
            r.resident = st->rss_pages * page_;
            rows_.push_back(std::move(r));
        }
        ::closedir(dir);
        // Drop the caches of processes that are gone.
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->second.generation != generation_) {
                close_entry(it->second);
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pss_.begin(); it != pss_.end();) {
            auto c = cache_.find(it->first.pid);
            it = (c == cache_.end() || c->second.start != it->first.start_time) ? pss_.erase(it) : std::next(it);
        }
        return std::span<RawProc>(rows_);
    }

    void measure(std::span<RawProc* const> rows, const MeasureOptions& opt) override {
        char path[64];
        for (RawProc* r : rows) {
            auto c = cache_.find(r->pid);
            if (c == cache_.end()) continue;
            Entry& e = c->second;
            bool ok = false;
            if (e.statm_fd < 0) {
                std::snprintf(path, sizeof path, "%u/statm", r->pid);
                const int fd = ::openat(proc_fd_, path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    ok = pread_all(fd, buf_, 256);
                    if (ok && open_fds_ < fd_cap_) {
                        e.statm_fd = fd;
                        ++open_fds_;
                    } else {
                        ::close(fd);
                    }
                }
            } else {
                ok = pread_all(e.statm_fd, buf_, 256);
            }
            auto sm = ok ? procfs::parse_statm(buf_) : std::nullopt;
            if (!sm) {
                r->access_denied = errno == EACCES || errno == EPERM;
                continue;
            }
            r->measured = true;
            r->resident = sm->resident * page_;
            const uint64_t anon = (sm->resident > sm->shared ? sm->resident - sm->shared : 0) * page_;
            r->private_resident = anon;
            if (auto p = pss_.find(r->key()); p != pss_.end()) {
                r->main = p->second.pss;
                r->main_estimated = false;
            } else {
                r->main = anon;
                r->main_estimated = true;
            }
            if (opt.detail) {
                std::snprintf(path, sizeof path, "%u/status", r->pid);
                if (read_file(proc_fd_, path, buf_)) r->peak = procfs::find_kb_field(buf_, "VmHWM");
            }
        }
    }

    bool refine(std::span<RawProc* const> rows, std::chrono::microseconds budget) override {
        const auto start = steady::now();
        struct Cand {
            RawProc* r;
            double score;
        };
        std::vector<Cand> cands;
        for (RawProc* r : rows) {
            if (!r->measured || r->own_user != true || r->private_resident.value_or(0) < kPssMinAnon) continue;
            auto p = pss_.find(r->key());
            double age_s = 1e9;  // never read: first
            if (p != pss_.end()) {
                const auto age = start - p->second.read_at;
                if (age < kPssMaxAge) continue;
                age_s = std::chrono::duration<double>(age).count();
            }
            cands.push_back({r, age_s * static_cast<double>(*r->private_resident)});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        char path[64];
        for (const Cand& c : cands) {
            if (steady::now() - start >= budget) return false;
            std::snprintf(path, sizeof path, "%u/smaps_rollup", c.r->pid);
            if (!read_file(proc_fd_, path, buf_)) continue;
            const auto sr = procfs::parse_smaps_rollup(buf_);
            if (!sr.pss) continue;
            pss_[c.r->key()] = PssEntry{*sr.pss, steady::now()};
            c.r->main = *sr.pss;
            c.r->main_estimated = false;
        }
        return true;
    }

    Result<ContainmentReading> read_containment(const Containment& c) override {
        if (c.kind != ContainmentKind::Cgroup && c.kind != ContainmentKind::CgroupNoMemory)
            return make_error(Errc::not_supported, "containment kind has no reading on Linux");
        if (!mount_) {
            std::string mi;
            if (!read_path("/proc/self/mountinfo", mi, 64 * 1024)) return last_os_error("read mountinfo");
            mount_ = procfs::find_cgroup2_mount(mi).value_or(std::string());
        }
        if (mount_->empty()) return make_error(Errc::unavailable, "no cgroup2 mount");
        std::string dir = *mount_;
        if (!c.handle.empty() && c.handle.front() != '/') dir += '/';
        dir += c.handle;
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();

        ContainmentReading rd;
        std::string text;
        if (!read_path(dir + "/cgroup.procs", text, 64 * 1024)) return last_os_error("read cgroup.procs");
        rd.pids = procfs::parse_pid_list(text);
        collect_children(dir, rd.pids, 0);
        if (c.kind == ContainmentKind::Cgroup) {
            if (read_path(dir + "/memory.current", text)) rd.main = procfs::parse_number(text);
            if (read_path(dir + "/memory.peak", text)) rd.peak = procfs::parse_number(text);
            if (read_path(dir + "/memory.stat", text, 16 * 1024)) {
                const auto ms = procfs::parse_memory_stat(text);
                rd.file_cache = ms.file;
                rd.shmem = ms.shmem;
            }
        }
        if (read_path(dir + "/cpu.stat", text)) {
            if (auto us = procfs::find_key_value(text, "usage_usec")) rd.cpu_ns = *us * 1000;
        }
        return rd;
    }

    ProcessDetails details(const ProcessKey& key) override {
        ProcessDetails d;
        char path[64];
        std::snprintf(path, sizeof path, "%u/stat", key.pid);
        std::string text;
        if (!read_file(proc_fd_, path, text)) return d;
        auto st = procfs::parse_stat(text);
        if (!st || (key.start_time != 0 && st->starttime != key.start_time)) return d;
        std::snprintf(path, sizeof path, "%u/exe", key.pid);
        char link[4096];
        const ssize_t n = ::readlinkat(proc_fd_, path, link, sizeof link);
        if (n > 0) d.path = std::string(link, static_cast<size_t>(n));
        std::snprintf(path, sizeof path, "%u/cmdline", key.pid);
        if (read_file(proc_fd_, path, text, 64 * 1024)) {
            if (!text.empty() && text.back() == '\0') text.pop_back();
            d.argv = procfs::split_nul(text);
        }
        return d;
    }

private:
    struct Entry {
        int stat_fd = -1;
        int statm_fd = -1;
        uint64_t start = 0;
        uint32_t uid = 0;
        uint64_t generation = 0;
        bool kthread = false;
        uint64_t verified = 0;  // kernel threads: tick of the last stat read
        uint32_t ppid = 0;
        std::string name;
    };
    struct PssEntry {
        uint64_t pss = 0;
        steady::time_point read_at;
    };

    void close_entry(Entry& e) {
        if (e.stat_fd >= 0) {
            ::close(e.stat_fd);
            --open_fds_;
        }
        if (e.statm_fd >= 0) {
            ::close(e.statm_fd);
            --open_fds_;
        }
        e.stat_fd = e.statm_fd = -1;
    }

    // Processes in child cgroups (the session's host/ and work/), depth-limited.
    void collect_children(const std::string& dir, std::vector<uint32_t>& pids, int depth) {
        if (depth >= 8) return;
        DIR* d = ::opendir(dir.c_str());
        if (!d) return;
        std::vector<std::string> subs;
        while (dirent* de = ::readdir(d)) {
            if (de->d_name[0] == '.') continue;
            if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN) continue;
            subs.emplace_back(dir + "/" + de->d_name);
        }
        ::closedir(d);
        std::string text;
        for (const auto& s : subs) {
            if (!read_path(s + "/cgroup.procs", text, 64 * 1024)) continue;
            auto more = procfs::parse_pid_list(text);
            pids.insert(pids.end(), more.begin(), more.end());
            collect_children(s, pids, depth + 1);
        }
    }

    int proc_fd_ = -1;
    uint64_t ns_per_tick_ = 10000000;
    uint64_t page_ = 4096;
    uid_t uid_ = 0;
    size_t fd_cap_ = 2048;
    size_t open_fds_ = 0;
    uint64_t generation_ = 0;
    std::unordered_map<uint32_t, Entry> cache_;
    std::unordered_map<ProcessKey, PssEntry, ProcessKeyHash> pss_;
    std::vector<RawProc> rows_;
    std::string buf_;
    std::optional<std::string> mount_;
};

}  // namespace

std::unique_ptr<ProcessSource> ProcessSource::create(const SourceOptions& opt) {
    return std::make_unique<LinuxSource>(opt);
}

void lower_current_thread_priority() {
    (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 5);
}

}  // namespace kt::resources
