// OS side of the limits probe (agents/limits_probe.h): find the agent's program, run it with a
// timeout and read its standard output.
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "agents/limits_probe.h"
#include "agents/os_access.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kt::agents {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxOutput = 1u << 20;

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string expand_home(const std::string& dir) {
    if (dir == "~" || dir.starts_with("~/")) return home_directory() + dir.substr(1);
    return dir;
}

// The first program of `names` in PATH, then in the known folders; "" when none.
std::string find_program(const LimitsProbe& p) {
#ifdef _WIN32
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::vector<std::string> dirs;
    const std::string path = environment_variable("PATH");
    std::size_t b = 0;
    while (b <= path.size()) {
        std::size_t e = path.find(kSep, b);
        if (e == std::string::npos) e = path.size();
        if (e > b) dirs.push_back(path.substr(b, e - b));
        b = e + 1;
    }
    for (const std::string& d : p.known_dirs) dirs.push_back(expand_home(d));
    for (const std::string& name : p.program_names) {
        for (const std::string& d : dirs) {
            std::error_code ec;
            const fs::path f = from_utf8(d) / from_utf8(name);
            if (fs::is_regular_file(f, ec)) {
#ifndef _WIN32
                if (::access(f.c_str(), X_OK) != 0) continue;
#endif
                return to_utf8(f);
            }
        }
    }
    return {};
}

#if defined(_WIN32)

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument quoted for CommandLineToArgvW (the C runtime's rules).
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

Result<std::string> run(const std::string& program, const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    std::wstring cmd;
    const bool script = program.size() > 4 && (program.ends_with(".cmd") || program.ends_with(".bat"));
    append_arg(cmd, wide(program));
    for (const std::string& a : args) append_arg(cmd, wide(a));
    std::wstring app;
    if (script) {
        // A batch file runs through cmd.exe: /s keeps the inner quotes of the whole line.
        wchar_t sys[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        app = std::wstring(sys, n) + L"\\cmd.exe";
        cmd = L"cmd.exe /d /s /c \"" + cmd + L"\"";
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return make_error(Errc::os_error, "pipe");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    // The whole tree goes when the job closes (a timeout, or claude.cmd's node).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    const std::wstring home = wide(home_directory());
    const BOOL ok = CreateProcessW(script ? app.c_str() : nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                   home.empty() ? nullptr : home.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return make_error(Errc::os_error, "cannot start " + program, static_cast<int64_t>(err));
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0)
            if (out.size() < kMaxOutput) out.append(buf, n);
    });
    const DWORD w = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    if (job) CloseHandle(job);   // ends the children too: the pipe reaches its end
    else TerminateProcess(pi.hProcess, 1);
    reader.join();
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    if (w != WAIT_OBJECT_0) return make_error(Errc::timeout, program + " did not finish in time");
    return out;
}

#else

Result<std::string> run(const std::string& program, const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    int fds[2];
    if (::pipe(fds) != 0) return make_error(Errc::os_error, "pipe", errno);
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    // Its own process group: a timeout ends the whole tree.
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    std::vector<std::string> store{program};
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& s : store) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, program.c_str(), &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return make_error(Errc::os_error, "cannot start " + program, rc);
    }
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int r = ::poll(&p, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        char buf[4096];
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;   // the end: every writer is gone
        if (out.size() < kMaxOutput) out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    if (timed_out) ::kill(-pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) return make_error(Errc::timeout, program + " did not finish in time");
    return out;
}

#endif

}  // namespace

Result<std::vector<RateLimit>> run_limits_probe(const LimitsProbe& p, std::chrono::milliseconds timeout) {
    const std::string program = find_program(p);
    if (program.empty()) return make_error(Errc::unavailable, p.agent + ": the program is not found");
    auto out = run(program, p.args, timeout);
    if (!out) return Unexpected(out.error());
    std::vector<RateLimit> limits = p.parse ? p.parse(*out) : std::vector<RateLimit>{};
    if (limits.empty()) return make_error(Errc::unavailable, p.agent + ": no limits in the output");
    return limits;
}

}  // namespace kt::agents
