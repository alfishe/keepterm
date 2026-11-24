// Windows process source, the documented path: Toolhelp32 for identity,
// PROCESS_QUERY_LIMITED_INFORMATION handles kept across ticks (creation time, token user),
// GetProcessMemoryInfo / GetProcessTimes for the measured rows, job object readings.
// Only kernel32 / advapi32 functions present in build 14393, plus ntdll's
// NtQuerySystemInformation through GetProcAddress for the private working set where
// PROCESS_MEMORY_COUNTERS_EX2 is missing (14393).
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/source.h"

namespace kt::resources {
namespace {

// PROCESS_MEMORY_COUNTERS_EX2 (documented; accepted by newer Windows 10 builds). Declared here
// under its own name because the 14393 headers may not have it.
struct PmcEx2Layout {
    PROCESS_MEMORY_COUNTERS_EX ex;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

uint64_t ft_to_u64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string narrow(const wchar_t* s, size_t n) {
    if (n == 0) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

Unexpected<Error> win_error(const char* what) {
    return make_error(Errc::os_error, what, static_cast<int64_t>(::GetLastError()));
}

// The token user SID of a process handle, as bytes.
bool token_user_sid(HANDLE process, std::vector<uint8_t>& out) {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &tok)) return false;
    DWORD need = 0;
    ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<uint8_t> buf(need ? need : 1);
    const bool ok = need && ::GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
    ::CloseHandle(tok);
    if (!ok) return false;
    TOKEN_USER tu{};
    std::memcpy(&tu, buf.data(), sizeof tu);
    const DWORD len = ::GetLengthSid(tu.User.Sid);
    out.assign(static_cast<uint8_t*>(tu.User.Sid), static_cast<uint8_t*>(tu.User.Sid) + len);
    return true;
}

// Private working sets of every process from one SystemProcessInformation snapshot: what Task
// Manager shows as "Memory". The field is the first 8 bytes of winternl's Reserved1 (the
// documented layout keeps it opaque but stable since Vista).
using NtQsiFn = LONG(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

NtQsiFn nt_query_system_information() {
    static const NtQsiFn fn = [] {
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        return nt ? reinterpret_cast<NtQsiFn>(reinterpret_cast<void*>(::GetProcAddress(nt, "NtQuerySystemInformation")))
                  : nullptr;
    }();
    return fn;
}

bool private_working_sets(std::vector<uint8_t>& buf, std::unordered_map<uint32_t, uint64_t>& out) {
    out.clear();
    const NtQsiFn fn = nt_query_system_information();
    if (!fn) return false;
    if (buf.size() < 512 * 1024) buf.resize(512 * 1024);
    for (int attempt = 0; attempt < 6; ++attempt) {
        ULONG need = 0;
        const LONG st = fn(SystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &need);
        if (st == static_cast<LONG>(0xC0000004L)) {   // STATUS_INFO_LENGTH_MISMATCH
            buf.resize(std::max<size_t>(buf.size() * 2, static_cast<size_t>(need) + 64 * 1024));
            continue;
        }
        if (st < 0) return false;
        size_t off = 0;
        for (;;) {
            if (off + sizeof(SYSTEM_PROCESS_INFORMATION) > buf.size()) break;
            const uint8_t* e = buf.data() + off;
            int64_t ws_private = 0;
            HANDLE pid = nullptr;
            ULONG next = 0;
            std::memcpy(&ws_private, e + offsetof(SYSTEM_PROCESS_INFORMATION, Reserved1), sizeof ws_private);
            std::memcpy(&pid, e + offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId), sizeof pid);
            std::memcpy(&next, e + offsetof(SYSTEM_PROCESS_INFORMATION, NextEntryOffset), sizeof next);
            if (ws_private > 0) out[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pid))] = static_cast<uint64_t>(ws_private);
            if (next == 0) break;
            off += next;
        }
        return true;
    }
    return false;
}

class WinSource final : public ProcessSource {
public:
    explicit WinSource(const SourceOptions& opt) {
        if (opt.extended_nt_process_info) {
            extended_status_ = "extended.nt_process_info is not implemented yet; using Toolhelp + PSAPI";
        }
        token_user_sid(::GetCurrentProcess(), own_sid_);
        // PROCESS_MEMORY_COUNTERS_EX2 detection, once, on our own process.
        PmcEx2Layout pm{};
        pm.ex.cb = sizeof pm;
        ex2_ = ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),
                                      sizeof pm) &&
               pm.PrivateWorkingSetSize != 0;
    }

    ~WinSource() override {
        for (auto& [pid, h] : held_) ::CloseHandle(h.handle);
        for (auto& [name, h] : jobs_) ::CloseHandle(h);
    }

    OsKind os() const override { return OsKind::Windows; }
    std::string_view main_metric_name() const override { return "private_working_set"; }
    std::string_view resident_metric_name() const override { return "working_set"; }
    std::string_view extended_status() const override { return extended_status_; }

    bool last_take_partial() const override { return partial_; }
    void force_full_next() override { force_full_ = true; }

    Result<std::span<RawProc>> take() override {
        // Toolhelp (like every full enumeration) costs about 15 ms with 850 processes on a
        // Windows 10 desktop. Windows never reparents, so between full snapshots the cached
        // rows stay valid: only exits are checked, through the held handles.
        if (!force_full_ && !rows_.empty() && ticks_since_full_ + 1 < kFullEvery) {
            ++ticks_since_full_;
            partial_ = true;
            size_t keep = 0;
            for (size_t i = 0; i < rows_.size(); ++i) {
                RawProc& r = rows_[i];
                if (auto it = held_.find(r.pid); it != held_.end()) {
                    DWORD code = 0;
                    if (!::GetExitCodeProcess(it->second.handle, &code) || code != STILL_ACTIVE) {
                        ::CloseHandle(it->second.handle);
                        held_.erase(it);
                        continue;
                    }
                }
                RawProc fresh;
                fresh.pid = r.pid;
                fresh.ppid = r.ppid;
                fresh.start_time = r.start_time;
                fresh.name = std::move(r.name);
                fresh.threads = r.threads;  // refreshed at the next full snapshot
                fresh.own_user = r.own_user;
                rows_[keep++] = std::move(fresh);
            }
            rows_.resize(keep);
            return std::span<RawProc>(rows_);
        }
        force_full_ = false;
        partial_ = false;
        ticks_since_full_ = 0;
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return win_error("CreateToolhelp32Snapshot");
        rows_.clear();
        ++generation_;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        for (BOOL more = ::Process32FirstW(snap, &pe); more; more = ::Process32NextW(snap, &pe)) {
            RawProc r;
            r.pid = pe.th32ProcessID;
            r.ppid = pe.th32ParentProcessID;
            r.threads = pe.cntThreads;
            r.name = narrow(pe.szExeFile, ::wcsnlen(pe.szExeFile, MAX_PATH));
            if (Held* h = hold(r.pid, r.ppid, r.name)) {
                r.start_time = h->create_time;
                r.own_user = h->own;
            }
            rows_.push_back(std::move(r));
        }
        ::CloseHandle(snap);
        for (auto it = held_.begin(); it != held_.end();) {
            if (it->second.generation != generation_) {
                ::CloseHandle(it->second.handle);
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = denied_.begin(); it != denied_.end();) {
            it = it->second.generation != generation_ ? denied_.erase(it) : std::next(it);
        }
        return std::span<RawProc>(rows_);
    }

    void measure(std::span<RawProc* const> rows, const MeasureOptions&) override {
        // The figure is physical memory in use (private working set), never the commit charge:
        // a V8 / Bun process commits hundreds of MB it never touches.
        const bool snapshot = !ex2_ && !rows.empty() && private_working_sets(nt_buf_, nt_ws_);
        for (RawProc* r : rows) {
            auto it = held_.find(r->pid);
            if (it == held_.end() || it->second.create_time != r->start_time) {
                r->access_denied = true;
                continue;
            }
            HANDLE h = it->second.handle;
            PmcEx2Layout pm{};
            pm.ex.cb = ex2_ ? sizeof(PmcEx2Layout) : sizeof(PROCESS_MEMORY_COUNTERS_EX);
            if (!::GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), pm.ex.cb)) {
                r->access_denied = true;
                continue;
            }
            r->measured = true;
            r->committed = pm.ex.PrivateUsage;
            r->peak = pm.ex.PeakWorkingSetSize;
            r->resident = pm.ex.WorkingSetSize;
            if (ex2_) {
                r->private_resident = pm.PrivateWorkingSetSize;
            } else if (snapshot) {
                auto ws = nt_ws_.find(r->pid);
                r->private_resident = ws != nt_ws_.end() ? ws->second : 0;
            }
            if (r->private_resident) {
                r->main = r->private_resident;
            } else {
                r->main = pm.ex.WorkingSetSize;   // no private figure: the whole working set
                r->main_estimated = true;
            }
            FILETIME c{}, e{}, k{}, u{};
            if (::GetProcessTimes(h, &c, &e, &k, &u)) r->cpu_ns = (ft_to_u64(k) + ft_to_u64(u)) * 100;
        }
    }

    bool refine(std::span<RawProc* const>, std::chrono::microseconds) override { return true; }

    Result<ContainmentReading> read_containment(const Containment& c) override {
        if (c.kind != ContainmentKind::Job) return make_error(Errc::not_supported, "not a job object");
        HANDLE job = nullptr;
        if (auto it = jobs_.find(c.handle); it != jobs_.end()) {
            job = it->second;
        } else {
            job = ::OpenJobObjectW(JOB_OBJECT_QUERY, FALSE, widen(c.handle).c_str());
            if (!job) return win_error("OpenJobObjectW");
            jobs_.emplace(c.handle, job);
        }
        ContainmentReading rd;
        DWORD cap = 64;
        if (auto it = job_sizes_.find(c.handle); it != job_sizes_.end()) cap = it->second + 64;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::vector<uint8_t> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + cap * sizeof(ULONG_PTR));
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
            if (::QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buf.size()),
                                            nullptr) ||
                ::GetLastError() == ERROR_MORE_DATA)
            {
                if (list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses) {
                    cap = list->NumberOfAssignedProcesses + 64;
                    continue;
                }
                for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
                    rd.pids.push_back(static_cast<uint32_t>(list->ProcessIdList[i]));
                job_sizes_[c.handle] = list->NumberOfProcessIdsInList;
                break;
            }
            return win_error("QueryInformationJobObject(ProcessIdList)");
        }
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        rd.read_at = ft_to_u64(now);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
        if (::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof acc, nullptr)) {
            rd.cpu_ns = static_cast<uint64_t>(acc.TotalUserTime.QuadPart + acc.TotalKernelTime.QuadPart) * 100;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        if (::QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr) &&
            li.PeakJobMemoryUsed != 0)
        {
            rd.peak = li.PeakJobMemoryUsed;
        }
        return rd;
    }

    ProcessDetails details(const ProcessKey& key) override {
        ProcessDetails d;
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, key.pid);
        if (!h) return d;
        FILETIME c{}, e{}, k{}, u{};
        if (::GetProcessTimes(h, &c, &e, &k, &u) && (key.start_time == 0 || ft_to_u64(c) == key.start_time)) {
            std::wstring path(32768, L'\0');
            DWORD len = static_cast<DWORD>(path.size());
            if (::QueryFullProcessImageNameW(h, 0, path.data(), &len)) d.path = narrow(path.data(), len);
        }
        ::CloseHandle(h);
        // The command line of another process needs WMI or the NT process snapshot; not on this path.
        return d;
    }

private:
    struct Held {
        HANDLE handle = nullptr;
        uint64_t create_time = 0;
        std::optional<bool> own;
        uint64_t generation = 0;
    };
    struct Denied {
        uint32_t ppid = 0;
        std::string name;
        uint64_t generation = 0;
    };

    // The held handle of `pid`, opened on first sight. A handle pins the process object, so the
    // PID cannot be reused while it is held; an exit code other than STILL_ACTIVE means it exited.
    Held* hold(uint32_t pid, uint32_t ppid, const std::string& name) {
        if (auto it = held_.find(pid); it != held_.end()) {
            DWORD code = 0;
            if (::GetExitCodeProcess(it->second.handle, &code) && code == STILL_ACTIVE) {
                it->second.generation = generation_;
                return &it->second;
            }
            ::CloseHandle(it->second.handle);
            held_.erase(it);
        }
        if (auto it = denied_.find(pid); it != denied_.end()) {
            if (it->second.ppid == ppid && it->second.name == name) {
                it->second.generation = generation_;
                return nullptr;
            }
            denied_.erase(it);
        }
        HANDLE h = pid == 0 ? nullptr : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        FILETIME c{}, e{}, k{}, u{};
        if (!h || !::GetProcessTimes(h, &c, &e, &k, &u)) {
            if (h) ::CloseHandle(h);
            denied_[pid] = Denied{ppid, name, generation_};
            return nullptr;
        }
        Held held;
        held.handle = h;
        held.create_time = ft_to_u64(c);
        held.generation = generation_;
        std::vector<uint8_t> sid;
        if (!own_sid_.empty() && token_user_sid(h, sid)) held.own = sid == own_sid_;
        return &held_.emplace(pid, held).first->second;
    }

    static constexpr uint32_t kFullEvery = 5;  // a full snapshot every 5th tick (10 s at 2 s)

    std::vector<uint8_t> own_sid_;
    bool ex2_ = false;
    bool partial_ = false, force_full_ = false;
    uint32_t ticks_since_full_ = 0;
    std::string extended_status_;
    uint64_t generation_ = 0;
    std::unordered_map<uint32_t, Held> held_;
    std::unordered_map<uint32_t, Denied> denied_;
    std::unordered_map<std::string, HANDLE> jobs_;
    std::unordered_map<std::string, DWORD> job_sizes_;
    std::vector<RawProc> rows_;
    std::vector<uint8_t> nt_buf_;
    std::unordered_map<uint32_t, uint64_t> nt_ws_;
};

}  // namespace

std::unique_ptr<ProcessSource> ProcessSource::create(const SourceOptions& opt) {
    return std::make_unique<WinSource>(opt);
}

uint64_t current_thread_cpu_ns() {
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetThreadTimes(::GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return (ft_to_u64(k) + ft_to_u64(u)) * 100;
}

void lower_current_thread_priority() { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

uint32_t current_pid() { return ::GetCurrentProcessId(); }

}  // namespace kt::resources
