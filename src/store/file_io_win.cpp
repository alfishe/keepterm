#include "store/file_io.h"

#include <chrono>

#include <windows.h>

namespace kt::store {

namespace {

class WinFile final : public File {
public:
    explicit WinFile(HANDLE h) : h_(h) {}
    ~WinFile() override { ::CloseHandle(h_); }

    Result<std::size_t> read_at(uint64_t offset, std::span<std::byte> out) override {
        std::size_t done = 0;
        while (done < out.size()) {
            OVERLAPPED ov{};
            const uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            const std::size_t want = out.size() - done;
            const DWORD chunk = want > (1u << 30) ? (1u << 30) : static_cast<DWORD>(want);
            DWORD got = 0;
            if (!::ReadFile(h_, out.data() + done, chunk, &got, &ov)) {
                if (::GetLastError() == ERROR_HANDLE_EOF) break;
                return last_os_error("ReadFile");
            }
            if (got == 0) break;
            done += got;
        }
        return done;
    }

    Result<void> write_at(uint64_t offset, std::span<const std::byte> bytes) override {
        std::size_t done = 0;
        while (done < bytes.size()) {
            OVERLAPPED ov{};
            const uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            const std::size_t want = bytes.size() - done;
            const DWORD chunk = want > (1u << 30) ? (1u << 30) : static_cast<DWORD>(want);
            DWORD put = 0;
            if (!::WriteFile(h_, bytes.data() + done, chunk, &put, &ov)) return last_os_error("WriteFile");
            done += put;
        }
        return {};
    }

    Result<uint64_t> size() override {
        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(h_, &li)) return last_os_error("GetFileSizeEx");
        return static_cast<uint64_t>(li.QuadPart);
    }

    Result<void> truncate(uint64_t size) override {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        if (!::SetFileInformationByHandle(h_, FileEndOfFileInfo, &info, sizeof info)) {
            return last_os_error("SetFileInformationByHandle");
        }
        return {};
    }

    Result<void> sync() override {
        if (!::FlushFileBuffers(h_)) return last_os_error("FlushFileBuffers");
        return {};
    }

private:
    HANDLE h_;
};

class WinLock final : public LockFile {
public:
    explicit WinLock(HANDLE h) : h_(h) {}
    ~WinLock() override {
        OVERLAPPED ov{};
        ::UnlockFileEx(h_, 0, 1, 0, &ov);
        ::CloseHandle(h_);
    }

private:
    HANDLE h_;
};

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

}  // namespace

Result<std::unique_ptr<File>> File::open(const std::filesystem::path& p, Mode mode) {
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING;
    if (mode != Mode::read) access |= GENERIC_WRITE;
    if (mode == Mode::create_new) disposition = CREATE_NEW;
    HANDLE h = ::CreateFileW(p.c_str(), access, kShareAll, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        auto e = last_os_error("CreateFileW " + p.filename().string());
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) e.error().code = Errc::unavailable;
        return e;
    }
    return std::unique_ptr<File>(new WinFile(h));
}

void sync_dir(const std::filesystem::path&) {}

Result<std::unique_ptr<LockFile>> LockFile::try_acquire(const std::filesystem::path& p) {
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, kShareAll, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return last_os_error("CreateFileW lock file");
    OVERLAPPED ov{};
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) return std::unique_ptr<LockFile>();
        return make_error(Errc::os_error, "LockFileEx", static_cast<int64_t>(err));
    }
    return std::unique_ptr<LockFile>(new WinLock(h));
}

void lower_thread_priority() { ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); }

uint64_t now_unix_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace kt::store
