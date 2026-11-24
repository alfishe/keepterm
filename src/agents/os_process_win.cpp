#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "agents/os_access.h"

namespace kt::agents {
namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// FILETIME (100 ns since 1601) to nanoseconds since 1970.
std::uint64_t unix_ns(const FILETIME& ft) {
    constexpr std::uint64_t kEpochDiff100ns = 116444736000000000ULL;
    const std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t < kEpochDiff100ns ? 0 : (t - kEpochDiff100ns) * 100;
}

}  // namespace

bool OsProcessProbe::is_alive(std::uint32_t pid) const {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok != FALSE && code == STILL_ACTIVE;
}

std::optional<std::uint64_t> OsProcessProbe::start_time(std::uint32_t pid) const {
    if (pid == 0) return std::nullopt;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return std::nullopt;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(h, &created, &exited, &kernel, &user);
    CloseHandle(h);
    if (ok == FALSE) return std::nullopt;
    const std::uint64_t t = unix_ns(created);
    return t == 0 ? std::nullopt : std::optional<std::uint64_t>(t);
}

std::string environment_variable(std::string_view name) {
    const std::wstring wname = widen(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    if (got == 0 || got >= n) return {};
    value.resize(got);
    return narrow(value);
}

std::string home_directory() { return environment_variable("USERPROFILE"); }

}  // namespace kt::agents
