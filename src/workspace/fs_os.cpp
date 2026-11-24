// The real file system for the state files (kt_workspace_os), over the session module's helpers.
#include <chrono>
#include <filesystem>
#include <system_error>

#include "session/os.h"
#include "workspace/runtime.h"

namespace kt::ws {

namespace {

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

WsError io_error(const kt::Error& e, const std::string& path) { return WsError{WsErrc::Io, e.what, path}; }

std::string parent_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

class OsFileSystem final : public IFileSystem {
public:
    ReadResult read(const std::string& path) override {
        if (!session::os::exists(path)) return {ReadStatus::Missing, {}, {}};
        auto r = session::os::read_file(path);
        if (!r) {
            if (!session::os::exists(path)) return {ReadStatus::Missing, {}, {}};
            return {ReadStatus::Error, {}, r.error().what};
        }
        return {ReadStatus::Ok, std::move(*r), {}};
    }

    WsResult<void> write_atomic(const std::string& path, std::string_view data) override {
        if (auto r = session::os::write_file_atomic(path, data); !r) return Unexpected(io_error(r.error(), path));
        (void)session::os::fsync_dir(parent_of(path));
        return {};
    }

    WsResult<void> rename(const std::string& from, const std::string& to) override {
        if (auto r = session::os::rename_file(from, to); !r) return Unexpected(io_error(r.error(), from));
        return {};
    }

    WsResult<void> remove(const std::string& path) override {
        if (auto r = session::os::remove_file(path); !r) return Unexpected(io_error(r.error(), path));
        return {};
    }

    bool exists(const std::string& path) override { return session::os::exists(path); }

    std::vector<std::string> list(const std::string& dir) override { return session::os::list_dir(dir); }

    WsResult<void> make_private_dir(const std::string& dir) override {
        if (auto r = session::os::make_private_dir(dir); !r) return Unexpected(io_error(r.error(), dir));
        return {};
    }

    std::optional<std::int64_t> mtime_unix_ms(const std::string& path) override {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(fs_path(path), ec);
        if (ec) return std::nullopt;
        // file_clock -> system_clock without clock_cast (not in every standard library yet).
        const auto now_file = std::filesystem::file_time_type::clock::now();
        const auto now_sys = std::chrono::system_clock::now();
        const auto sys = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - now_file);
        return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
    }
};

}  // namespace

std::unique_ptr<IFileSystem> make_os_file_system() { return std::make_unique<OsFileSystem>(); }

}  // namespace kt::ws
