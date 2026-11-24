// Folder watcher primitive.
//
// Watches folders, not files: editors save by writing a temporary file and renaming it
// over the target, which replaces the inode. Consumers filter by file name and debounce.
//   Windows  ReadDirectoryChangesW (FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NAME), overlapped
//   macOS    FSEvents with file-level events
//   Linux    inotify IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "base/error.h"

namespace kt::platform {

struct FsEvent {
    enum class Kind : std::uint8_t {
        changed,   // created, written, renamed into the folder, or a change the OS did not classify
        removed,   // deleted or renamed out of the folder
        overflow,  // events were lost; rescan everything under `dir`
    };
    Kind kind = Kind::changed;
    std::filesystem::path dir;   // the folder as passed to add_dir()
    std::filesystem::path name;  // file name inside `dir`; empty for overflow
};

class FileWatcher {
public:
    // Runs on the watcher's own thread, never concurrently with itself.
    using Callback = std::function<void(std::span<const FsEvent>)>;

    static Result<std::unique_ptr<FileWatcher>> create(Callback cb);

    virtual ~FileWatcher() = default;
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Watches the direct children of `dir` (not recursive). The folder must exist.
    virtual Result<void> add_dir(const std::filesystem::path& dir) = 0;
    virtual void remove_dir(const std::filesystem::path& dir) = 0;

protected:
    FileWatcher() = default;
};

}  // namespace kt::platform
