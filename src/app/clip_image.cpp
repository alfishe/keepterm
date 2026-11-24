#include "app/clip_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#include <fstream>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "platform/paths.h"

namespace kt::app {

namespace fs = std::filesystem;

namespace {

void prune(const fs::path& dir, int max_age_days, size_t max_files) {
    std::error_code ec;
    struct Item {
        fs::path p;
        fs::file_time_type t;
    };
    std::vector<Item> items;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = platform::paths::to_utf8(p.filename());
        if (!name.starts_with("clip-") || !name.ends_with(".png")) continue;
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const auto t = it->last_write_time(e2);
        if (!e2) items.push_back({p, t});
    }
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24) * max_age_days;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t > b.t; });
    for (size_t i = 0; i < items.size(); ++i) {
        // Room for the new file: keep max_files - 1.
        if (items[i].t < cutoff || i + 1 >= max_files) fs::remove(items[i].p, ec);
    }
}

Result<void> write_private(const fs::path& p, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return make_error(Errc::os_error, "cannot create the image file");
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f.flush()) return make_error(Errc::os_error, "cannot write the image file");
    return {};
#else
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return last_os_error("open");
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            ::unlink(p.c_str());
            return last_os_error("write");
        }
        off += static_cast<size_t>(n);
    }
    if (::close(fd) != 0) return last_os_error("close");
    return {};
#endif
}

}  // namespace

Result<fs::path> save_clip_image(const fs::path& dir, const std::vector<uint8_t>& png, int max_age_days, size_t max_files) {
    if (png.empty()) return make_error(Errc::invalid_argument, "no image");
    if (auto r = platform::paths::make_private_dir(dir); !r) return make_error(r.error().code, r.error().what, r.error().os_code);
    prune(dir, max_age_days, max_files);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    static std::atomic<unsigned> seq{0};
    for (int attempt = 0; attempt < 50; ++attempt) {
        const fs::path p = dir / ("clip-" + std::string(stamp) + "-" + std::to_string(++seq) + ".png");
        std::error_code ec;
        if (fs::exists(p, ec)) continue;
        if (auto w = write_private(p, png); !w) {
            if (w.error().os_code == EEXIST) continue;
            return make_error(w.error().code, w.error().what, w.error().os_code);
        }
        return p;
    }
    return make_error(Errc::busy, "no free image file name");
}

}  // namespace kt::app
