// FileWatcher on macOS: one FSEvents stream with file-level events on a private serial
// dispatch queue. The stream is rebuilt when the folder set changes.
//
// FSEvents reports canonical paths (/private/var/... for /var/...), so each folder is
// matched by its realpath. Flags of one event may be coalesced (created + modified +
// removed); the current existence of the file decides between changed and removed.
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <climits>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "platform/file_watcher.h"

namespace kt::platform {
namespace {

struct Dir {
    std::filesystem::path user;  // as passed to add_dir
    std::string real;            // realpath, no trailing slash
};

std::string real_path(const std::filesystem::path& p) {
    char buf[PATH_MAX];
    if (::realpath(p.c_str(), buf)) return std::string(buf);
    return p.string();
}

class FsEventsWatcher final : public FileWatcher {
public:
    explicit FsEventsWatcher(Callback cb)
        : cb_(std::move(cb)), queue_(dispatch_queue_create("keepterm.file_watcher", DISPATCH_QUEUE_SERIAL)) {}

    ~FsEventsWatcher() override {
        {
            std::lock_guard lock(mu_);
            stop_stream();
        }
        dispatch_sync_f(queue_, nullptr, [](void*) {});  // drain callbacks already queued
        dispatch_release(queue_);
    }

    Result<void> add_dir(const std::filesystem::path& dir) override {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return make_error(Errc::invalid_argument, "not a folder");
        std::lock_guard lock(mu_);
        for (const auto& d : dirs_)
            if (d.user == dir) return {};
        dirs_.push_back({dir, real_path(dir)});
        return restart_stream();
    }

    void remove_dir(const std::filesystem::path& dir) override {
        std::lock_guard lock(mu_);
        std::erase_if(dirs_, [&](const Dir& d) { return d.user == dir; });
        [[maybe_unused]] auto r = restart_stream();
    }

private:
    static void on_events(ConstFSEventStreamRef, void* info, size_t n, void* paths,
                          const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        static_cast<FsEventsWatcher*>(info)->deliver(n, static_cast<char**>(paths), flags);
    }

    void deliver(size_t n, char** paths, const FSEventStreamEventFlags flags[]) {
        std::vector<FsEvent> out;
        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < n; ++i) {
                const std::string p = paths[i];
                const FSEventStreamEventFlags f = flags[i];
                for (const auto& d : dirs_) {
                    if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                             kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagRootChanged)) {
                        if (p.starts_with(d.real)) out.push_back({FsEvent::Kind::overflow, d.user, {}});
                        continue;
                    }
                    if (p.size() <= d.real.size() + 1 || !p.starts_with(d.real) || p[d.real.size()] != '/') continue;
                    const std::string name = p.substr(d.real.size() + 1);
                    if (name.find('/') != std::string::npos) continue;  // not a direct child
                    if (f & kFSEventStreamEventFlagItemIsDir) continue;
                    std::error_code ec;
                    const bool exists = std::filesystem::exists(p, ec);
                    out.push_back({exists ? FsEvent::Kind::changed : FsEvent::Kind::removed, d.user, name});
                }
            }
        }
        if (!out.empty()) cb_(out);
    }

    void stop_stream() {
        if (!stream_) return;
        FSEventStreamStop(stream_);
        FSEventStreamInvalidate(stream_);
        FSEventStreamRelease(stream_);
        stream_ = nullptr;
    }

    Result<void> restart_stream() {
        stop_stream();
        if (dirs_.empty()) return {};
        CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        for (const auto& d : dirs_) {
            CFStringRef s = CFStringCreateWithCString(nullptr, d.real.c_str(), kCFStringEncodingUTF8);
            if (s) {
                CFArrayAppendValue(arr, s);
                CFRelease(s);
            }
        }
        FSEventStreamContext ctx{};
        ctx.info = this;
        stream_ = FSEventStreamCreate(nullptr, &FsEventsWatcher::on_events, &ctx, arr, kFSEventStreamEventIdSinceNow,
                                      0.03,
                                      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                          kFSEventStreamCreateFlagWatchRoot);
        CFRelease(arr);
        if (!stream_) return make_error(Errc::os_error, "FSEventStreamCreate");
        FSEventStreamSetDispatchQueue(stream_, queue_);
        if (!FSEventStreamStart(stream_)) {
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
            return make_error(Errc::os_error, "FSEventStreamStart");
        }
        return {};
    }

    Callback cb_;
    dispatch_queue_t queue_;
    std::recursive_mutex mu_;
    std::vector<Dir> dirs_;
    FSEventStreamRef stream_ = nullptr;
};

}  // namespace

Result<std::unique_ptr<FileWatcher>> FileWatcher::create(Callback cb) {
    return std::unique_ptr<FileWatcher>(new FsEventsWatcher(std::move(cb)));
}

}  // namespace kt::platform
