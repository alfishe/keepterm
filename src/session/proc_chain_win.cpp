// Process facts on Windows: one Toolhelp snapshot per chain, GetProcessTimes for creation times,
// the command line and the directory from the process's PEB (session/cwd.h), throttled: the
// command line is read once per process, the directory at most every 2 s per process.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "session/cwd.h"
#include "session/proc_chain.h"

namespace kt::session {
namespace {

std::string narrow(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

uint64_t creation_time(uint32_t pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c{}, e{}, k{}, u{};
    uint64_t t = 0;
    if (::GetProcessTimes(h, &c, &e, &k, &u)) t = (uint64_t{c.dwHighDateTime} << 32) | c.dwLowDateTime;
    ::CloseHandle(h);
    return t;
}

struct Row {
    uint32_t ppid = 0;
    std::string image;
};

}  // namespace

ProcReader make_proc_reader() {
    auto table = std::make_shared<std::optional<std::unordered_map<uint32_t, Row>>>();
    return [table](uint32_t pid) -> std::optional<ProcFacts> {
        if (pid == 0) return std::nullopt;
        if (!*table) {
            table->emplace();
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof pe;
                for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
                    (**table)[pe.th32ProcessID] = Row{pe.th32ParentProcessID, image_name(narrow(pe.szExeFile))};
                }
                ::CloseHandle(snap);
            }
        }
        const auto it = (*table)->find(pid);
        if (it == (*table)->end()) return std::nullopt;
        ProcFacts f;
        f.ppid = it->second.ppid;
        f.image = it->second.image;
        f.start = creation_time(pid);
        // FILETIME: 100 ns since 1601-01-01.
        constexpr uint64_t kEpoch = 116444736000000000ull;
        if (f.start > kEpoch) f.start_unix_ns = (f.start - kEpoch) * 100ull;
        return f;
    };
}

namespace {

struct Cached {
    std::chrono::steady_clock::time_point cwd_at{};
    std::string cwd;
    bool have_argv = false;
    std::vector<std::string> argv;
};

std::mutex g_cache_mu;
std::map<std::pair<uint32_t, uint64_t>, Cached> g_cache;   // (pid, creation time)

Cached& cache_of(uint32_t pid) {
    const std::pair<uint32_t, uint64_t> key{pid, creation_time(pid)};
    if (g_cache.size() > 512) {
        for (auto it = g_cache.begin(); it != g_cache.end();) {
            it = creation_time(it->first.first) == it->first.second ? std::next(it) : g_cache.erase(it);
        }
        if (g_cache.size() > 512) g_cache.clear();
    }
    return g_cache[key];
}

std::vector<std::string> split_command_line(const std::string& utf8) {
    std::vector<std::string> out;
    if (utf8.empty()) return out;
    const int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wn);
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(w.c_str(), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out.push_back(narrow(argv[i]));
    ::LocalFree(argv);
    return out;
}

}  // namespace

std::vector<std::string> process_argv(uint32_t pid) {
    if (pid == 0) return {};
    std::lock_guard lk(g_cache_mu);
    Cached& c = cache_of(pid);
    if (!c.have_argv) {
        c.have_argv = true;
        if (auto cl = query_process_command_line(pid)) c.argv = split_command_line(*cl);
    }
    return c.argv;
}

std::string process_cwd(uint32_t pid) {
    if (pid == 0) return {};
    std::lock_guard lk(g_cache_mu);
    Cached& c = cache_of(pid);
    const auto now = std::chrono::steady_clock::now();
    if (c.cwd_at == std::chrono::steady_clock::time_point{} || now - c.cwd_at >= std::chrono::seconds(2)) {
        c.cwd_at = now;
        if (auto d = query_process_cwd(pid)) c.cwd = *d;
    }
    return c.cwd;
}

}  // namespace kt::session
