// PosixPty: posix_openpt + fork/setsid/TIOCSCTTY + exec.

#include "pty/pty.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <spawn.h>
#endif

extern char** environ;

namespace kt::pty {
namespace {

Result<void> set_flags(int fd, int add_fl, bool cloexec) {
    const int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | add_fl) < 0) return last_os_error("fcntl F_SETFL");
    if (cloexec) {
        const int fd_fl = ::fcntl(fd, F_GETFD);
        if (fd_fl < 0 || ::fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0) return last_os_error("fcntl F_SETFD");
    }
    return {};
}

winsize to_winsize(GridSize s) {
    winsize ws{};
    ws.ws_col = s.cols;
    ws.ws_row = s.rows;
    ws.ws_xpixel = s.px_w;
    ws.ws_ypixel = s.px_h;
    return ws;
}

// The environment of the child: ours with the edits applied.
std::vector<std::string> build_env(const std::vector<EnvEdit>& edits) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) env.emplace_back(*e);
    for (const auto& ed : edits) {
        const std::string prefix = ed.name + "=";
        std::erase_if(env, [&](const std::string& s) { return s.compare(0, prefix.size(), prefix) == 0; });
        if (ed.value) env.push_back(prefix + *ed.value);
    }
    return env;
}

class PosixPty final : public IPty {
public:
    PosixPty(int master, pid_t pid, int wake_r, int wake_w, uint64_t tty_dev, const ContainmentRequest& c)
        : master_(master), pid_(pid), wake_r_(wake_r), wake_w_(wake_w) {
        // The host's containment; without one, the descendants are tracked.
        info_.kind = c.kind == ContainmentKind::None ? ContainmentKind::Tracked : c.kind;
        info_.handle = c.handle;
        info_.accuracy = (info_.kind == ContainmentKind::Cgroup || info_.kind == ContainmentKind::CgroupNoMemory)
                             ? Accuracy::Exact
                             : Accuracy::BestEffort;
        info_.root_pid = static_cast<Pid>(pid);
        info_.posix_sid = static_cast<Pid>(pid);
        info_.tty_dev = tty_dev;
    }

    ~PosixPty() override {
        close(std::chrono::milliseconds(500));
        if (wake_r_ >= 0) ::close(wake_r_);
        if (wake_w_ >= 0) ::close(wake_w_);
    }

    Result<ReadResult> read(std::span<std::byte> buf) override {
        ReadResult rr;
        for (;;) {
            if (master_ < 0) {
                rr.eof = true;
                return rr;
            }
            pollfd fds[2] = {{master_, POLLIN, 0}, {wake_r_, POLLIN, 0}};
            const int n = ::poll(fds, 2, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                return last_os_error("poll");
            }
            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (::read(wake_r_, drain, sizeof drain) > 0) {
                }
                rr.woken = true;
                return rr;
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                const ssize_t got = ::read(master_, buf.data(), buf.size());
                if (got > 0) {
                    rr.bytes = static_cast<std::size_t>(got);
                    return rr;
                }
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                // 0 or EIO: the slave side is closed (Linux reports EIO).
                rr.eof = true;
                reap(false);
                return rr;
            }
        }
    }

    Result<std::size_t> write(std::span<const std::byte> bytes) override {
        if (master_ < 0) return make_error(Errc::lost, "pty closed");
        const ssize_t n = ::write(master_, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return std::size_t{0};
            return last_os_error("write pty");
        }
        return static_cast<std::size_t>(n);
    }

    Result<void> resize(GridSize s) override {
        if (master_ < 0) return make_error(Errc::lost, "pty closed");
        const winsize ws = to_winsize(s);
        if (::ioctl(master_, TIOCSWINSZ, &ws) != 0) return last_os_error("TIOCSWINSZ");
        return {};
    }

    Result<void> signal(PtySignal sig) override {
        int s = SIGTERM;
        switch (sig) {
            case PtySignal::Interrupt: s = SIGINT; break;
            case PtySignal::Break: s = SIGQUIT; break;
            case PtySignal::Hangup: s = SIGHUP; break;
            case PtySignal::Terminate: s = SIGTERM; break;
            case PtySignal::Kill: s = SIGKILL; break;
        }
        // To the foreground process group, like the line discipline does.
        pid_t pg = master_ >= 0 ? ::tcgetpgrp(master_) : -1;
        if (pg <= 0) pg = pid_;
        if (::kill(-pg, s) != 0 && ::kill(pid_, s) != 0) return last_os_error("kill");
        // Kill ends the pane: the shell's own group too, not only a job in the foreground.
        if (sig == PtySignal::Kill && pg != pid_) (void)::kill(-pid_, SIGKILL);
        return {};
    }

    void wake() override {
        const char c = 1;
        [[maybe_unused]] const ssize_t n = ::write(wake_w_, &c, 1);
    }

    void close(std::chrono::milliseconds grace) override {
        std::lock_guard lock(close_mutex_);
        if (master_ >= 0) {
            ::close(master_);  // SIGHUP to the session
            master_ = -1;
            wake();
        }
        if (reap(false)) return;
        ::kill(-pid_, SIGHUP);
        const auto deadline = std::chrono::steady_clock::now() + grace;
        while (std::chrono::steady_clock::now() < deadline) {
            if (reap(false)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(-pid_, SIGKILL);
        ::kill(pid_, SIGKILL);
        reap(true);
    }

    std::optional<ExitStatus> exit_status() const override {
        const_cast<PosixPty*>(this)->reap(false);
        std::lock_guard lock(status_mutex_);
        return status_;
    }

    NativeWaitable exit_waitable() const override { return -1; }  // pidfd / kqueue later
    BackendKind kind() const override { return BackendKind::Posix; }
    BackendCaps caps() const override { return expected_caps(BackendKind::Posix); }
    const ContainmentInfo& containment() const override { return info_; }

    Result<std::vector<Pid>> process_ids() const override {
        return std::vector<Pid>{static_cast<Pid>(pid_)};  // the full tree: the resource monitor
    }

    Result<Pid> foreground_pid() const override {
        if (master_ < 0) return make_error(Errc::lost, "pty closed");
        const pid_t pg = ::tcgetpgrp(master_);
        if (pg <= 0) return last_os_error("tcgetpgrp");
        return static_cast<Pid>(pg);
    }

private:
    // Returns true once the root process has been reaped.
    bool reap(bool block) {
        std::lock_guard lock(status_mutex_);
        if (status_) return true;
        int st = 0;
        pid_t r;
        do {
            r = ::waitpid(pid_, &st, block ? 0 : WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r != pid_) return false;
        ExitStatus es;
        if (WIFEXITED(st)) {
            es.code = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            es.signal = WTERMSIG(st);
            es.code = 128 + WTERMSIG(st);
        }
        status_ = es;
        return true;
    }

    int master_;
    pid_t pid_;
    int wake_r_, wake_w_;
    ContainmentInfo info_;
    mutable std::mutex status_mutex_;
    std::optional<ExitStatus> status_;
    std::mutex close_mutex_;
};

}  // namespace

BackendKind resolve_backend(BackendChoice, const ExtendedOptions&) { return BackendKind::Posix; }

BackendCaps expected_caps(BackendKind) { return BackendCaps{0, true, true, true}; }

Result<std::unique_ptr<IPty>> spawn(const SpawnSpec& spec, BackendChoice choice, const ExtendedOptions&) {
    if (choice != BackendChoice::Auto && choice != BackendChoice::Posix) {
        return make_error(Errc::not_supported, "PTY backend not available on POSIX");
    }
    if (spec.argv.empty()) return make_error(Errc::invalid_argument, "empty argv");

    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return last_os_error("posix_openpt");
    auto fail = [&](Unexpected<Error> e) -> Result<std::unique_ptr<IPty>> {
        ::close(master);
        return e;
    };
    if (::grantpt(master) != 0) return fail(last_os_error("grantpt"));
    if (::unlockpt(master) != 0) return fail(last_os_error("unlockpt"));
    char slave_name[128];
    if (::ptsname_r(master, slave_name, sizeof slave_name) != 0) return fail(last_os_error("ptsname_r"));
    if (auto r = set_flags(master, O_NONBLOCK, true); !r) return fail(Unexpected(r.error()));

    struct stat sst{};
    uint64_t tty_dev = 0;
    if (::stat(slave_name, &sst) == 0) tty_dev = static_cast<uint64_t>(sst.st_rdev);

    int wake[2];
    if (::pipe(wake) != 0) return fail(last_os_error("pipe"));
    for (int fd : wake) {
        if (auto r = set_flags(fd, O_NONBLOCK, true); !r) {
            ::close(wake[0]);
            ::close(wake[1]);
            return fail(Unexpected(r.error()));
        }
    }

    // Everything the child needs is prepared before fork: after fork only async-signal-safe calls.
    std::vector<std::string> args = spec.argv;
    std::string program = args[0];
    if (spec.login_shell) {
        const auto slash = args[0].rfind('/');
        args[0] = "-" + (slash == std::string::npos ? args[0] : args[0].substr(slash + 1));
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env = build_env(spec.env_edits);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    // PATH lookup in the parent: execvpe is not portable to macOS.
    std::string resolved = program;
    if (program.find('/') == std::string::npos) {
        const char* path = std::getenv("PATH");
        std::string p = path ? path : "/usr/bin:/bin";
        std::size_t start = 0;
        while (start <= p.size()) {
            const auto colon = p.find(':', start);
            const std::string dir = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            const std::string cand = (dir.empty() ? std::string(".") : dir) + "/" + program;
            if (::access(cand.c_str(), X_OK) == 0) {
                resolved = cand;
                break;
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    const winsize ws = to_winsize(spec.size);
    const char* cwd = spec.cwd.empty() ? nullptr : spec.cwd.c_str();
    const int procs_fd = spec.containment.cgroup_procs_fd;

    // Exec failures are reported through a CLOEXEC pipe: the parent reads errno or EOF.
    int errpipe[2];
    if (::pipe(errpipe) != 0) {
        ::close(wake[0]);
        ::close(wake[1]);
        return fail(last_os_error("pipe"));
    }
    (void)set_flags(errpipe[0], 0, true);
    (void)set_flags(errpipe[1], 0, true);

#ifdef __APPLE__
    // macOS charges a child's network, privacy (TCC) and firewall decisions to its
    // "responsible" process, inherited from the parent: without this, Little Snitch and the
    // privacy prompts see every program in every pane as keepterm. The shell disclaims it and
    // becomes responsible for itself, as in Terminal.app and iTerm2. The call is SPI (not in
    // the SDK), so it is looked up at run time; without it the shell starts as before. The
    // attributes are made here: the child of a threaded process must not allocate.
    using DisclaimFn = int (*)(posix_spawnattr_t*, int);
    static const auto disclaim_fn =
        reinterpret_cast<DisclaimFn>(::dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim"));
    posix_spawnattr_t spawn_attr;
    bool disclaim = disclaim_fn && ::posix_spawnattr_init(&spawn_attr) == 0;
    if (disclaim && (::posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_SETEXEC) != 0 || disclaim_fn(&spawn_attr, 1) != 0)) {
        ::posix_spawnattr_destroy(&spawn_attr);
        disclaim = false;
    }
#endif

    const pid_t pid = ::fork();
#ifdef __APPLE__
    if (disclaim && pid != 0) ::posix_spawnattr_destroy(&spawn_attr);
#endif
    if (pid < 0) {
        for (int fd : {wake[0], wake[1], errpipe[0], errpipe[1]}) ::close(fd);
        return fail(last_os_error("fork"));
    }
    if (pid == 0) {
        auto die = [&](int stage) {
            const int e[2] = {stage, errno};
            [[maybe_unused]] auto n = ::write(errpipe[1], e, sizeof e);
            ::_exit(127);
        };
        ::close(errpipe[0]);
        // Default signal dispositions and an empty mask for the shell.
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int s : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU}) {
            ::signal(s, SIG_DFL);
        }
        if (procs_fd >= 0) {
            // Join the session's work/ cgroup before exec; "0" is this process.
            [[maybe_unused]] auto n = ::write(procs_fd, "0", 1);
        }
        if (::setsid() < 0) die(1);
        const int slave = ::open(slave_name, O_RDWR);
        if (slave < 0) die(2);
        if (::ioctl(slave, TIOCSCTTY, 0) != 0) die(3);
        ::ioctl(slave, TIOCSWINSZ, &ws);
        if (::dup2(slave, 0) < 0 || ::dup2(slave, 1) < 0 || ::dup2(slave, 2) < 0) die(4);
        if (slave > 2) ::close(slave);
        if (cwd && ::chdir(cwd) != 0) die(5);
#ifdef __APPLE__
        if (disclaim) {
            // POSIX_SPAWN_SETEXEC replaces this process like execve; it returns only on failure,
            // and then the plain execve below still runs the shell.
            pid_t self = 0;
            (void)::posix_spawn(&self, resolved.c_str(), nullptr, &spawn_attr, argv.data(), envp.data());
        }
#endif
        ::execve(resolved.c_str(), argv.data(), envp.data());
        die(6);
    }

    ::close(errpipe[1]);
    int child_err[2] = {0, 0};
    ssize_t got;
    do {
        got = ::read(errpipe[0], child_err, sizeof child_err);
    } while (got < 0 && errno == EINTR);
    ::close(errpipe[0]);
    if (got == static_cast<ssize_t>(sizeof child_err)) {
        int st = 0;
        ::waitpid(pid, &st, 0);
        ::close(wake[0]);
        ::close(wake[1]);
        static const char* stages[] = {"", "setsid", "open slave", "TIOCSCTTY", "dup2", "chdir", "exec"};
        const int stage = (child_err[0] >= 1 && child_err[0] <= 6) ? child_err[0] : 6;
        ::close(master);
        return make_error(stage == 6 ? Errc::unavailable : Errc::os_error,
                          std::string("spawn: ") + stages[stage] + ": " + std::strerror(child_err[1]),
                          child_err[1]);
    }

    return std::unique_ptr<IPty>(new PosixPty(master, pid, wake[0], wake[1], tty_dev, spec.containment));
}

}  // namespace kt::pty
