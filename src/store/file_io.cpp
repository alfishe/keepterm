#include "store/file_io.h"

#include <system_error>

namespace kt::store {

Result<void> File::read_exact(uint64_t offset, std::span<std::byte> out) {
    auto n = read_at(offset, out);
    if (!n) return Unexpected(n.error());
    if (*n != out.size()) return make_error(Errc::corrupt, "short read");
    return {};
}

Result<void> write_file_atomic(const std::filesystem::path& p, std::span<const std::byte> bytes) {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    auto f = File::open(tmp, File::Mode::create_new);
    if (!f) return Unexpected(f.error());
    if (auto r = (*f)->write_at(0, bytes); !r) return r;
    if (auto r = (*f)->sync(); !r) return r;
    f->reset();
    std::filesystem::rename(tmp, p, ec);
    if (ec) return make_error(Errc::os_error, "rename: " + ec.message(), ec.value());
    sync_dir(p.parent_path());
    return {};
}

}  // namespace kt::store
