// Interned tables referenced by cells: styles, hyperlinks and graphemes.
//
// Each table is per terminal. Cells hold 16-bit (styles, links) or 21-bit (graphemes) indices.
// Entries are not refcounted; instead the terminal runs a collection pass (mark the indices in
// use on both screens and in the scrollback, compact, rewrite cells) when a table passes its
// threshold. The UI gets immutable copies through snapshot(), which is cached until the next
// change (copy-on-write versions).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vt/cell.h"

namespace kt::vt {

struct StyleHash {
    std::size_t operator()(const Style& s) const;
};

class StyleTable {
public:
    static constexpr std::size_t kCapacity = 65535;
    static constexpr std::size_t kCollectAt = kCapacity / 4 * 3;  // 75 %

    StyleTable();

    // Index of `s`, adding it if needed. When the table is full, returns the nearest existing
    // style (same colors, fewer attributes) or 0.
    uint16_t intern(const Style& s);
    const Style& operator[](uint16_t i) const { return entries_[i]; }
    std::size_t size() const { return entries_.size(); }
    bool needs_collect() const { return entries_.size() >= kCollectAt; }

    // Keeps the entries marked in `used` (index 0 always) and returns old index -> new index.
    std::vector<uint16_t> compact(const std::vector<bool>& used);
    void clear();

    using Snapshot = std::shared_ptr<const std::vector<Style>>;
    Snapshot snapshot() const;
    uint64_t version() const { return version_; }

private:
    std::vector<Style> entries_;
    std::unordered_map<Style, uint16_t, StyleHash> index_;
    uint64_t version_ = 1;
    mutable Snapshot snap_;
};

struct Link {
    std::string id;   // OSC 8 id= parameter; may be empty
    std::string uri;
};

class LinkTable {
public:
    // Entries are bounded by the 16-bit cell index and by the bytes of their ids and URIs.
    static constexpr std::size_t kMaxLive = 65534;
    static constexpr std::size_t kMaxUri = 2048;
    static constexpr std::size_t kMaxBytes = 8u << 20;
    static constexpr std::size_t kCollectAt = kMaxLive / 4 * 3;  // 75 %

    LinkTable();

    // Index of (id, uri), or 0 when the URI is too long or the table is full.
    uint16_t intern(std::string_view id, std::string_view uri);
    const Link& operator[](uint16_t i) const { return entries_[i]; }
    std::size_t size() const { return entries_.size(); }  // includes the unused entry 0
    bool full() const { return entries_.size() > kMaxLive || bytes_ >= kMaxBytes; }
    bool needs_collect() const { return entries_.size() >= kCollectAt || bytes_ >= kMaxBytes / 4 * 3; }
    std::size_t bytes() const { return bytes_; }

    std::vector<uint16_t> compact(const std::vector<bool>& used);
    void clear();

    using Snapshot = std::shared_ptr<const std::vector<Link>>;
    Snapshot snapshot() const;

private:
    std::vector<Link> entries_;
    std::unordered_map<std::string, uint16_t> index_;  // key: id + '\0' + uri
    std::size_t bytes_ = 0;                            // ids and URIs of the entries
    mutable Snapshot snap_;
};

class GraphemePool {
public:
    static constexpr std::size_t kMaxCodePoints = 32;  // longer clusters are truncated
    static constexpr std::size_t kCollectAt = 1u << 16;
    static constexpr std::size_t kMaxEntries = 1u << 20;

    GraphemePool();

    // Index of the cluster, or 0 when the pool is full. `cps.size()` must be >= 2.
    uint32_t intern(std::u32string_view cps);
    std::u32string_view operator[](uint32_t i) const { return entries_[i]; }
    std::size_t size() const { return entries_.size(); }
    bool needs_collect() const { return entries_.size() >= kCollectAt; }

    std::vector<uint32_t> compact(const std::vector<bool>& used);
    void clear();

    using Snapshot = std::shared_ptr<const std::vector<std::u32string>>;
    Snapshot snapshot() const;

private:
    std::vector<std::u32string> entries_;
    std::unordered_map<std::u32string, uint32_t> index_;
    mutable Snapshot snap_;
};

}  // namespace kt::vt
