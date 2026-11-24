// FileWatcher on Linux: inotify, one thread polling the inotify fd and a wake-up eventfd.
#include <cerrno>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "platform/file_watcher.h"

namespace kt::platform {
namespace {

constexpr std::uint32_t kMask =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF |
    IN_ATTRIB | IN_ONLYDIR;

class InotifyWatcher final : public FileWatcher {
public:
    InotifyWatcher(Callback cb, int ifd, int wake) : cb_(std::move(cb)), ifd_(ifd), wake_(wake) {
        thread_ = std::thread([this] { run(); });
    }
    ~InotifyWatcher() override {
        const std::uint64_t one = 1;
        [[maybe_unused]] auto n = ::write(wake_, &one, sizeof one);
        thread_.join();
        ::close(ifd_);
        ::close(wake_);
    }

    Result<void> add_dir(const std::filesystem::path& dir) override {
        const int wd = ::inotify_add_watch(ifd_, dir.c_str(), kMask);
        if (wd < 0) return last_os_error("inotify_add_watch");
        std::lock_guard lock(mu_);
        dirs_[wd] = dir;
        return {};
    }

    void remove_dir(const std::filesystem::path& dir) override {
        std::lock_guard lock(mu_);
        for (auto it = dirs_.begin(); it != dirs_.end(); ++it) {
            if (it->second == dir) {
                ::inotify_rm_watch(ifd_, it->first);
                dirs_.erase(it);
                return;
            }
        }
    }

private:
    void run() {
        alignas(inotify_event) char buf[16 * 1024];
        for (;;) {
            pollfd fds[2] = {{ifd_, POLLIN, 0}, {wake_, POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (fds[1].revents) return;
            if (!(fds[0].revents & POLLIN)) continue;
            const ssize_t n = ::read(ifd_, buf, sizeof buf);
            if (n <= 0) continue;
            std::vector<FsEvent> out;
            {
                std::lock_guard lock(mu_);
                for (ssize_t off = 0; off < n;) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
                    off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
                    if (ev->mask & IN_Q_OVERFLOW) {
                        for (const auto& [wd, d] : dirs_) out.push_back({FsEvent::Kind::overflow, d, {}});
                        continue;
                    }
                    const auto it = dirs_.find(ev->wd);
                    if (it == dirs_.end()) continue;
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        out.push_back({FsEvent::Kind::overflow, it->second, {}});
                        continue;
                    }
                    if (ev->mask & IN_IGNORED) {
                        dirs_.erase(it);
                        continue;
                    }
                    if (ev->len == 0 || (ev->mask & IN_ISDIR)) continue;
                    const auto kind = (ev->mask & (IN_DELETE | IN_MOVED_FROM)) ? FsEvent::Kind::removed
                                                                               : FsEvent::Kind::changed;
                    out.push_back({kind, it->second, std::filesystem::path(ev->name)});
                }
            }
            if (!out.empty()) cb_(out);
        }
    }

    Callback cb_;
    int ifd_, wake_;
    std::mutex mu_;
    std::map<int, std::filesystem::path> dirs_;
    std::thread thread_;
};

}  // namespace

Result<std::unique_ptr<FileWatcher>> FileWatcher::create(Callback cb) {
    const int ifd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return last_os_error("inotify_init1");
    const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
        auto err = last_os_error("eventfd");
        ::close(ifd);
        return err;
    }
    return std::unique_ptr<FileWatcher>(new InotifyWatcher(std::move(cb), ifd, wake));
}

}  // namespace kt::platform
