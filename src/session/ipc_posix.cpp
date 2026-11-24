// Unix socket transport.

#include "session/ipc.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>
#endif

namespace kt::session::ipc {
namespace {

#ifdef __APPLE__
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

void set_cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

int new_socket() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_cloexec(fd);
#ifdef __APPLE__
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

bool make_addr(const std::string& path, sockaddr_un& a) {
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return false;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// The peer's uid must be ours; returns its pid (0 when unknown).
Result<uint32_t> check_peer(int fd) {
#ifdef __APPLE__
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) return last_os_error("getpeereid");
    if (uid != ::getuid()) return make_error(Errc::invalid_argument, "peer is another user (uid " + std::to_string(uid) + ")");
    pid_t pid = 0;
    socklen_t len = sizeof pid;
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) != 0) pid = 0;
    return static_cast<uint32_t>(pid);
#else
    ucred cr{};
    socklen_t len = sizeof cr;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return last_os_error("SO_PEERCRED");
    if (cr.uid != ::getuid()) return make_error(Errc::invalid_argument, "peer is another user (uid " + std::to_string(cr.uid) + ")");
    return static_cast<uint32_t>(cr.pid);
#endif
}

class SocketStream final : public Stream {
public:
    SocketStream(int fd, uint32_t pid) : fd_(fd), pid_(pid) {}
    ~SocketStream() override { ::close(fd_); }

    Result<std::size_t> read(std::span<std::byte> buf, int timeout_ms) override {
        for (;;) {
            if (shut_) return std::size_t{0};
            if (timeout_ms >= 0) {
                pollfd p{fd_, POLLIN, 0};
                const int n = ::poll(&p, 1, timeout_ms);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return last_os_error("poll");
                }
                if (n == 0) return make_error(Errc::timeout, "read timeout");
            }
            const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == ECONNRESET || errno == ENOTCONN || errno == EBADF) return std::size_t{0};
                return last_os_error("recv");
            }
            return static_cast<std::size_t>(n);
        }
    }

    Result<void> write_all(std::span<const std::byte> bytes) override {
        std::size_t done = 0;
        while (done < bytes.size()) {
            if (shut_) return make_error(Errc::lost, "stream shut down");
            const ssize_t n = ::send(fd_, bytes.data() + done, bytes.size() - done, kSendFlags);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) return make_error(Errc::lost, "peer closed");
                return last_os_error("send");
            }
            done += static_cast<std::size_t>(n);
        }
        return {};
    }

    void shutdown() override {
        if (shut_.exchange(true)) return;
        ::shutdown(fd_, SHUT_RDWR);
    }

    uint32_t peer_pid() const override { return pid_; }

private:
    int fd_;
    uint32_t pid_;
    std::atomic<bool> shut_{false};
};

class SocketListener final : public Listener {
public:
    SocketListener(int fd, std::string path, int wake_r, int wake_w)
        : fd_(fd), path_(std::move(path)), wake_r_(wake_r), wake_w_(wake_w) {}
    ~SocketListener() override {
        close();
        ::close(fd_);
        ::close(wake_r_);
        ::close(wake_w_);
    }

    Result<std::unique_ptr<Stream>> accept() override {
        for (;;) {
            if (closed_) return make_error(Errc::lost, "listener closed");
            pollfd p[2] = {{fd_, POLLIN, 0}, {wake_r_, POLLIN, 0}};
            const int n = ::poll(p, 2, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                return last_os_error("poll");
            }
            if (closed_ || (p[1].revents & POLLIN)) return make_error(Errc::lost, "listener closed");
            if (!(p[0].revents & POLLIN)) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
                return last_os_error("accept");
            }
            set_cloexec(c);
            const int fl = ::fcntl(c, F_GETFL);
            if (fl >= 0) ::fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
#ifdef __APPLE__
            int one = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            auto pid = check_peer(c);
            if (!pid) {
                ::close(c);   // another user: closed before any byte
                continue;
            }
            return std::unique_ptr<Stream>(new SocketStream(c, *pid));
        }
    }

    void close() override {
        if (closed_.exchange(true)) return;
        const char b = 1;
        [[maybe_unused]] const ssize_t n = ::write(wake_w_, &b, 1);
        ::unlink(path_.c_str());
    }

private:
    int fd_;
    std::string path_;
    int wake_r_, wake_w_;
    std::atomic<bool> closed_{false};
};

}  // namespace

Result<std::unique_ptr<Listener>> listen(const std::string& endpoint, unsigned /*max_instances*/) {
    sockaddr_un addr;
    if (!make_addr(endpoint, addr)) return make_error(Errc::invalid_argument, "socket path too long");
    const int fd = new_socket();
    if (fd < 0) return last_os_error("socket");
    const mode_t old = ::umask(077);
    int r = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    if (r != 0 && errno == EADDRINUSE) {
        // A stale socket file (no listener) is unlinked and bind retried once.
        const int probe = new_socket();
        const bool live = probe >= 0 && ::connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
        if (probe >= 0) ::close(probe);
        if (live) {
            ::umask(old);
            ::close(fd);
            return make_error(Errc::busy, "endpoint is in use");
        }
        ::unlink(endpoint.c_str());
        r = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    }
    ::umask(old);
    if (r != 0) {
        auto e = last_os_error("bind");
        ::close(fd);
        return e;
    }
    ::chmod(endpoint.c_str(), 0600);
    if (::listen(fd, 16) != 0) {
        auto e = last_os_error("listen");
        ::close(fd);
        ::unlink(endpoint.c_str());
        return e;
    }
    int wake[2];
    if (::pipe(wake) != 0) {
        auto e = last_os_error("pipe");
        ::close(fd);
        ::unlink(endpoint.c_str());
        return e;
    }
    set_cloexec(wake[0]);
    set_cloexec(wake[1]);
    return std::unique_ptr<Listener>(new SocketListener(fd, endpoint, wake[0], wake[1]));
}

Result<std::unique_ptr<Stream>> connect(const std::string& endpoint, int timeout_ms) {
    sockaddr_un addr;
    if (!make_addr(endpoint, addr)) return make_error(Errc::invalid_argument, "socket path too long");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int fd = new_socket();
        if (fd < 0) return last_os_error("socket");
        // Non-blocking connect so the timeout holds even when the backlog is full.
        const int fl = ::fcntl(fd, F_GETFL);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
        if (r != 0 && (errno == EINPROGRESS || errno == EAGAIN)) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd p{fd, POLLOUT, 0};
            const int n = ::poll(&p, 1, static_cast<int>(std::max<int64_t>(0, left.count())));
            int err = 0;
            socklen_t len = sizeof err;
            if (n > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                r = 0;
            } else {
                errno = n == 0 ? ETIMEDOUT : (err ? err : errno);
                r = -1;
            }
        }
        if (r == 0) {
            ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            auto pid = check_peer(fd);
            if (!pid) {
                ::close(fd);
                return Unexpected(pid.error());
            }
            return std::unique_ptr<Stream>(new SocketStream(fd, *pid));
        }
        const int e = errno;
        ::close(fd);
        if (e == ENOENT || e == ECONNREFUSED) {
            return make_error(Errc::unavailable, "no session host at the endpoint", e);
        }
        if (e == ETIMEDOUT || std::chrono::steady_clock::now() >= deadline) {
            return make_error(Errc::timeout, "connect timeout", e);
        }
        errno = e;
        return last_os_error("connect");
    }
}

}  // namespace kt::session::ipc
