// Named pipe transport. Public Win32 API, baseline 14393.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>
#include <aclapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "pty/util_win.h"
#include "session/ipc.h"

namespace kt::session::ipc {
namespace {

using pty::win::Handle;
using pty::win::widen;
using pty::win::win_error;

constexpr DWORD kBuffer = 64 * 1024;

std::vector<std::byte> token_user(HANDLE token) {
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<std::byte> buf(len ? len : 1);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
    return buf;
}

const std::vector<std::byte>& own_user() {
    static const std::vector<std::byte> u = [] {
        HANDLE t = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &t)) return std::vector<std::byte>{};
        auto r = token_user(t);
        ::CloseHandle(t);
        return r;
    }();
    return u;
}

PSID sid_of(const std::vector<std::byte>& tu) {
    return tu.empty() ? nullptr : reinterpret_cast<const TOKEN_USER*>(tu.data())->User.Sid;
}

// The pipe was created by our user: its owner is our SID (or the Administrators group when an
// elevated process of ours created it).
Result<void> check_pipe_owner(HANDLE pipe) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return make_error(Errc::invalid_argument, "pipe owner unknown");
    PSID mine = sid_of(own_user());
    bool ok = owner && mine && ::EqualSid(owner, mine);
    if (!ok && owner && ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) {
        BOOL member = FALSE;
        ok = ::CheckTokenMembership(nullptr, owner, &member) && member;
    }
    ::LocalFree(sd);
    if (!ok) return make_error(Errc::invalid_argument, "endpoint of another user");
    return {};
}

// The peer process must run as our user. A process of another logon session of the same user
// (an SSH logon, the desktop) may not be opened from here: its token's default DACL admits its
// own logon session only. Then the pipe vouches instead: a server's pipe DACL admits our user
// alone (PipeSecurity), and a client checks that the pipe it opened is owned by our user.
Result<void> check_peer_pid(uint32_t pid, HANDLE pipe, bool server) {
    if (pid == 0) return make_error(Errc::invalid_argument, "peer pid unknown");
    Handle p(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p && ::GetLastError() == ERROR_ACCESS_DENIED) return server ? Result<void>{} : check_pipe_owner(pipe);
    if (!p) return win_error("OpenProcess peer");
    HANDLE t = nullptr;
    if (!::OpenProcessToken(p.get(), TOKEN_QUERY, &t)) {
        if (::GetLastError() == ERROR_ACCESS_DENIED) return server ? Result<void>{} : check_pipe_owner(pipe);
        return win_error("OpenProcessToken peer");
    }
    Handle token(t);
    const auto theirs = token_user(token.get());
    PSID a = sid_of(theirs), b = sid_of(own_user());
    if (!a || !b || !::EqualSid(a, b)) return make_error(Errc::invalid_argument, "peer is another user");
    return {};
}

// D:P, full access for the user SID only. Remote clients are refused by
// PIPE_REJECT_REMOTE_CLIENTS. There is no deny ACE for NETWORK (NU): a local process of a user who
// logged on over SSH has NU in its token and would be locked out of its own sessions.
class PipeSecurity {
public:
    PipeSecurity() {
        PSID sid = sid_of(own_user());
        LPWSTR s = nullptr;
        if (!sid || !::ConvertSidToStringSidW(sid, &s)) return;
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(s) + L")";
        ::LocalFree(s);
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sd_ = nullptr;
            return;
        }
        sa_.nLength = sizeof sa_;
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
    }
    ~PipeSecurity() {
        if (sd_) ::LocalFree(sd_);
    }
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    SECURITY_ATTRIBUTES* get() { return sd_ ? &sa_ : nullptr; }

private:
    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

// One overlapped operation: wait for its event or the stop event.
// Returns bytes, Errc::lost (stopped / broken), Errc::timeout.
Result<DWORD> finish(HANDLE h, OVERLAPPED& ov, BOOL started, HANDLE stop, int timeout_ms, const char* what) {
    DWORD n = 0;
    if (started) {
        if (!::GetOverlappedResult(h, &ov, &n, FALSE)) return win_error(what);
        return n;
    }
    const DWORD e = ::GetLastError();
    if (e == ERROR_BROKEN_PIPE || e == ERROR_NO_DATA || e == ERROR_PIPE_NOT_CONNECTED) {
        return make_error(Errc::lost, "pipe closed", e);
    }
    if (e != ERROR_IO_PENDING) return win_error(what, e);
    HANDLE evs[2] = {ov.hEvent, stop};
    const DWORD w = ::WaitForMultipleObjects(2, evs, FALSE, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    if (w != WAIT_OBJECT_0) {
        ::CancelIoEx(h, &ov);
        ::GetOverlappedResult(h, &ov, &n, TRUE);   // the OVERLAPPED must outlive the I/O
        if (w == WAIT_TIMEOUT) {
            // The cancel may have raced with completion: keep the data.
            if (n > 0) return n;
            return make_error(Errc::timeout, "timeout");
        }
        return make_error(Errc::lost, "stream shut down");
    }
    if (!::GetOverlappedResult(h, &ov, &n, FALSE)) {
        const DWORD e2 = ::GetLastError();
        if (e2 == ERROR_BROKEN_PIPE || e2 == ERROR_PIPE_NOT_CONNECTED || e2 == ERROR_OPERATION_ABORTED) {
            return make_error(Errc::lost, "pipe closed", e2);
        }
        if (e2 == ERROR_MORE_DATA) return n;   // byte mode: not expected, harmless
        return win_error(what, e2);
    }
    return n;
}

class PipeStream final : public Stream {
public:
    PipeStream(Handle h, uint32_t peer, bool server)
        : h_(std::move(h)), peer_(peer), server_(server),
          stop_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          rd_ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          wr_ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PipeStream() override {
        shutdown();
        std::lock_guard a(rmu_), b(wmu_);
        if (server_) ::DisconnectNamedPipe(h_.get());
    }

    Result<std::size_t> read(std::span<std::byte> buf, int timeout_ms) override {
        std::lock_guard lk(rmu_);
        if (stopped_) return make_error(Errc::lost, "stream shut down");
        OVERLAPPED ov{};
        ov.hEvent = rd_ev_.get();
        ::ResetEvent(ov.hEvent);
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(buf.size(), kBuffer));
        const BOOL ok = ::ReadFile(h_.get(), buf.data(), want, nullptr, &ov);
        auto r = finish(h_.get(), ov, ok, stop_.get(), timeout_ms, "ReadFile pipe");
        if (!r) {
            if (r.error().code == Errc::lost && r.error().os_code == ERROR_BROKEN_PIPE) return std::size_t{0};
            if (r.error().code == Errc::lost && !stopped_) return std::size_t{0};
            return Unexpected(r.error());
        }
        return std::size_t{*r};
    }

    Result<void> write_all(std::span<const std::byte> bytes) override {
        std::lock_guard lk(wmu_);
        std::size_t done = 0;
        while (done < bytes.size()) {
            if (stopped_) return make_error(Errc::lost, "stream shut down");
            OVERLAPPED ov{};
            ov.hEvent = wr_ev_.get();
            ::ResetEvent(ov.hEvent);
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, kBuffer));
            const BOOL ok = ::WriteFile(h_.get(), bytes.data() + done, want, nullptr, &ov);
            auto r = finish(h_.get(), ov, ok, stop_.get(), -1, "WriteFile pipe");
            if (!r) return Unexpected(r.error());
            if (*r == 0) return make_error(Errc::lost, "pipe closed");
            done += *r;
        }
        return {};
    }

    void shutdown() override {
        stopped_ = true;
        ::SetEvent(stop_.get());
    }

    uint32_t peer_pid() const override { return peer_; }

private:
    Handle h_;
    uint32_t peer_;
    bool server_;
    Handle stop_, rd_ev_, wr_ev_;
    std::mutex rmu_, wmu_;
    std::atomic<bool> stopped_{false};
};

class PipeListener final : public Listener {
public:
    PipeListener(std::wstring name, unsigned max) : name_(std::move(name)), max_(max) {}
    ~PipeListener() override { close(); }

    Result<void> open_first() {
        auto h = create(true);
        if (!h) return Unexpected(h.error());
        next_ = std::move(*h);
        return {};
    }

    Result<std::unique_ptr<Stream>> accept() override {
        std::lock_guard lk(mu_);
        for (;;) {
            if (closed_) return make_error(Errc::lost, "listener closed");
            if (!next_) {
                auto h = create(false);
                if (!h) return Unexpected(h.error());
                next_ = std::move(*h);
            }
            Handle ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED ov{};
            ov.hEvent = ev.get();
            const BOOL ok = ::ConnectNamedPipe(next_.get(), &ov);
            bool connected = false;
            if (ok) {
                connected = true;
            } else {
                const DWORD e = ::GetLastError();
                if (e == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (e == ERROR_IO_PENDING) {
                    HANDLE evs[2] = {ev.get(), stop_.get()};
                    const DWORD w = ::WaitForMultipleObjects(2, evs, FALSE, INFINITE);
                    DWORD n = 0;
                    if (w != WAIT_OBJECT_0) {
                        ::CancelIoEx(next_.get(), &ov);
                        ::GetOverlappedResult(next_.get(), &ov, &n, TRUE);
                        return make_error(Errc::lost, "listener closed");
                    }
                    connected = ::GetOverlappedResult(next_.get(), &ov, &n, FALSE) != FALSE;
                } else if (e == ERROR_NO_DATA) {
                    // The client already left: reuse the instance.
                    ::DisconnectNamedPipe(next_.get());
                    continue;
                } else {
                    return win_error("ConnectNamedPipe", e);
                }
            }
            if (!connected) {
                ::DisconnectNamedPipe(next_.get());
                continue;
            }
            Handle h = std::move(next_);
            ULONG pid = 0;
            ::GetNamedPipeClientProcessId(h.get(), &pid);
            if (auto c = check_peer_pid(pid, h.get(), true); !c) {
                ::DisconnectNamedPipe(h.get());
                continue;   // not ours: drop it, keep listening
            }
            return std::unique_ptr<Stream>(new PipeStream(std::move(h), pid, true));
        }
    }

    void close() override {
        closed_ = true;
        ::SetEvent(stop_.get());
    }

private:
    Result<Handle> create(bool first) {
        PipeSecurity sec;
        if (!sec.get()) return make_error(Errc::os_error, "no security descriptor for the pipe");
        const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        HANDLE h = ::CreateNamedPipeW(name_.c_str(), open, mode, max_ ? max_ : PIPE_UNLIMITED_INSTANCES, kBuffer, kBuffer, 0,
                                      sec.get());
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = ::GetLastError();
            if (first && (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY)) {
                return make_error(Errc::busy, "endpoint already in use", e);
            }
            return win_error("CreateNamedPipeW", e);
        }
        return Handle(h);
    }

    std::wstring name_;
    unsigned max_;
    std::mutex mu_;
    Handle next_;
    Handle stop_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> closed_{false};
};

}  // namespace

Result<std::unique_ptr<Listener>> listen(const std::string& endpoint, unsigned max_instances) {
    // A pipe disappears with its last handle: there are no stale endpoints to replace.
    auto l = std::make_unique<PipeListener>(widen(endpoint), max_instances);
    if (auto r = l->open_first(); !r) return Unexpected(r.error());
    return std::unique_ptr<Listener>(std::move(l));
}

Result<std::unique_ptr<Stream>> connect(const std::string& endpoint, int timeout_ms) {
    const std::wstring name = widen(endpoint);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 3600000 : timeout_ms);
    for (;;) {
        // Identification only: the server cannot act as us with our token.
        HANDLE h = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            Handle hh(h);
            ULONG pid = 0;
            ::GetNamedPipeServerProcessId(hh.get(), &pid);
            if (auto c = check_peer_pid(pid, hh.get(), false); !c) return Unexpected(c.error());
            return std::unique_ptr<Stream>(new PipeStream(std::move(hh), pid, false));
        }
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return make_error(Errc::unavailable, "no such endpoint", e);
        }
        if (e != ERROR_PIPE_BUSY) return win_error("CreateFileW pipe", e);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return make_error(Errc::timeout, "endpoint busy");
        ::WaitNamedPipeW(name.c_str(), static_cast<DWORD>(std::min<long long>(left.count(), 1000)));
    }
}

}  // namespace kt::session::ipc
