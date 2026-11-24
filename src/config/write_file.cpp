// Atomic file replacement for Settings writes. The only OS calls of
// kt_config's write path live here.
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "config/writer.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kt::config {

Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return last_os_error("cannot create " + display_path(tmp));
    size_t off = 0;
    bool ok = true;
    while (ok && off < text.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(text.size() - off, 1u << 20));
        ok = WriteFile(h, text.data() + off, want, &n, nullptr) != 0;
        off += n;
    }
    ok = ok && FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (!ok) {
        auto e = last_os_error("cannot write " + display_path(tmp));
        DeleteFileW(tmp.c_str());
        return e;
    }
    // An editor may hold the file open without sharing for a moment.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
        const DWORD err = GetLastError();
        if ((err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION) || attempt >= 3) {
            auto e = last_os_error("cannot replace " + display_path(path));
            DeleteFileW(tmp.c_str());
            return e;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    mode_t mode = 0600;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return last_os_error("cannot create " + display_path(tmp));
    size_t off = 0;
    bool ok = true;
    while (ok && off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) off += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    (void)::fchmod(fd, mode);
    ::close(fd);
    if (!ok) {
        auto e = last_os_error("cannot write " + display_path(tmp));
        ::unlink(tmp.c_str());
        return e;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        auto e = last_os_error("cannot replace " + display_path(path));
        ::unlink(tmp.c_str());
        return e;
    }
    return {};
#endif
}

}  // namespace kt::config
