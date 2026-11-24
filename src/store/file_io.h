// Small file layer of the store: positional reads, appends, truncation, fsync,
// an exclusive lock file, and low-priority background threads.
//
// Windows opens every file with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, so the
// janitor can delete a segment that a reader still has open (the name goes away when the last
// handle closes), as POSIX unlink does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "base/error.h"

namespace kt::store {

class File {
public:
    enum class Mode : uint8_t { read, write_existing, create_new };
    static Result<std::unique_ptr<File>> open(const std::filesystem::path& p, Mode mode);
    virtual ~File() = default;

    virtual Result<std::size_t> read_at(uint64_t offset, std::span<std::byte> out) = 0;  // short at EOF
    virtual Result<void> write_at(uint64_t offset, std::span<const std::byte> bytes) = 0;
    virtual Result<uint64_t> size() = 0;
    virtual Result<void> truncate(uint64_t size) = 0;
    virtual Result<void> sync() = 0;
    // Drops cached pages where the OS allows it (Linux posix_fadvise); benchmarks only.
    virtual void drop_cache() {}

    // Reads exactly out.size() bytes or fails with Errc::corrupt.
    Result<void> read_exact(uint64_t offset, std::span<std::byte> out);
};

// Writes `bytes` to `p` through a temp file, fsync and rename.
Result<void> write_file_atomic(const std::filesystem::path& p, std::span<const std::byte> bytes);
// Fsyncs a folder entry (POSIX; no-op on Windows).
void sync_dir(const std::filesystem::path& dir);

// An exclusive advisory lock on a lock file, held while the object lives (flock / LockFileEx).
class LockFile {
public:
    // nullptr when another process holds it.
    static Result<std::unique_ptr<LockFile>> try_acquire(const std::filesystem::path& p);
    virtual ~LockFile() = default;
};

// Lowers the calling thread's CPU and IO priority (janitor and compaction).
void lower_thread_priority();

// Wall clock, ms since the Unix epoch.
uint64_t now_unix_ms();

}  // namespace kt::store
