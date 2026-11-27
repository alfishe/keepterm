// control::launch_gui (app/control.h): a new GUI process of this executable, detached from the
// caller so it outlives the window that restarts and the pane a CLI runs in.
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "app/control.h"
#include "session/os.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kt::app::control {

#ifdef _WIN32

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument quoted for CommandLineToArgvW.
void append_arg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') cmd.append(slashes * 2 + 1, L'\\');
        else cmd.append(slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

}  // namespace

Result<uint32_t> launch_gui(const std::vector<std::string>& args) {
    const std::string exe = session::os::self_exe();
    if (exe.empty()) return make_error(Errc::unavailable, "cannot find the keepterm executable");
    const std::wstring app = wide(exe);
    std::wstring cmd;
    append_arg(cmd, app);
    for (const std::string& a : args) append_arg(cmd, wide(a));
    STARTUPINFOW si{};   // no STARTF_USESHOWWINDOW: the window shows as the user's default
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Out of the caller's job when it allows that (a terminal's job ends with the terminal).
    const DWORD base = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, base | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             nullptr, &si, &pi);
    if (!ok) ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, base, nullptr, nullptr, &si, &pi);
    if (!ok) return make_error(Errc::os_error, "cannot start keepterm", static_cast<int64_t>(GetLastError()));
    // Let the new window come to the front: the caller has the foreground right now.
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<uint32_t>(pi.dwProcessId);
}

namespace {

// Reads a pipe to its end; keeps the first `max` bytes.
void drain(HANDLE h, std::string& to, std::size_t max, bool& truncated) {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
        const std::size_t room = to.size() < max ? max - to.size() : 0;
        if (n > room) truncated = true;
        to.append(buf, std::min<std::size_t>(n, room));
    }
}

}  // namespace

Result<SelfRun> run_program(const std::string& exe, const std::vector<std::string>& args, int timeout_ms, std::size_t max_bytes) {
    const std::wstring app = wide(exe);
    std::wstring cmd;
    append_arg(cmd, app);
    for (const std::string& a : args) append_arg(cmd, wide(a));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) return make_error(Errc::os_error, "pipe", static_cast<int64_t>(GetLastError()));
    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        CloseHandle(out_r);
        CloseHandle(out_w);
        return make_error(Errc::os_error, "pipe", static_cast<int64_t>(GetLastError()));
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // Only these handles reach the child, not every inheritable handle of the app.
    HANDLE inherit[3] = {out_w, err_w, nul};
    const DWORD count = nul == INVALID_HANDLE_VALUE ? 2 : 3;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attr_mem(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
    const bool listed = InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, count * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul == INVALID_HANDLE_VALUE ? nullptr : nul;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = listed ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD started_error = ok ? 0 : GetLastError();
    if (listed) DeleteProcThreadAttributeList(attrs);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        return make_error(Errc::os_error, "cannot start keepterm", static_cast<int64_t>(started_error));
    }
    CloseHandle(pi.hThread);
    SelfRun r;
    bool trunc_out = false, trunc_err = false;
    std::thread err_reader([&] { drain(err_r, r.err, max_bytes, trunc_err); });
    std::thread out_reader([&] { drain(out_r, r.out, max_bytes, trunc_out); });
    if (WaitForSingleObject(pi.hProcess, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    // The pipes end when the child and anything it started have closed them.
    out_reader.join();
    err_reader.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);
    r.exit_code = static_cast<int>(code);
    r.truncated = trunc_out || trunc_err;
    return r;
}

#else

Result<uint32_t> launch_gui(const std::vector<std::string>& args) {
    std::string exe = session::os::self_exe();
    if (exe.empty()) return make_error(Errc::unavailable, "cannot find the keepterm executable");
    // Everything is built before fork: the child of a threaded process only calls
    // async-signal-safe functions.
    std::vector<std::string> store{exe};
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& s : store) argv.push_back(s.data());
    argv.push_back(nullptr);
    int fds[2];
    if (::pipe(fds) != 0) return make_error(Errc::os_error, "pipe", errno);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return make_error(Errc::os_error, "fork", errno);
    }
    if (child == 0) {
        ::close(fds[0]);
        ::setsid();   // not in the caller's session: a pane's hangup does not reach it
        const pid_t gc = ::fork();
        if (gc != 0) {
            const uint32_t v = gc > 0 ? static_cast<uint32_t>(gc) : 0;
            (void)!::write(fds[1], &v, sizeof v);
            ::_exit(0);
        }
        ::close(fds[1]);
        const int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) {
            ::dup2(nul, 0);
            ::dup2(nul, 1);
            ::dup2(nul, 2);
            if (nul > 2) ::close(nul);
        }
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    uint32_t pid = 0;
    ssize_t n;
    do n = ::read(fds[0], &pid, sizeof pid);
    while (n < 0 && errno == EINTR);
    ::close(fds[0]);
    int st = 0;
    while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {
    }
    if (n != static_cast<ssize_t>(sizeof pid) || pid == 0) return make_error(Errc::os_error, "cannot start keepterm");
    return pid;
}

Result<SelfRun> run_program(const std::string& exe, const std::vector<std::string>& args, int timeout_ms, std::size_t max_bytes) {
    std::vector<std::string> store{exe};
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& s : store) argv.push_back(s.data());
    argv.push_back(nullptr);
    int out[2], err[2];
    if (::pipe(out) != 0) return make_error(Errc::os_error, "pipe", errno);
    if (::pipe(err) != 0) {
        ::close(out[0]);
        ::close(out[1]);
        return make_error(Errc::os_error, "pipe", errno);
    }
    const pid_t child = ::fork();
    if (child < 0) {
        for (int fd : {out[0], out[1], err[0], err[1]}) ::close(fd);
        return make_error(Errc::os_error, "fork", errno);
    }
    if (child == 0) {
        const int nul = ::open("/dev/null", O_RDONLY);
        if (nul >= 0) ::dup2(nul, 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(out[1]);
    ::close(err[1]);
    SelfRun r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    std::string* to[2] = {&r.out, &r.err};
    int open_fds = 2;
    while (open_fds > 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (timeout_ms >= 0 && left <= 0) {
            r.timed_out = true;
            ::kill(child, SIGKILL);
            break;
        }
        const int n = ::poll(fds, 2, timeout_ms < 0 ? -1 : static_cast<int>(left));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[8192];
            const ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
                continue;
            }
            const std::size_t room = to[i]->size() < max_bytes ? max_bytes - to[i]->size() : 0;
            if (static_cast<std::size_t>(got) > room) r.truncated = true;
            to[i]->append(buf, std::min(static_cast<std::size_t>(got), room));
        }
    }
    for (const pollfd& p : fds)
        if (p.fd >= 0) ::close(p.fd);
    int st = 0;
    while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {
    }
    r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return r;
}

#endif

Result<SelfRun> run_self(const std::vector<std::string>& args, int timeout_ms, std::size_t max_bytes) {
    const std::string exe = session::os::self_exe();
    if (exe.empty()) return make_error(Errc::unavailable, "cannot find the keepterm executable");
    return run_program(exe, args, timeout_ms, max_bytes);
}

}  // namespace kt::app::control
