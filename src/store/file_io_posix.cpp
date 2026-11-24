#include "store/file_io.h"

#include <cerrno>
#include <chrono>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/qos.h>
#include <sys/resource.h>
#else
#include <sys/syscall.h>
#endif

namespace kt::store {

namespace {

class PosixFile final : public File {
public:
    explicit PosixFile(int fd) : fd_(fd) {}
    ~PosixFile() override { ::close(fd_); }

    Result<std::size_t> read_at(uint64_t offset, std::span<std::byte> out) override {
        std::size_t done = 0;
        while (done < out.size()) {
            const ssize_t n = ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return last_os_error("pread");
            }
            if (n == 0) break;
            done += static_cast<std::size_t>(n);
        }
        return done;
    }

    Result<void> write_at(uint64_t offset, std::span<const std::byte> bytes) override {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::pwrite(fd_, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return last_os_error("pwrite");
            }
            done += static_cast<std::size_t>(n);
        }
        return {};
    }

    Result<uint64_t> size() override {
        struct stat st {};
        if (::fstat(fd_, &st) != 0) return last_os_error("fstat");
        return static_cast<uint64_t>(st.st_size);
    }

    Result<void> truncate(uint64_t size) override {
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) return last_os_error("ftruncate");
        return {};
    }

    Result<void> sync() override {
#if defined(__APPLE__)
        if (::fsync(fd_) != 0) return last_os_error("fsync");
#else
        if (::fdatasync(fd_) != 0) return last_os_error("fdatasync");
#endif
        return {};
    }

    void drop_cache() override {
#if defined(__linux__)
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
#endif
    }

private:
    int fd_;
};

class PosixLock final : public LockFile {
public:
    explicit PosixLock(int fd) : fd_(fd) {}
    ~PosixLock() override {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }

private:
    int fd_;
};

}  // namespace

Result<std::unique_ptr<File>> File::open(const std::filesystem::path& p, Mode mode) {
    int flags = O_CLOEXEC;
    switch (mode) {
        case Mode::read: flags |= O_RDONLY; break;
        case Mode::write_existing: flags |= O_RDWR; break;
        case Mode::create_new: flags |= O_RDWR | O_CREAT | O_EXCL; break;
    }
    const int fd = ::open(p.c_str(), flags, 0600);
    if (fd < 0) {
        auto e = last_os_error("open " + p.filename().string());
        if (errno == ENOENT) e.error().code = Errc::unavailable;
        return e;
    }
    return std::unique_ptr<File>(new PosixFile(fd));
}

void sync_dir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

Result<std::unique_ptr<LockFile>> LockFile::try_acquire(const std::filesystem::path& p) {
    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return last_os_error("open lock file");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        ::close(fd);
        if (e == EWOULDBLOCK) return std::unique_ptr<LockFile>();
        errno = e;
        return last_os_error("flock");
    }
    return std::unique_ptr<LockFile>(new PosixLock(fd));
}

void lower_thread_priority() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
#else
    // Linux: nice values and IO priorities are per thread.
    const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
    ::setpriority(PRIO_PROCESS, tid, 19);
#if defined(SYS_ioprio_set)
    constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13;
    ::syscall(SYS_ioprio_set, kWhoProcess, static_cast<int>(tid), kClassIdle << kClassShift);
#endif
#endif
}

uint64_t now_unix_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace kt::store
