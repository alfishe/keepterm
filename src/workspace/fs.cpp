#include "workspace/fs.h"

namespace kt::ws {

std::string path_join(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
    out.append(name);
    return out;
}

namespace {
std::string parent_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
}  // namespace

ReadResult MemoryFileSystem::read(const std::string& path) {
    if (fail_read && fail_read(path)) return {ReadStatus::Error, {}, "injected read error"};
    std::lock_guard lock(mu_);
    auto it = files_.find(path);
    if (it == files_.end()) return {ReadStatus::Missing, {}, {}};
    return {ReadStatus::Ok, it->second.first, {}};
}

WsResult<void> MemoryFileSystem::write_atomic(const std::string& path, std::string_view data) {
    if (on_write) on_write(path);
    if (fail_write && fail_write(path)) return ws_error(WsErrc::Io, "injected write error", path);
    std::lock_guard lock(mu_);
    auto& f = files_[path];
    f.first.assign(data);
    ++writes_;
    return {};
}

WsResult<void> MemoryFileSystem::rename(const std::string& from, const std::string& to) {
    if (fail_rename && fail_rename(from)) return ws_error(WsErrc::Io, "injected rename error", from);
    std::lock_guard lock(mu_);
    auto it = files_.find(from);
    if (it == files_.end()) return ws_error(WsErrc::NotFound, "no such file", from);
    auto v = std::move(it->second);
    files_.erase(it);
    files_[to] = std::move(v);
    return {};
}

WsResult<void> MemoryFileSystem::remove(const std::string& path) {
    std::lock_guard lock(mu_);
    if (!files_.erase(path)) return ws_error(WsErrc::NotFound, "no such file", path);
    return {};
}

bool MemoryFileSystem::exists(const std::string& path) {
    std::lock_guard lock(mu_);
    return files_.count(path) != 0;
}

std::vector<std::string> MemoryFileSystem::list(const std::string& dir) {
    std::lock_guard lock(mu_);
    std::vector<std::string> out;
    for (const auto& [p, v] : files_)
        if (parent_of(p) == dir) out.push_back(p.substr(dir.size() + 1));
    return out;
}

WsResult<void> MemoryFileSystem::make_private_dir(const std::string&) { return {}; }

std::optional<std::int64_t> MemoryFileSystem::mtime_unix_ms(const std::string& path) {
    std::lock_guard lock(mu_);
    auto it = files_.find(path);
    if (it == files_.end()) return std::nullopt;
    return it->second.second;
}

void MemoryFileSystem::put(const std::string& path, std::string data, std::int64_t mtime) {
    std::lock_guard lock(mu_);
    files_[path] = {std::move(data), mtime};
}

std::optional<std::string> MemoryFileSystem::get(const std::string& path) const {
    std::lock_guard lock(mu_);
    auto it = files_.find(path);
    if (it == files_.end()) return std::nullopt;
    return it->second.first;
}

std::vector<std::string> MemoryFileSystem::paths() const {
    std::lock_guard lock(mu_);
    std::vector<std::string> out;
    for (const auto& [p, v] : files_) out.push_back(p);
    return out;
}

int MemoryFileSystem::writes() const {
    std::lock_guard lock(mu_);
    return writes_;
}

}  // namespace kt::ws
