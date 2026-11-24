// The file access of the workspace's state files, behind an interface so the core stays free of
// OS calls (platform through interfaces only). The real one (fs_os.cpp, kt_workspace_os)
// uses the session module's atomic write; MemoryFileSystem is for tests and fault injection.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/error.h"

namespace kt::ws {

enum class ReadStatus : std::uint8_t { Ok, Missing, Error };

struct ReadResult {
    ReadStatus status = ReadStatus::Missing;
    std::string data;
    std::string error;   // when status == Error
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual ReadResult read(const std::string& path) = 0;
    // Temp file in the same folder, flush, rename over `path`, fsync the folder (POSIX).
    virtual WsResult<void> write_atomic(const std::string& path, std::string_view data) = 0;
    virtual WsResult<void> rename(const std::string& from, const std::string& to) = 0;
    virtual WsResult<void> remove(const std::string& path) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual std::vector<std::string> list(const std::string& dir) = 0;   // names
    virtual WsResult<void> make_private_dir(const std::string& dir) = 0;
    // Modification time in Unix ms; nullopt when missing.
    virtual std::optional<std::int64_t> mtime_unix_ms(const std::string& path) = 0;
};

// "<dir>/<name>" with '/'.
std::string path_join(std::string_view dir, std::string_view name);

// An in-memory file system. Thread-safe. `fail_*` hooks inject faults.
class MemoryFileSystem final : public IFileSystem {
public:
    ReadResult read(const std::string& path) override;
    WsResult<void> write_atomic(const std::string& path, std::string_view data) override;
    WsResult<void> rename(const std::string& from, const std::string& to) override;
    WsResult<void> remove(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list(const std::string& dir) override;
    WsResult<void> make_private_dir(const std::string& dir) override;
    std::optional<std::int64_t> mtime_unix_ms(const std::string& path) override;

    void put(const std::string& path, std::string data, std::int64_t mtime = 0);
    std::optional<std::string> get(const std::string& path) const;
    std::vector<std::string> paths() const;
    int writes() const;

    // Return true to fail the call with an I/O error.
    std::function<bool(const std::string& path)> fail_write;
    std::function<bool(const std::string& path)> fail_read;
    std::function<bool(const std::string& from)> fail_rename;
    // Called inside write_atomic before the data lands (blocking writes in tests).
    std::function<void(const std::string& path)> on_write;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::pair<std::string, std::int64_t>> files_;
    int writes_ = 0;
};

}  // namespace kt::ws
