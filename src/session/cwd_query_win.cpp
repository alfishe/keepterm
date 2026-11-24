// Working directory OS query and host facts, Windows. The directory and the
// command line of another process come from its PEB: NtQueryInformationProcess (documented in
// winternl.h, resolved from ntdll at run time so nothing links against ntdll.lib) gives the PEB
// address, ReadProcessMemory reads RTL_USER_PROCESS_PARAMETERS. The layouts of the fields read
// here (PEB.ProcessParameters, CurrentDirectory.DosPath, CommandLine) have been stable since
// Windows XP for both 32- and 64-bit processes. A 64-bit keepterm reads 32-bit (WOW64) targets
// through their 32-bit PEB; a 32-bit keepterm does not read 64-bit targets.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <initializer_list>
#include <cstdint>
#include <optional>
#include <iterator>
#include <string>

#include "session/cwd.h"

namespace kt::session {
namespace {

std::string narrow(const wchar_t* w, int n) {
    if (n <= 0) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, n, out.data(), len, nullptr, nullptr);
    return out;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string computer_name(COMPUTER_NAME_FORMAT fmt) {
    wchar_t buf[256];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (!::GetComputerNameExW(fmt, buf, &n)) return {};
    return lower_ascii(narrow(buf, static_cast<int>(n)));
}

}  // namespace

namespace {

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn nt_query() {
    static const NtQueryInformationProcessFn f = [] {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        FARPROC p = ntdll ? ::GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr;
        return reinterpret_cast<NtQueryInformationProcessFn>(reinterpret_cast<void*>(p));
    }();
    return f;
}

struct BasicInfo {   // PROCESS_BASIC_INFORMATION
    LONG_PTR exit_status;
    PVOID peb;
    ULONG_PTR affinity;
    LONG_PTR base_priority;
    ULONG_PTR pid;
    ULONG_PTR parent_pid;
};

constexpr ULONG kProcessBasicInformation = 0, kProcessWow64Information = 26;

template <class T>
bool read_at(HANDLE h, uintptr_t addr, T& out) {
    SIZE_T n = 0;
    return addr != 0 && ::ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), &out, sizeof out, &n) && n == sizeof out;
}

// A UNICODE_STRING of the target: length in bytes and the buffer address.
std::optional<std::string> read_ustring(HANDLE h, uint16_t len, uintptr_t buf) {
    if (len == 0) return std::string();
    if (buf == 0 || len % 2 != 0) return std::nullopt;
    std::wstring w(len / 2u, L'\0');
    SIZE_T n = 0;
    if (!::ReadProcessMemory(h, reinterpret_cast<LPCVOID>(buf), w.data(), len, &n) || n != len) return std::nullopt;
    return narrow(w.data(), static_cast<int>(w.size()));
}

enum class Param { Cwd, CommandLine };

Result<std::string> read_param(uint32_t pid, Param which) {
    const NtQueryInformationProcessFn q = nt_query();
    if (!q) return make_error(Errc::not_supported, "NtQueryInformationProcess is not available");
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return make_error(Errc::os_error, "OpenProcess", static_cast<int64_t>(::GetLastError()));
    struct Closer {
        HANDLE h;
        ~Closer() { ::CloseHandle(h); }
    } closer{h};
    BOOL self_wow = FALSE, target_wow = FALSE;
    ::IsWow64Process(::GetCurrentProcess(), &self_wow);
    if (!::IsWow64Process(h, &target_wow)) return make_error(Errc::os_error, "IsWow64Process", static_cast<int64_t>(::GetLastError()));
    std::optional<std::string> out;
    // 32-bit layout: PEB.ProcessParameters at 0x10; CurrentDirectory.DosPath at 0x24, CommandLine at 0x40.
    auto read32 = [&](uintptr_t peb) {
        uint32_t params = 0;
        if (!read_at(h, peb + 0x10, params)) return;
        struct {
            uint16_t len, max;
            uint32_t buf;
        } us{};
        if (!read_at(h, uintptr_t{params} + (which == Param::Cwd ? 0x24u : 0x40u), us)) return;
        out = read_ustring(h, us.len, us.buf);
    };
#ifdef _WIN64
    (void)self_wow;
    if (target_wow) {
        ULONG_PTR peb32 = 0;
        if (q(h, kProcessWow64Information, &peb32, sizeof peb32, nullptr) < 0 || !peb32) {
            return make_error(Errc::os_error, "no 32-bit PEB");
        }
        read32(static_cast<uintptr_t>(peb32));
    } else {
        BasicInfo bi{};
        if (q(h, kProcessBasicInformation, &bi, sizeof bi, nullptr) < 0 || !bi.peb) return make_error(Errc::os_error, "no PEB");
        // 64-bit layout: PEB.ProcessParameters at 0x20; CurrentDirectory.DosPath at 0x38, CommandLine at 0x70.
        uint64_t params = 0;
        if (read_at(h, reinterpret_cast<uintptr_t>(bi.peb) + 0x20, params)) {
            struct {
                uint16_t len, max;
                uint32_t pad;
                uint64_t buf;
            } us{};
            if (read_at(h, static_cast<uintptr_t>(params) + (which == Param::Cwd ? 0x38u : 0x70u), us)) {
                out = read_ustring(h, us.len, static_cast<uintptr_t>(us.buf));
            }
        }
    }
#else
    if (self_wow && !target_wow) return make_error(Errc::not_supported, "a 64-bit process from a 32-bit keepterm");
    BasicInfo bi{};
    if (q(h, kProcessBasicInformation, &bi, sizeof bi, nullptr) < 0 || !bi.peb) return make_error(Errc::os_error, "no PEB");
    read32(reinterpret_cast<uintptr_t>(bi.peb));
#endif
    if (!out) return make_error(Errc::os_error, "ReadProcessMemory", static_cast<int64_t>(::GetLastError()));
    return *out;
}

}  // namespace

Result<std::string> query_process_cwd(uint32_t pid) {
    auto r = read_param(pid, Param::Cwd);
    if (!r) return r;
    std::string& p = *r;
    // "C:\dir\" -> "C:\dir"; "C:\" stays.
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (p.empty()) return make_error(Errc::unavailable, "empty directory");
    return r;
}

Result<std::string> query_process_command_line(uint32_t pid) { return read_param(pid, Param::CommandLine); }

bool is_local_host(std::string_view host) {
    if (host.empty()) return true;
    const std::string h = lower_ascii(host);
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;
    for (COMPUTER_NAME_FORMAT f : {ComputerNameDnsHostname, ComputerNameDnsFullyQualified, ComputerNameNetBIOS}) {
        const std::string self = computer_name(f);
        if (!self.empty() && (h == self || h.substr(0, h.find('.')) == self.substr(0, self.find('.')))) return true;
    }
    return false;
}

std::string home_dir() {
    wchar_t buf[32768];
    const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return "C:\\";
    return narrow(buf, static_cast<int>(n));
}

}  // namespace kt::session
