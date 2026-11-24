// POSIX implementation of session/os.h (Linux and macOS).

#include "platform/paths.h"
#include "session/os.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#ifdef __APPLE__
#include <libproc.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#else
#include <sys/prctl.h>
#include <sys/statfs.h>
#endif

extern char** environ;

namespace kt::session::os {
namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

Result<void> mkdir_p(const std::string& path) {
    if (path.empty()) return make_error(Errc::invalid_argument, "empty path");
    std::string cur;
    std::size_t i = 0;
    while (i <= path.size()) {
        const std::size_t slash = path.find('/', i);
        cur = path.substr(0, slash == std::string::npos ? path.size() : slash);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
            return last_os_error("mkdir " + cur);
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return {};
}

// The zmx rule: a directory, not a symlink, ours, mode 0700.
Result<void> check_private(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return last_os_error("lstat " + path);
    if (S_ISLNK(st.st_mode)) return make_error(Errc::invalid_argument, path + " is a symlink");
    if (!S_ISDIR(st.st_mode)) return make_error(Errc::invalid_argument, path + " is not a directory");
    if (st.st_uid != ::getuid()) return make_error(Errc::invalid_argument, path + " is owned by another user");
    if ((st.st_mode & 0777) != 0700) return make_error(Errc::invalid_argument, path + " does not have mode 0700");
    return {};
}

[[maybe_unused]] uint64_t fnv1a(const void* data, std::size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ------------------------------------------------------------------------- journal stores

class MappedStore final : public journal::JournalStore {
public:
    MappedStore(int fd, std::byte* base, std::size_t size) : fd_(fd), base_(base), size_(size) {}
    ~MappedStore() override {
        ::munmap(base_, size_);
        ::close(fd_);
    }
    void write(std::size_t offset, std::span<const std::byte> bytes) override {
        if (offset > size_ || bytes.size() > size_ - offset) return;
        std::memcpy(base_ + offset, bytes.data(), bytes.size());
    }
    Result<void> flush() override {
        if (::msync(base_, size_, MS_SYNC) != 0) return last_os_error("msync");
        if (::fsync(fd_) != 0) return last_os_error("fsync");
        return {};
    }
    bool mapped() const override { return true; }

private:
    int fd_;
    std::byte* base_;
    std::size_t size_;
};

// The portable fallback: the same bytes, buffered and written with pwrite every 250 ms.
class FileStore final : public journal::JournalStore {
public:
    explicit FileStore(int fd) : fd_(fd) {}
    ~FileStore() override {
        (void)pump();
        ::close(fd_);
    }
    void write(std::size_t offset, std::span<const std::byte> bytes) override {
        std::lock_guard lk(mu_);
        if (offset < journal::kRingOffset) {
            auto& slot = headers_[offset / journal::kHeaderPage];
            slot.assign(bytes.begin(), bytes.end());
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
        for (const auto& [off, bytes] : ring) {
            if (auto r = pwrite_all(off, bytes); !r) return r;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (headers[i].empty()) continue;
            if (auto r = pwrite_all(i * journal::kHeaderPage, headers[i]); !r) return r;
        }
        return {};
    }
    Result<void> flush() override {
        if (auto r = pump(); !r) return r;
        if (::fsync(fd_) != 0) return last_os_error("fsync");
        return {};
    }
    bool mapped() const override { return false; }

private:
    Result<void> pwrite_all(std::size_t off, std::span<const std::byte> b) {
        std::size_t done = 0;
        while (done < b.size()) {
            const ssize_t n = ::pwrite(fd_, b.data() + done, b.size() - done, static_cast<off_t>(off + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return last_os_error("pwrite journal");
            }
            done += static_cast<std::size_t>(n);
        }
        return {};
    }

    int fd_;
    std::mutex mu_;
    std::vector<std::pair<std::size_t, std::vector<std::byte>>> ring_;
    std::array<std::vector<std::byte>, 2> headers_;
};

bool network_fs(int fd, std::string* name) {
#ifdef __APPLE__
    struct statfs sf{};
    if (::fstatfs(fd, &sf) != 0) return false;
    const std::string t = sf.f_fstypename;
    if (name) *name = t;
    return t == "nfs" || t == "smbfs" || t == "afpfs" || t == "webdav" || t.rfind("fuse", 0) == 0 ||
           t.rfind("macfuse", 0) == 0 || t.rfind("osxfuse", 0) == 0;
#else
    struct statfs sf{};
    if (::fstatfs(fd, &sf) != 0) return false;
    const auto t = static_cast<unsigned long>(sf.f_type);
    if (name) *name = std::to_string(t);
    return t == 0x6969ul || t == 0x517Bul || t == 0xFF534D42ul || t == 0xFE534D42ul || t == 0x65735546ul;
#endif
}

bool allocate(int fd, std::size_t total) {
#ifdef __APPLE__
    fstore_t fs{};
    fs.fst_flags = F_ALLOCATEALL;
    fs.fst_posmode = F_PEOFPOSMODE;
    fs.fst_offset = 0;
    fs.fst_length = static_cast<off_t>(total);
    if (::fcntl(fd, F_PREALLOCATE, &fs) != 0) {
        // Some file systems (for example APFS on a sparse image) do not support it; the
        // ftruncate below still sizes the file.
    }
    return ::ftruncate(fd, static_cast<off_t>(total)) == 0;
#else
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) return false;
    const int r = ::posix_fallocate(fd, 0, static_cast<off_t>(total));
    return r == 0 || r == EOPNOTSUPP || r == EINVAL;
#endif
}

}  // namespace

// ------------------------------------------------------------------------------ folders

std::string profile() { return env("KEEPTERM_PROFILE"); }

namespace {
// The default profile's folders come from platform::paths (the same layout); a named profile
// keeps the "keepterm-<profile>" folders the session layer has always used.
std::string platform_dir(platform::paths::Kind kind) {
    auto d = platform::paths::get(kind, {});
    return d ? platform::paths::to_utf8(*d) : std::string();
}
}  // namespace

Result<Paths> paths() {
    Paths p;
    const std::string prof = profile();
    const std::string suffix = prof.empty() ? std::string() : "-" + prof;
    const std::string uid = std::to_string(::getuid());
    const std::string home = home_dir();

    p.runtime = env("KEEPTERM_RUNTIME_DIR");
    if (p.runtime.empty()) {
#ifdef __APPLE__
        std::string tmp = env("TMPDIR");
        if (tmp.empty()) tmp = "/tmp";
        while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
        p.runtime = tmp + "/keepterm" + suffix + "-" + uid;
        // sun_path holds 104 bytes on macOS.
        if (p.runtime.size() + std::string("/sessions/0123456789abcdef.int.sock").size() >= 104) {
            p.runtime = "/tmp/keepterm" + suffix + "-" + uid;
        }
#else
        const std::string xdg = env("XDG_RUNTIME_DIR");
        if (!xdg.empty() && is_dir(xdg)) {
            p.runtime = xdg + "/keepterm" + suffix;
        } else {
            p.runtime = "/tmp/keepterm" + suffix + "-" + uid;
        }
#endif
    }
    p.run_sessions = p.runtime + "/sessions";

    p.state = env("KEEPTERM_STATE_DIR");
    if (p.state.empty() && prof.empty()) {
        p.state = platform_dir(platform::paths::Kind::state);
        p.logs = platform_dir(platform::paths::Kind::logs);
    }
    if (p.state.empty()) {
#ifdef __APPLE__
        p.state = home + "/Library/Application Support/keepterm" + suffix + "/state";
        p.logs = home + "/Library/Logs/keepterm" + suffix;
#else
        std::string xdg = env("XDG_STATE_HOME");
        if (xdg.empty()) xdg = home + "/.local/state";
        p.state = xdg + "/keepterm" + suffix;
        p.logs = p.state + "/logs";
#endif
    } else if (p.logs.empty()) {
        p.logs = p.state + "/logs";
    }
    p.state_sessions = p.state + "/sessions";

    for (const std::string* d : {&p.runtime, &p.run_sessions, &p.state, &p.state_sessions}) {
        if (auto r = mkdir_p(*d); !r) return Unexpected(r.error());
    }
    // Folders we create ourselves are 0700 already; the checks catch someone else's folder.
    for (const std::string* d : {&p.runtime, &p.run_sessions}) {
        if (auto r = check_private(*d); !r) return Unexpected(r.error());
    }
    ::chmod(p.state_sessions.c_str(), 0700);
    return p;
}

std::string endpoint_name(const Paths& p, const SessionId& id, bool integration) {
    return p.run_sessions + "/" + id.str() + (integration ? ".int.sock" : ".sock");
}

std::string control_endpoint_name(const Paths& p) { return p.runtime + "/control.sock"; }

std::vector<SessionId> list_endpoints(const Paths& p) {
    std::vector<SessionId> out;
    for (const std::string& name : list_dir(p.run_sessions)) {
        if (name.size() != SessionId::kLength + 5 || name.substr(SessionId::kLength) != ".sock") continue;
        if (auto id = SessionId::parse(name.substr(0, SessionId::kLength))) out.push_back(*id);
    }
    return out;
}

// ------------------------------------------------------------------------------ files

std::string join(std::string_view a, std::string_view b) {
    std::string s(a);
    if (!s.empty() && s.back() != '/') s.push_back('/');
    s.append(b);
    return s;
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

Result<std::string> read_file(const std::string& path, std::size_t max_bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return make_error(Errc::unavailable, "no such file", ENOENT);
        return last_os_error("open " + path);
    }
    std::string out;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return last_os_error("read " + path);
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > max_bytes) {
            ::close(fd);
            return make_error(Errc::corrupt, "file too large: " + path);
        }
    }
    ::close(fd);
    return out;
}

Result<void> write_file_atomic(const std::string& path, std::string_view data) {
    static std::atomic<unsigned> counter{0};
    const std::string tmp = path + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(counter++);
    const int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return last_os_error("create " + tmp);
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            auto e = last_os_error("write " + tmp);
            ::close(fd);
            ::unlink(tmp.c_str());
            return e;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        auto e = last_os_error("fsync " + tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        return e;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        auto e = last_os_error("rename " + tmp);
        ::unlink(tmp.c_str());
        return e;
    }
    return {};
}

Result<void> make_private_dir(const std::string& path) {
    if (auto r = mkdir_p(path); !r) return r;
    ::chmod(path.c_str(), 0700);
    return {};
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        out.push_back(n);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

Result<void> remove_file(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return last_os_error("unlink " + path);
    return {};
}

Result<void> rename_file(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) return last_os_error("rename " + from);
    return {};
}

Result<void> remove_tree(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return {};
    if (S_ISDIR(st.st_mode)) {
        for (const std::string& n : list_dir(path)) (void)remove_tree(path + "/" + n);
        if (::rmdir(path.c_str()) != 0) return last_os_error("rmdir " + path);
        return {};
    }
    return remove_file(path);
}

Result<void> fsync_dir(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return last_os_error("open dir");
    const int r = ::fsync(fd);
    ::close(fd);
    if (r != 0) return last_os_error("fsync dir");
    return {};
}

uint64_t unix_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

std::string home_dir() {
    std::string h = env("HOME");
    if (!h.empty()) return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/";
}

Result<std::unique_ptr<journal::JournalStore>> open_journal_store(const std::string& path, std::size_t total,
                                                                  bool force_fallback, std::string* why) {
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return last_os_error("create journal");
    ::fchmod(fd, 0600);
    std::string fsname;
    bool fallback = force_fallback;
    if (fallback && why) *why = "forced";
    if (!fallback && network_fs(fd, &fsname)) {
        fallback = true;
        if (why) *why = "network or FUSE file system (" + fsname + ")";
    }
    if (!allocate(fd, total)) {
        fallback = true;
        if (why) *why = "allocation failed";
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            auto e = last_os_error("size journal");
            ::close(fd);
            return e;
        }
    }
    if (!fallback) {
        void* m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            return std::unique_ptr<journal::JournalStore>(new MappedStore(fd, static_cast<std::byte*>(m), total));
        }
        if (why) *why = "mmap failed";
    }
    return std::unique_ptr<journal::JournalStore>(new FileStore(fd));
}

// ------------------------------------------------------------------------------ processes

uint32_t current_pid() { return static_cast<uint32_t>(::getpid()); }

uint64_t process_start_time(uint32_t pid) {
    if (pid == 0) return 0;
#ifdef __APPLE__
    proc_bsdinfo info{};
    const int n = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof info);
    if (n != static_cast<int>(sizeof info)) return 0;
    return static_cast<uint64_t>(info.pbi_start_tvsec) * 1000000ull + static_cast<uint64_t>(info.pbi_start_tvusec);
#else
    auto stat = read_file("/proc/" + std::to_string(pid) + "/stat", 4096);
    if (!stat) return 0;
    const std::size_t rp = stat->rfind(')');
    if (rp == std::string::npos) return 0;
    // Fields after the command: state is field 3, starttime is field 22.
    std::size_t pos = rp + 2;
    for (int field = 3; field < 22; ++field) {
        pos = stat->find(' ', pos);
        if (pos == std::string::npos) return 0;
        ++pos;
    }
    return std::strtoull(stat->c_str() + pos, nullptr, 10);
#endif
}

bool process_matches(uint32_t pid, uint64_t start_time) {
    const uint64_t st = process_start_time(pid);
    return st != 0 && st == start_time;
}

Result<void> kill_process(uint32_t pid, uint64_t start_time) {
    if (!process_matches(pid, start_time)) return make_error(Errc::unavailable, "process is gone");
    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0) return last_os_error("kill");
    return {};
}

BootId boot_id() {
    BootId b;
#ifdef __APPLE__
    timeval tv{};
    std::size_t len = sizeof tv;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (::sysctl(mib, 2, &tv, &len, nullptr, 0) == 0) {
        const int64_t secs = static_cast<int64_t>(tv.tv_sec);
        const uint64_t h1 = fnv1a(&secs, sizeof secs);
        const uint64_t h2 = fnv1a(&secs, sizeof secs, h1);
        for (int i = 0; i < 8; ++i) {
            b.bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h1 >> (8 * i));
            b.bytes[static_cast<std::size_t>(i + 8)] = static_cast<uint8_t>(h2 >> (8 * i));
        }
    }
    b.exact = false;
#else
    auto s = read_file("/proc/sys/kernel/random/boot_id", 256);
    if (s) {
        std::size_t o = 0;
        int hi = -1;
        for (char c : *s) {
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) continue;
            if (hi < 0) {
                hi = v;
            } else if (o < b.bytes.size()) {
                b.bytes[o++] = static_cast<uint8_t>((hi << 4) | v);
                hi = -1;
            }
        }
        b.exact = o == b.bytes.size();
    }
#endif
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
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    char real[PATH_MAX];
    if (::realpath(buf.c_str(), real)) return real;
    return buf;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

Result<Spawned> spawn_detached(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    if (args.empty()) return make_error(Errc::invalid_argument, "empty argv");
    int ready[2];
    if (::pipe(ready) != 0) return last_os_error("pipe");
    for (int fd : ready) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    // Everything is prepared before fork: after it only async-signal-safe calls.
    std::vector<std::string> a = args;
    bool uses_ready = false;
    for (auto& s : a) {
        if (const auto p = s.find("{ready}"); p != std::string::npos) {
            s.replace(p, 7, std::to_string(ready[1]));
            uses_ready = true;
        }
    }
    std::vector<char*> argv;
    for (auto& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    rlimit rl{};
    int maxfd = 1024;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        maxfd = static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536));
    }
    const int keep = uses_ready ? ready[1] : -1;
    const char* exe = a[0].c_str();

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        return last_os_error("fork");
    }
    if (child == 0) {
        // First child: new session, then the grandchild that becomes the host.
        if (::setsid() < 0) ::_exit(126);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        const pid_t gc = ::fork();
        if (gc != 0) ::_exit(gc < 0 ? 126 : 0);
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, 0);
            ::dup2(null_fd, 1);
            ::dup2(null_fd, 2);
        }
        for (int fd = 3; fd < maxfd; ++fd) {
            if (fd != keep) ::close(fd);
        }
        if (keep >= 0) ::fcntl(keep, F_SETFD, 0);
        ::execve(exe, argv.data(), environ);
        ::_exit(127);
    }
    ::close(ready[1]);
    int st = 0;
    while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {
    }
    Spawned out;
    if (!uses_ready) {
        // A plain background process (another GUI window): nothing to wait for.
        ::close(ready[0]);
        out.ready_line = "OK";
        return out;
    }
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            ::close(ready[0]);
            return make_error(Errc::timeout, "session host did not report ready");
        }
        pollfd pfd{ready[0], POLLIN, 0};
        const int n = ::poll(&pfd, 1, static_cast<int>(left.count()));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) continue;
        char buf[512];
        const ssize_t got = ::read(ready[0], buf, sizeof buf);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        line.append(buf, static_cast<std::size_t>(got));
        if (line.find('\n') != std::string::npos) break;
    }
    ::close(ready[0]);
    if (const auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
    if (line.empty()) return make_error(Errc::unavailable, "session host exited before it was ready");
    out.ready_line = line;
    if (line.rfind("OK ", 0) == 0) out.pid = static_cast<uint32_t>(std::strtoul(line.c_str() + 3, nullptr, 10));
    return out;
}

bool signal_ready(int64_t handle, std::string_view line) {
    if (handle < 0) return true;
    const std::string l = std::string(line) + "\n";
    ssize_t n;
    do n = ::write(static_cast<int>(handle), l.data(), l.size());
    while (n < 0 && errno == EINTR);
    ::close(static_cast<int>(handle));
    return n == static_cast<ssize_t>(l.size());
}

void release_free_memory() {
#if defined(__APPLE__)
    (void)malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
}

bool host_process_setup() {
    ::umask(077);
    ::signal(SIGPIPE, SIG_IGN);
    const std::string log = env("KEEPTERM_HOST_LOG");
    if (!log.empty()) {
        const int fd = ::open(log.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0) {
            ::dup2(fd, 2);
            ::close(fd);
        }
    }
#ifdef __linux__
    return ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0;
#else
    return false;
#endif
}

void watch_shutdown(std::function<void(bool)> cb) {
    sigset_t set;
    sigemptyset(&set);
    for (int s : {SIGTERM, SIGHUP, SIGINT, SIGQUIT}) sigaddset(&set, s);
    ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set, cb = std::move(cb)] {
        for (;;) {
            int sig = 0;
            if (::sigwait(&set, &sig) != 0) continue;
            cb(true);
        }
    }).detach();
}

std::string getenv_utf8(const char* name) { return env(name); }

}  // namespace kt::session::os
