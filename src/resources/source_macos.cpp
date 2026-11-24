// macOS process source: one sysctl(KERN_PROC_ALL) per tick, proc_pid_rusage
// for the measured rows. Public interfaces only: sysctl and libproc.
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "resources/source.h"

namespace kt::resources {
namespace {

uint64_t tv_to_us(const timeval& tv) {
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ull + static_cast<uint64_t>(tv.tv_usec);
}

class MacSource final : public ProcessSource {
public:
    explicit MacSource(const SourceOptions&) {
        mach_timebase_info_data_t tb{};
        if (::mach_timebase_info(&tb) == KERN_SUCCESS && tb.denom != 0) {
            numer_ = tb.numer;
            denom_ = tb.denom;
        }
        uid_ = ::getuid();
    }

    OsKind os() const override { return OsKind::MacOS; }
    std::string_view main_metric_name() const override { return "phys_footprint"; }
    std::string_view resident_metric_name() const override { return "rss"; }

    Result<std::span<RawProc>> take() override {
        int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
        size_t len = 0;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (::sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0) return last_os_error("sysctl KERN_PROC_ALL size");
            len += len / 10;
            const size_t count = len / sizeof(kinfo_proc) + 1;
            if (kp_.size() < count) kp_.resize(count);
            len = kp_.size() * sizeof(kinfo_proc);
            if (::sysctl(mib, 3, kp_.data(), &len, nullptr, 0) == 0) {
                ok = true;
            } else if (errno != ENOMEM) {
                return last_os_error("sysctl KERN_PROC_ALL");
            }
        }
        if (!ok) return make_error(Errc::busy, "process table kept growing");
        const size_t n = len / sizeof(kinfo_proc);
        rows_.clear();
        rows_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const kinfo_proc& k = kp_[i];
            RawProc r;
            r.pid = static_cast<uint32_t>(k.kp_proc.p_pid);
            r.ppid = static_cast<uint32_t>(k.kp_eproc.e_ppid);
            r.start_time = tv_to_us(k.kp_proc.p_starttime);
            r.name.assign(k.kp_proc.p_comm, ::strnlen(k.kp_proc.p_comm, sizeof k.kp_proc.p_comm));
            r.pgid = static_cast<int32_t>(k.kp_eproc.e_pgid);
            if (k.kp_eproc.e_tdev != NODEV) r.tty_dev = static_cast<uint64_t>(static_cast<uint32_t>(k.kp_eproc.e_tdev));
            const bool own = k.kp_eproc.e_ucred.cr_uid == uid_;
            r.own_user = own;
            if (own) {
                const pid_t sid = ::getsid(k.kp_proc.p_pid);
                if (sid >= 0) r.sid = static_cast<int32_t>(sid);
            }
            rows_.push_back(std::move(r));
        }
        return std::span<RawProc>(rows_);
    }

    void measure(std::span<RawProc* const> rows, const MeasureOptions&) override {
        for (RawProc* r : rows) {
            rusage_info_v4 ri{};
            if (::proc_pid_rusage(static_cast<int>(r->pid), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0) {
                r->access_denied = errno == EPERM || errno == EACCES;
                continue;
            }
            r->measured = true;
            r->main = ri.ri_phys_footprint;
            r->resident = ri.ri_resident_size;
            r->peak = ri.ri_lifetime_max_phys_footprint;
            r->cpu_ns = mach_to_ns(ri.ri_user_time + ri.ri_system_time);
            proc_taskinfo ti{};
            if (::proc_pidinfo(static_cast<int>(r->pid), PROC_PIDTASKINFO, 0, &ti, sizeof ti) ==
                static_cast<int>(sizeof ti))
                r->threads = static_cast<uint32_t>(ti.pti_threadnum);
        }
    }

    bool refine(std::span<RawProc* const>, std::chrono::microseconds) override { return true; }

    Result<ContainmentReading> read_containment(const Containment&) override {
        return make_error(Errc::not_supported, "macOS has no containment reading");
    }

    ProcessDetails details(const ProcessKey& key) override {
        ProcessDetails d;
        const int pid = static_cast<int>(key.pid);
        proc_bsdinfo bi{};
        if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != static_cast<int>(sizeof bi)) return d;
        const uint64_t start = bi.pbi_start_tvsec * 1000000ull + bi.pbi_start_tvusec;
        if (key.start_time != 0 && start != key.start_time) return d;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (::proc_pidpath(pid, path, sizeof path) > 0) d.path = std::string(path);

        int argmax = 0;
        size_t sz = sizeof argmax;
        int mib_max[2] = {CTL_KERN, KERN_ARGMAX};
        if (::sysctl(mib_max, 2, &argmax, &sz, nullptr, 0) != 0 || argmax <= 0) return d;
        std::vector<char> buf(static_cast<size_t>(argmax));
        int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
        size_t len = buf.size();
        if (::sysctl(mib, 3, buf.data(), &len, nullptr, 0) != 0 || len < sizeof(int)) return d;
        int argc = 0;
        std::memcpy(&argc, buf.data(), sizeof argc);
        size_t p = sizeof(int);
        while (p < len && buf[p] != '\0') ++p;  // exec path
        while (p < len && buf[p] == '\0') ++p;  // padding
        std::vector<std::string> argv;
        for (int i = 0; i < argc && p < len; ++i) {
            const size_t b = p;
            while (p < len && buf[p] != '\0') ++p;
            argv.emplace_back(buf.data() + b, p - b);
            ++p;
        }
        d.argv = std::move(argv);
        return d;
    }

private:
    uint64_t mach_to_ns(uint64_t t) const {
        if (numer_ == denom_) return t;
        return t / denom_ * numer_ + t % denom_ * numer_ / denom_;  // no overflow for t * numer
    }

    uint32_t numer_ = 1, denom_ = 1;
    uid_t uid_ = 0;
    std::vector<kinfo_proc> kp_;
    std::vector<RawProc> rows_;
};

}  // namespace

std::unique_ptr<ProcessSource> ProcessSource::create(const SourceOptions& opt) {
    return std::make_unique<MacSource>(opt);
}

void lower_current_thread_priority() { (void)::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); }

}  // namespace kt::resources
