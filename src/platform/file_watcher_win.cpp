// FileWatcher on Windows: ReadDirectoryChangesW on overlapped folder handles, one thread
// waiting on the per-folder events plus a wake-up event (API baseline 14393).
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

#include "platform/file_watcher.h"

namespace kt::platform {
namespace {

constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE;
constexpr DWORD kBufBytes = 32 * 1024;

struct Watch {
    std::filesystem::path dir;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED ov{};
    std::vector<DWORD> buf = std::vector<DWORD>(kBufBytes / sizeof(DWORD));
    bool pending = false;

    bool arm() {
        ov = OVERLAPPED{};
        ov.hEvent = event;
        pending = ::ReadDirectoryChangesW(handle, buf.data(), kBufBytes, FALSE, kFilter, nullptr, &ov, nullptr) != 0;
        return pending;
    }
    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            if (pending) {
                ::CancelIoEx(handle, &ov);
                DWORD n = 0;
                ::GetOverlappedResult(handle, &ov, &n, TRUE);
            }
            ::CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        if (event) ::CloseHandle(event);
        event = nullptr;
    }
};

class RdcwWatcher final : public FileWatcher {
public:
    RdcwWatcher(Callback cb, HANDLE wake) : cb_(std::move(cb)), wake_(wake) {
        thread_ = std::thread([this] { run(); });
    }
    ~RdcwWatcher() override {
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        ::SetEvent(wake_);
        thread_.join();
        for (auto& w : watches_) w->close();
        for (auto& w : retired_) w->close();
        ::CloseHandle(wake_);
    }

    Result<void> add_dir(const std::filesystem::path& dir) override {
        auto w = std::make_unique<Watch>();
        w->dir = dir;
        w->handle = ::CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (w->handle == INVALID_HANDLE_VALUE) return last_os_error("CreateFileW (folder)");
        w->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!w->event) {
            auto err = last_os_error("CreateEventW");
            w->close();
            return err;
        }
        if (!w->arm()) {
            auto err = last_os_error("ReadDirectoryChangesW");
            w->close();
            return err;
        }
        {
            std::lock_guard lock(mu_);
            if (watches_.size() >= MAXIMUM_WAIT_OBJECTS - 1) {
                w->close();
                return make_error(Errc::busy, "too many watched folders");
            }
            watches_.push_back(std::move(w));
        }
        ::SetEvent(wake_);
        return {};
    }

    void remove_dir(const std::filesystem::path& dir) override {
        std::lock_guard lock(mu_);
        for (auto it = watches_.begin(); it != watches_.end(); ++it) {
            if ((*it)->dir == dir) {
                // The thread may be waiting on its event: it closes the handles after waking.
                retired_.push_back(std::move(*it));
                watches_.erase(it);
                break;
            }
        }
        ::SetEvent(wake_);
    }

private:
    void run() {
        for (;;) {
            std::vector<HANDLE> handles{wake_};
            {
                std::lock_guard lock(mu_);
                if (stop_) return;
                for (auto& w : retired_) w->close();
                retired_.clear();
                for (auto& w : watches_) handles.push_back(w->event);
            }
            const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) {
                ::ResetEvent(wake_);
                continue;  // folder set changed or stop
            }
            if (r < WAIT_OBJECT_0 + 1 || r >= WAIT_OBJECT_0 + handles.size()) {
                ::Sleep(50);  // WAIT_FAILED: a handle went away under us; rebuild the set
                continue;
            }
            const HANDLE fired = handles[r - WAIT_OBJECT_0];
            std::vector<FsEvent> out;
            {
                std::lock_guard lock(mu_);
                if (stop_) return;
                for (auto& w : watches_) {
                    if (w->event != fired) continue;
                    collect(*w, out);
                    break;
                }
            }
            if (!out.empty()) cb_(out);
        }
    }

    static void collect(Watch& w, std::vector<FsEvent>& out) {
        DWORD bytes = 0;
        const BOOL ok = ::GetOverlappedResult(w.handle, &w.ov, &bytes, FALSE);
        w.pending = false;
        ::ResetEvent(w.event);
        if (!ok || bytes == 0) {
            // Buffer overflow (bytes == 0) or an error: report a rescan of the folder.
            out.push_back({FsEvent::Kind::overflow, w.dir, {}});
        } else {
            const auto* base = reinterpret_cast<const std::byte*>(w.buf.data());
            for (DWORD off = 0;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);
                const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                const bool removed =
                    info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME;
                out.push_back({removed ? FsEvent::Kind::removed : FsEvent::Kind::changed, w.dir,
                               std::filesystem::path(name)});
                if (info->NextEntryOffset == 0) break;
                off += info->NextEntryOffset;
            }
        }
        if (!w.arm()) out.push_back({FsEvent::Kind::overflow, w.dir, {}});
    }

    Callback cb_;
    HANDLE wake_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Watch>> watches_;
    std::vector<std::unique_ptr<Watch>> retired_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace

Result<std::unique_ptr<FileWatcher>> FileWatcher::create(Callback cb) {
    HANDLE wake = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake) return last_os_error("CreateEventW");
    return std::unique_ptr<FileWatcher>(new RdcwWatcher(std::move(cb), wake));
}

}  // namespace kt::platform
