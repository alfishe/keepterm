#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "agents/os_access.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace kt::agents {
namespace {

namespace fs = std::filesystem;

fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string from_path(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// file_time_type has no portable epoch in C++20 libraries without clock_cast: map it through the
// difference of the two clocks' current times.
std::int64_t to_unix_ns(fs::file_time_type t) {
    const auto file_now = fs::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    const auto sys = sys_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - file_now);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
}

}  // namespace

std::optional<std::string> StdFileSystem::read_file(const std::string& path, std::size_t max_bytes) const {
    std::error_code ec;
    const fs::path p = to_path(path);
    const auto size = fs::file_size(p, ec);
    if (ec || size > max_bytes) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string out(static_cast<std::size_t>(size), '\0');
    in.read(out.data(), static_cast<std::streamsize>(size));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

std::optional<std::vector<DirEntry>> StdFileSystem::list_dir(const std::string& path) const {
    std::error_code ec;
    fs::directory_iterator it(to_path(path), fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::nullopt;
    std::vector<DirEntry> out;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        DirEntry e;
        e.name = from_path(it->path().filename());
        std::error_code ec2;
        e.is_dir = it->is_directory(ec2);
        const auto t = it->last_write_time(ec2);
        if (!ec2) e.mtime_ns = to_unix_ns(t);
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<std::int64_t> StdFileSystem::mtime_ns(const std::string& path) const {
    std::error_code ec;
    const auto t = fs::last_write_time(to_path(path), ec);
    if (ec) return std::nullopt;
    return to_unix_ns(t);
}

std::optional<IFileSystem::FileStat> StdFileSystem::stat(const std::string& path) const {
    std::error_code ec;
    const fs::path p = to_path(path);
    FileStat st;
    st.size = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    st.mtime_ns = to_unix_ns(t);
#ifndef _WIN32
    struct ::stat sb {};
    if (::stat(path.c_str(), &sb) == 0) {
        st.id = (static_cast<std::uint64_t>(sb.st_dev) << 40) ^ static_cast<std::uint64_t>(sb.st_ino);
    }
#endif
    return st;
}

std::optional<std::string> StdFileSystem::read_range(const std::string& path, std::uint64_t offset,
                                                     std::size_t max_bytes) const {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto end = static_cast<std::uint64_t>(std::max<std::streamoff>(0, in.tellg()));
    if (offset >= end) return std::string();
    const std::uint64_t n = std::min<std::uint64_t>(end - offset, max_bytes);
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string out(static_cast<std::size_t>(n), '\0');
    in.read(out.data(), static_cast<std::streamsize>(n));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

SourcePaths default_source_paths(std::string_view configured_claude_dir) {
    SourcePaths p;
    p.home = home_directory();
    p.claude_config_dir = claude_config_dir(configured_claude_dir, environment_variable("CLAUDE_CONFIG_DIR"), p.home);
    const std::string codex = environment_variable("CODEX_HOME");
    p.codex_home = codex.empty() ? join_path(p.home, ".codex") : codex;
    return p;
}

}  // namespace kt::agents
