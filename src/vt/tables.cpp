#include "vt/tables.h"

namespace kt::vt {

std::size_t StyleHash::operator()(const Style& s) const {
    uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    };
    mix(s.fg.v);
    mix(s.bg.v);
    mix(s.underline_color.v);
    mix((uint64_t{s.attrs} << 8) | static_cast<uint64_t>(s.underline));
    return static_cast<std::size_t>(h);
}

// ------------------------------------------------------------------------------------ styles

StyleTable::StyleTable() { clear(); }

void StyleTable::clear() {
    entries_.assign(1, Style{});
    index_.clear();
    index_.emplace(Style{}, uint16_t{0});
    ++version_;
    snap_.reset();
}

uint16_t StyleTable::intern(const Style& s) {
    if (auto it = index_.find(s); it != index_.end()) return it->second;
    if (entries_.size() >= kCapacity) {
        Style fallback = s;
        fallback.attrs = 0;
        fallback.underline = Underline::None;
        if (auto it = index_.find(fallback); it != index_.end()) return it->second;
        return 0;
    }
    const auto idx = static_cast<uint16_t>(entries_.size());
    entries_.push_back(s);
    index_.emplace(s, idx);
    ++version_;
    snap_.reset();
    return idx;
}

std::vector<uint16_t> StyleTable::compact(const std::vector<bool>& used) {
    std::vector<uint16_t> remap(entries_.size(), 0);
    std::vector<Style> kept;
    kept.reserve(entries_.size());
    kept.push_back(Style{});
    index_.clear();
    index_.emplace(Style{}, uint16_t{0});
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (i < used.size() && used[i]) {
            const auto idx = static_cast<uint16_t>(kept.size());
            remap[i] = idx;
            kept.push_back(entries_[i]);
            index_.emplace(entries_[i], idx);
        }
    }
    entries_ = std::move(kept);
    ++version_;
    snap_.reset();
    return remap;
}

StyleTable::Snapshot StyleTable::snapshot() const {
    if (!snap_) snap_ = std::make_shared<const std::vector<Style>>(entries_);
    return snap_;
}

// ------------------------------------------------------------------------------------- links

namespace {
std::string link_key(std::string_view id, std::string_view uri) {
    std::string k;
    k.reserve(id.size() + uri.size() + 1);
    k.append(id);
    k.push_back('\0');
    k.append(uri);
    return k;
}
}  // namespace

LinkTable::LinkTable() { clear(); }

void LinkTable::clear() {
    entries_.assign(1, Link{});
    index_.clear();
    bytes_ = 0;
    snap_.reset();
}

uint16_t LinkTable::intern(std::string_view id, std::string_view uri) {
    if (uri.empty() || uri.size() > kMaxUri) return 0;
    std::string key = link_key(id, uri);
    if (auto it = index_.find(key); it != index_.end()) return it->second;
    if (full() || bytes_ + id.size() + uri.size() > kMaxBytes) return 0;
    const auto idx = static_cast<uint16_t>(entries_.size());
    entries_.push_back(Link{std::string(id), std::string(uri)});
    bytes_ += id.size() + uri.size();
    index_.emplace(std::move(key), idx);
    snap_.reset();
    return idx;
}

std::vector<uint16_t> LinkTable::compact(const std::vector<bool>& used) {
    std::vector<uint16_t> remap(entries_.size(), 0);
    std::vector<Link> kept;
    kept.push_back(Link{});
    index_.clear();
    bytes_ = 0;
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (i < used.size() && used[i]) {
            bytes_ += entries_[i].id.size() + entries_[i].uri.size();
            const auto idx = static_cast<uint16_t>(kept.size());
            remap[i] = idx;
            index_.emplace(link_key(entries_[i].id, entries_[i].uri), idx);
            kept.push_back(std::move(entries_[i]));
        }
    }
    entries_ = std::move(kept);
    snap_.reset();
    return remap;
}

LinkTable::Snapshot LinkTable::snapshot() const {
    if (!snap_) snap_ = std::make_shared<const std::vector<Link>>(entries_);
    return snap_;
}

// --------------------------------------------------------------------------------- graphemes

GraphemePool::GraphemePool() { clear(); }

void GraphemePool::clear() {
    entries_.assign(1, std::u32string{});
    index_.clear();
    snap_.reset();
}

uint32_t GraphemePool::intern(std::u32string_view cps) {
    if (cps.size() > kMaxCodePoints) cps = cps.substr(0, kMaxCodePoints);
    std::u32string key(cps);
    if (auto it = index_.find(key); it != index_.end()) return it->second;
    if (entries_.size() >= kMaxEntries) return 0;
    const auto idx = static_cast<uint32_t>(entries_.size());
    entries_.push_back(key);
    index_.emplace(std::move(key), idx);
    snap_.reset();
    return idx;
}

std::vector<uint32_t> GraphemePool::compact(const std::vector<bool>& used) {
    std::vector<uint32_t> remap(entries_.size(), 0);
    std::vector<std::u32string> kept;
    kept.emplace_back();
    index_.clear();
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (i < used.size() && used[i]) {
            const auto idx = static_cast<uint32_t>(kept.size());
            remap[i] = idx;
            index_.emplace(entries_[i], idx);
            kept.push_back(std::move(entries_[i]));
        }
    }
    entries_ = std::move(kept);
    snap_.reset();
    return remap;
}

GraphemePool::Snapshot GraphemePool::snapshot() const {
    if (!snap_) snap_ = std::make_shared<const std::vector<std::u32string>>(entries_);
    return snap_;
}

}  // namespace kt::vt
