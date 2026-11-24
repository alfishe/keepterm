// control::launch_gui (app/control.h): a new GUI process of this executable, detached from the
// caller so it outlives the window that restarts and the pane a CLI runs in.
#include <string>
#include <vector>

#include "app/control.h"
#include "session/os.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
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

#endif

}  // namespace kt::app::control
