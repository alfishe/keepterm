// Windows implementation of session/os.h (baseline 1607 / build 14393; public Win32 API only).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "platform/paths.h"
#include "pty/util_win.h"
#include "session/os.h"

namespace kt::session::os {
namespace {

using pty::win::Handle;
using pty::win::narrow;
using pty::win::widen;
using pty::win::win_error;

std::string env(const char* name) {
    const std::wstring n = widen(name);
    wchar_t buf[32768];
    const DWORD len = ::GetEnvironmentVariableW(n.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return {};
    return narrow(std::wstring_view(buf, len));
}

std::wstring user_sid_string() {
    std::wstring sid;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<std::byte> buf(len ? len : 1);
    if (::GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        LPWSTR str = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) {
            sid = str;
            ::LocalFree(str);
        }
    }
    ::CloseHandle(token);
    return sid;
}

// A protected DACL for the user only (folders of the session module).
class PrivateSa {
public:
    PrivateSa() {
        const std::wstring sid = user_sid_string();
        if (sid.empty()) return;
        const std::wstring sddl = L"D:P(A;OICI;FA;;;" + sid + L")";
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sd_ = nullptr;
            return;
        }
        sa_.nLength = sizeof sa_;
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
    }
    ~PrivateSa() {
        if (sd_) ::LocalFree(sd_);
    }
    PrivateSa(const PrivateSa&) = delete;
    PrivateSa& operator=(const PrivateSa&) = delete;
    SECURITY_ATTRIBUTES* get() { return sd_ ? &sa_ : nullptr; }

private:
    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

Result<void> mkdir_p(const std::string& path) {
    const std::string p = slashes(path);
    PrivateSa sa;
    std::size_t i = 0;
    // Skip the drive or UNC prefix.
    if (p.size() >= 2 && p[1] == ':') i = 3;
    else if (p.rfind("\\\\", 0) == 0) i = p.find('\\', p.find('\\', 2) + 1) + 1;
    for (;;) {
        const std::size_t sl = p.find('\\', i);
        const std::string cur = p.substr(0, sl == std::string::npos ? p.size() : sl);
        if (!cur.empty()) {
            const std::wstring w = widen(cur);
            const DWORD attr = ::GetFileAttributesW(w.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                // Only the folders we create get the private DACL; existing ones keep theirs.
                if (!::CreateDirectoryW(w.c_str(), sa.get()) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
                    return win_error("CreateDirectoryW " + cur);
                }
            }
        }
        if (sl == std::string::npos) break;
        i = sl + 1;
    }
    return {};
}

uint64_t filetime_now_ms() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

// ------------------------------------------------------------------------- journal stores

class MappedStore final : public journal::JournalStore {
public:
    MappedStore(Handle file, Handle mapping, std::byte* base, std::size_t size)
        : file_(std::move(file)), map_(std::move(mapping)), base_(base), size_(size) {}
    ~MappedStore() override { ::UnmapViewOfFile(base_); }
    void write(std::size_t offset, std::span<const std::byte> bytes) override {
        if (offset > size_ || bytes.size() > size_ - offset) return;
        std::memcpy(base_ + offset, bytes.data(), bytes.size());
    }
    Result<void> flush() override {
        if (!::FlushViewOfFile(base_, 0)) return win_error("FlushViewOfFile");
        if (!::FlushFileBuffers(file_.get())) return win_error("FlushFileBuffers");
        return {};
    }
    bool mapped() const override { return true; }

private:
    Handle file_, map_;
    std::byte* base_;
    std::size_t size_;
};

class FileStore final : public journal::JournalStore {
public:
    explicit FileStore(Handle f) : f_(std::move(f)) {}
    ~FileStore() override { (void)pump(); }
    void write(std::size_t offset, std::span<const std::byte> bytes) override {
        std::lock_guard lk(mu_);
        if (offset < journal::kRingOffset) {
            headers_[offset / journal::kHeaderPage].assign(bytes.begin(), bytes.end());
            return;
        }
        if (!ring_.empty() && ring_.back().first + ring_.back().second.size() == offset) {
            ring_.back().second.insert(ring_.back().second.end(), bytes.begin(), bytes.end());
        } else {
            ring_.emplace_back(offset, std::vector<std::byte>(bytes.begin(), bytes.end()));
        }
    }
    Result<void> pump() override {
        std::vector<std::pair<std::size_t, std::vector<std::byte>>> ring;
        std::array<std::vector<std::byte>, 2> headers;
        {
            std::lock_guard lk(mu_);
            ring.swap(ring_);
            headers.swap(headers_);
        }
        for (const auto& [off, b] : ring) {
            if (auto r = write_at(off, b); !r) return r;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!headers[i].empty()) {
                if (auto r = write_at(i * journal::kHeaderPage, headers[i]); !r) return r;
            }
        }
        return {};
    }
    Result<void> flush() override {
        if (auto r = pump(); !r) return r;
        if (!::FlushFileBuffers(f_.get())) return win_error("FlushFileBuffers");
        return {};
    }
    bool mapped() const override { return false; }

private:
    Result<void> write_at(std::size_t off, std::span<const std::byte> b) {
        std::size_t done = 0;
        while (done < b.size()) {
            OVERLAPPED ov{};
            const uint64_t at = off + done;
            ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(b.size() - done, 1u << 20));
            if (!::WriteFile(f_.get(), b.data() + done, want, &n, &ov)) return win_error("WriteFile journal");
            done += n;
        }
        return {};
    }

    Handle f_;
    std::mutex mu_;
    std::vector<std::pair<std::size_t, std::vector<std::byte>>> ring_;
    std::array<std::vector<std::byte>, 2> headers_;
};

bool remote_drive(const std::string& path) {
    std::wstring w = widen(slashes(path));
    if (w.rfind(L"\\\\", 0) == 0) return true;   // UNC
    if (w.size() >= 3 && w[1] == L':') {
        const std::wstring root = w.substr(0, 3);
        return ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
}

std::function<void(bool)>* g_shutdown_cb = nullptr;

LRESULT CALLBACK shutdown_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYENDSESSION:
            if (g_shutdown_cb) (*g_shutdown_cb)(false);   // start saving; never block the logoff
            return TRUE;
        case WM_ENDSESSION:
            if (wp && g_shutdown_cb) (*g_shutdown_cb)(true);
            return 0;
        default: return ::DefWindowProcW(h, msg, wp, lp);
    }
}

}  // namespace

// ------------------------------------------------------------------------------ folders

std::string profile() { return env("KEEPTERM_PROFILE"); }

Result<Paths> paths() {
    Paths p;
    const std::string prof = profile();
    const std::string suffix = prof.empty() ? std::string() : "-" + prof;
    std::string local = env("LOCALAPPDATA");
    if (local.empty()) local = home_dir() + "\\AppData\\Local";
    const std::string base = local + "\\keepterm" + suffix;
    // The default profile's folders come from platform::paths (the same layout under
    // %LOCALAPPDATA%\keepterm); a named profile keeps the "keepterm-<profile>" folders.
    auto platform_dir = [&](platform::paths::Kind kind, const char* leaf) {
        if (prof.empty()) {
            if (auto d = platform::paths::get(kind, {})) return platform::paths::to_utf8(*d);
        }
        return base + "\\" + leaf;
    };
    p.runtime = env("KEEPTERM_RUNTIME_DIR");
    if (p.runtime.empty()) p.runtime = platform_dir(platform::paths::Kind::runtime, "run");
    p.runtime = slashes(p.runtime);
    p.run_sessions = p.runtime + "\\sessions";
    p.state = env("KEEPTERM_STATE_DIR");
    if (p.state.empty()) p.state = platform_dir(platform::paths::Kind::state, "state");
    p.state = slashes(p.state);
    p.state_sessions = p.state + "\\sessions";
    p.logs = platform_dir(platform::paths::Kind::logs, "logs");
    for (const std::string* d : {&p.runtime, &p.run_sessions, &p.state, &p.state_sessions}) {
        if (auto r = mkdir_p(*d); !r) return Unexpected(r.error());
    }
    return p;
}

namespace {
std::string pipe_prefix() {
    const std::string prof = profile();
    return "\\\\.\\pipe\\keepterm-" + pty::win::sid_hash() + (prof.empty() ? std::string() : "-" + prof) + "-";
}
}  // namespace

std::string endpoint_name(const Paths&, const SessionId& id, bool integration) {
    return pipe_prefix() + id.str() + (integration ? "-int" : "");
}

std::string control_endpoint_name(const Paths&) { return pipe_prefix() + "control"; }

std::vector<SessionId> list_endpoints(const Paths&) {
    std::vector<SessionId> out;
    const std::string prefix = pipe_prefix().substr(9);   // without "\\.\pipe\"
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::string n = narrow(fd.cFileName);
        if (n.size() != prefix.size() + SessionId::kLength || n.rfind(prefix, 0) != 0) continue;
        if (auto id = SessionId::parse(n.substr(prefix.size()))) out.push_back(*id);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

// ------------------------------------------------------------------------------ files

std::string join(std::string_view a, std::string_view b) {
    std::string s(a);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    s.append(b);
    return s;
}

bool exists(const std::string& path) { return ::GetFileAttributesW(widen(slashes(path)).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool is_dir(const std::string& path) {
    const DWORD a = ::GetFileAttributesW(widen(slashes(path)).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

Result<std::string> read_file(const std::string& path, std::size_t max_bytes) {
    Handle f(::CreateFileW(widen(slashes(path)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return make_error(Errc::unavailable, "no such file", e);
        return win_error("open " + path, e);
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(f.get(), &size)) return win_error("GetFileSizeEx");
    if (static_cast<uint64_t>(size.QuadPart) > max_bytes) return make_error(Errc::corrupt, "file too large: " + path);
    std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 24));
        if (!::ReadFile(f.get(), out.data() + done, want, &n, nullptr)) return win_error("ReadFile " + path);
        if (n == 0) break;
        done += n;
    }
    out.resize(done);
    return out;
}

Result<void> write_file_atomic(const std::string& path, std::string_view data) {
    static std::atomic<unsigned> counter{0};
    const std::string p = slashes(path);
    const std::string tmp = p + ".tmp-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(counter++);
    {
        Handle f(::CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!f) return win_error("create " + tmp);
        std::size_t done = 0;
        while (done < data.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 24));
            if (!::WriteFile(f.get(), data.data() + done, want, &n, nullptr)) {
                auto e = win_error("write " + tmp);
                f.reset();
                ::DeleteFileW(widen(tmp).c_str());
                return e;
            }
            done += n;
        }
        ::FlushFileBuffers(f.get());
    }
    if (!::MoveFileExW(widen(tmp).c_str(), widen(p).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        auto e = win_error("rename " + tmp);
        ::DeleteFileW(widen(tmp).c_str());
        return e;
    }
    return {};
}

Result<void> make_private_dir(const std::string& path) { return mkdir_p(path); }

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(widen(join(slashes(path), "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::string n = narrow(fd.cFileName);
        if (n == "." || n == "..") continue;
        out.push_back(n);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

Result<void> remove_file(const std::string& path) {
    if (!::DeleteFileW(widen(slashes(path)).c_str())) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return win_error("DeleteFileW " + path, e);
    }
    return {};
}

Result<void> rename_file(const std::string& from, const std::string& to) {
    if (!::MoveFileExW(widen(slashes(from)).c_str(), widen(slashes(to)).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return win_error("MoveFileExW " + from);
    }
    return {};
}

namespace {

// One pass over the tree: every file and folder that can go now. False when something stays.
bool remove_tree_pass(const std::string& p, DWORD& last) {
    const DWORD a = ::GetFileAttributesW(widen(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        bool all = true;
        for (const std::string& n : list_dir(p)) all = remove_tree_pass(join(p, n), last) && all;
        if (::RemoveDirectoryW(widen(p).c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND) return all;
        last = ::GetLastError();
        return false;
    }
    if (a & FILE_ATTRIBUTE_READONLY) ::SetFileAttributesW(widen(p).c_str(), a & ~DWORD{FILE_ATTRIBUTE_READONLY});
    if (::DeleteFileW(widen(p).c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    last = ::GetLastError();
    return false;
}

}  // namespace

Result<void> remove_tree(const std::string& path) {
    // A file still open elsewhere (the journal mapping of a dying host) goes a moment later:
    // whole-tree passes for at most 500 ms, so a file held for good costs that once, at any depth.
    const std::string p = slashes(path);
    DWORD last = 0;
    for (int i = 0; i < 20; ++i) {
        if (remove_tree_pass(p, last)) return {};
        ::Sleep(25);
    }
    return win_error("remove " + p, last);
}

Result<void> fsync_dir(const std::string&) { return {}; }

uint64_t unix_ms() { return filetime_now_ms(); }

std::string home_dir() {
    std::string h = env("USERPROFILE");
    return h.empty() ? std::string("C:\\") : h;
}

Result<std::unique_ptr<journal::JournalStore>> open_journal_store(const std::string& path, std::size_t total,
                                                                  bool force_fallback, std::string* why) {
    Handle f(::CreateFileW(widen(slashes(path)).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return win_error("create journal");
    LARGE_INTEGER sz;
    sz.QuadPart = static_cast<LONGLONG>(total);
    if (!::SetFilePointerEx(f.get(), sz, nullptr, FILE_BEGIN) || !::SetEndOfFile(f.get())) {
        return win_error("size journal");
    }
    bool fallback = force_fallback;
    if (fallback && why) *why = "forced";
    if (!fallback && remote_drive(path)) {
        fallback = true;
        if (why) *why = "network drive";
    }
    if (!fallback) {
        Handle m(::CreateFileMappingW(f.get(), nullptr, PAGE_READWRITE, static_cast<DWORD>(uint64_t{total} >> 32),
                                      static_cast<DWORD>(total & 0xFFFFFFFFu), nullptr));
        if (m) {
            void* base = ::MapViewOfFile(m.get(), FILE_MAP_WRITE, 0, 0, total);
            if (base) {
                return std::unique_ptr<journal::JournalStore>(
                    new MappedStore(std::move(f), std::move(m), static_cast<std::byte*>(base), total));
            }
        }
        if (why) *why = "mapping failed";
    }
    return std::unique_ptr<journal::JournalStore>(new FileStore(std::move(f)));
}

// ------------------------------------------------------------------------------ processes

uint32_t current_pid() { return ::GetCurrentProcessId(); }

uint64_t process_start_time(uint32_t pid) {
    if (pid == 0) return 0;
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!h) return 0;
    // An exited process whose handle someone still holds is not alive.
    if (::WaitForSingleObject(h.get(), 0) == WAIT_OBJECT_0) return 0;
    return pty::win::creation_time(h.get());
}

bool process_matches(uint32_t pid, uint64_t start_time) {
    const uint64_t st = process_start_time(pid);
    return st != 0 && st == start_time;
}

Result<void> kill_process(uint32_t pid, uint64_t start_time) {
    Handle h(::OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!h) return make_error(Errc::unavailable, "process is gone");
    if (pty::win::creation_time(h.get()) != start_time) return make_error(Errc::unavailable, "process is gone");
    if (!::TerminateProcess(h.get(), 1)) return win_error("TerminateProcess");
    ::WaitForSingleObject(h.get(), 2000);
    return {};
}

BootId boot_id() {
    // No public boot id: the boot time, rounded. Never exact; the pid check decides.
    BootId b;
    const uint64_t boot_ms = filetime_now_ms() - ::GetTickCount64();
    const uint64_t rounded = boot_ms / 60000;
    for (int i = 0; i < 8; ++i) b.bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>(rounded >> (8 * i));
    b.exact = false;
    return b;
}

std::string hex(std::span<const uint8_t> bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t v : bytes) {
        s.push_back(d[v >> 4]);
        s.push_back(d[v & 15]);
    }
    return s;
}

std::string self_exe() {
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return narrow(buf);
}

Result<Spawned> spawn_detached(const std::vector<std::string>& args, std::chrono::milliseconds timeout,
                               const std::function<void()>& before_wait) {
    if (args.empty()) return make_error(Errc::invalid_argument, "empty argv");
    Spawned out;
    // The timeout covers the whole start, the wait for the intermediate child included.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool uses_ready = false;
    for (const auto& a : args) uses_ready = uses_ready || a.find("{ready}") != std::string::npos;

    Handle rd, wr;
    if (uses_ready) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE r = nullptr, w = nullptr;
        if (!::CreatePipe(&r, &w, &sa, 0)) return win_error("CreatePipe");
        rd.reset(r);
        wr.reset(w);
        ::SetHandleInformation(rd.get(), HANDLE_FLAG_INHERIT, 0);
    }
    std::vector<std::string> a = args;
    for (auto& s : a) {
        if (const auto p = s.find("{ready}"); p != std::string::npos) {
            s.replace(p, 7, std::to_string(reinterpret_cast<uintptr_t>(wr.get())));
        }
    }
    std::wstring cmd = pty::win::build_command_line(a);

    // Breakaway: only when we are in a job that allows it.
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT;
    BOOL in_job = FALSE;
    if (::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) && in_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        if (::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr)) {
            const DWORD lf = li.BasicLimitInformation.LimitFlags;
            if (lf & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) {
                // children leave the job by themselves
            } else if (lf & JOB_OBJECT_LIMIT_BREAKAWAY_OK) {
                flags |= CREATE_BREAKAWAY_FROM_JOB;
            } else {
                out.may_end_with_gui = true;
            }
        }
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
        return win_error("InitializeProcThreadAttributeList");
    }
    HANDLE inherit[1] = {wr.get()};
    // The ready handle is the only inherited handle; with none, nothing is inherited.
    const bool inherit_handles = uses_ready;
    if (uses_ready &&
        !::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                     nullptr, nullptr)) {
        ::DeleteProcThreadAttributeList(si.lpAttributeList);
        return win_error("UpdateProcThreadAttribute");
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, inherit_handles, flags, nullptr, nullptr,
                               &si.StartupInfo, &pi);
    if (!ok && (flags & CREATE_BREAKAWAY_FROM_JOB) && ::GetLastError() == ERROR_ACCESS_DENIED) {
        flags &= ~DWORD{CREATE_BREAKAWAY_FROM_JOB};
        out.may_end_with_gui = true;
        ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, inherit_handles, flags, nullptr, nullptr,
                              &si.StartupInfo, &pi);
    }
    ::DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) return win_error("CreateProcessW");
    Handle proc(pi.hProcess);
    ::CloseHandle(pi.hThread);
    out.pid = pi.dwProcessId;
    wr.reset();
    if (!uses_ready) {
        out.ready_line = "OK";
        return out;
    }
    if (before_wait) before_wait();
    // Anonymous pipes have no overlapped reads: poll with PeekNamedPipe.
    std::string line;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            // Written off, a line already in the pipe or not: the child ends (one whose line lands
            // between the last look and the close of the pipe would otherwise live on unseen).
            ::TerminateProcess(proc.get(), 1);
            ::WaitForSingleObject(proc.get(), 5000);
            return make_error(Errc::timeout, "session host did not report ready");
        }
        DWORD avail = 0;
        if (!::PeekNamedPipe(rd.get(), nullptr, 0, nullptr, &avail, nullptr)) break;   // writer closed
        if (avail) {
            char buf[512];
            DWORD n = 0;
            if (!::ReadFile(rd.get(), buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) || n == 0) break;
            line.append(buf, n);
            if (line.find('\n') != std::string::npos) break;
            continue;
        }
        ::Sleep(5);
    }
    if (const auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return make_error(Errc::unavailable, "session host exited before it was ready");
    out.ready_line = line;
    return out;
}

bool signal_ready(int64_t handle, std::string_view line) {
    if (handle <= 0) return true;
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    const std::string l = std::string(line) + "\n";
    DWORD n = 0;
    const BOOL ok = ::WriteFile(h, l.data(), static_cast<DWORD>(l.size()), &n, nullptr);
    ::CloseHandle(h);
    return ok && n == l.size();
}

std::size_t LazyPages::page_size() {
    static const std::size_t n = [] {
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        return static_cast<std::size_t>(si.dwPageSize);
    }();
    return n;
}

LazyPages::LazyPages(std::size_t bytes) {
    const std::size_t pg = page_size();
    const std::size_t n = (std::max<std::size_t>(bytes, 1) + pg - 1) / pg * pg;
    // Committed (charged to the commit limit) but not in the working set until written.
    void* m = ::VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m) throw std::bad_alloc();
    p_ = static_cast<std::byte*>(m);
    n_ = n;
}

LazyPages& LazyPages::operator=(LazyPages&& o) noexcept {
    if (this != &o) {
        LazyPages old(std::move(*this));
        p_ = o.p_;
        n_ = o.n_;
        o.p_ = nullptr;
        o.n_ = 0;
    }
    return *this;
}

LazyPages::~LazyPages() {
    if (p_) ::VirtualFree(p_, 0, MEM_RELEASE);
}

void LazyPages::discard(std::size_t off, std::size_t len) {
    const std::size_t pg = page_size();
    if (off >= n_) return;
    len = std::min(len, n_ - off);
    const std::size_t a = (off + pg - 1) / pg * pg, b = (off + len) / pg * pg;
    if (b <= a) return;
    // Windows 8.1 and later: the pages leave the working set and stay committed, so the next
    // write cannot fail for want of commit. Not a fallible decommit and recommit.
    (void)::DiscardVirtualMemory(p_ + a, b - a);
}

std::size_t LazyPages::resident_bytes() const {
    if (!p_) return 0;
    const std::size_t pg = page_size();
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> v(n_ / pg);
    for (std::size_t i = 0; i < v.size(); ++i) v[i].VirtualAddress = p_ + i * pg;
    if (!::QueryWorkingSetEx(::GetCurrentProcess(), v.data(), static_cast<DWORD>(v.size() * sizeof(v[0])))) return 0;
    std::size_t r = 0;
    for (const auto& e : v) r += e.VirtualAttributes.Valid ? pg : 0;
    return r;
}

void release_free_memory() {
    (void)_heapmin();
    (void)HeapCompact(GetProcessHeap(), 0);
}

bool host_process_setup() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Among the last user processes to be asked at shutdown.
    ::SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);
    const std::string log = env("KEEPTERM_HOST_LOG");
    if (!log.empty()) {
        // Shared for reading and appending: several hosts may log to one file and readers may
        // look while they run (_wfreopen_s opens files exclusively).
        HANDLE h = ::CreateFileW(widen(log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILE* f = nullptr;
            if (_fileno(stderr) < 0) (void)::_wfreopen_s(&f, L"NUL", L"w", stderr);
            const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_APPEND);
            if (fd >= 0) {
                if (::_dup2(fd, _fileno(stderr)) == 0) std::setvbuf(stderr, nullptr, _IONBF, 0);
                ::_close(fd);
            } else {
                ::CloseHandle(h);
            }
        }
    }
    return false;
}

void watch_shutdown(std::function<void(bool)> cb) {
    g_shutdown_cb = new std::function<void(bool)>(std::move(cb));
    std::thread([] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = shutdown_wndproc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KeeptermSessionHost";
        ::RegisterClassExW(&wc);
        // A hidden top-level window: message-only windows do not get WM_QUERYENDSESSION.
        HWND w = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!w) return;
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }).detach();
}

std::string getenv_utf8(const char* name) { return env(name); }

}  // namespace kt::session::os
