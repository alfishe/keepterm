// palette_usage.json: what the palette ran and when. Core code.
//
// { "version": 1, "items": { "cmd:pane.split_right": { "n": 12, "t": [1763547302000, ...] } } }
// Per key: the total count and the last 10 timestamps (Unix ms, newest first). At most 2,000 keys;
// the least recently used key is evicted. Written atomically at most every 30 s and at quit.
// A corrupt file is kept aside as palette_usage.json.corrupt-<time> and usage starts empty.
// The command module does the ranking; this store only remembers.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/fs.h"
#include "workspace/ids.h"

namespace kt::ws {

inline constexpr std::string_view kUsageFileName = "palette_usage.json";
inline constexpr std::size_t kUsageMaxKeys = 2000;
inline constexpr std::size_t kUsageMaxStamps = 10;
inline constexpr std::int64_t kUsageSaveInterval = 30'000;

struct UsageEntry {
    std::uint64_t n = 0;
    std::vector<std::int64_t> t;   // newest first
};

class UsageStore {
public:
    UsageStore(IFileSystem& fs, IClock& clock, std::string state_dir);

    // Reads the file; a corrupt one is renamed aside. Returns the corrupt file name, if any.
    std::optional<std::string> load();
    void record(std::string_view key);
    const UsageEntry* get(std::string_view key) const;
    const std::map<std::string, UsageEntry, std::less<>>& items() const { return items_; }
    void forget(std::string_view key);

    // Writes when dirty and 30 s passed since the last write (call from a periodic tick).
    void maybe_save();
    WsResult<void> flush();   // quit
    bool dirty() const { return dirty_; }
    std::string file() const { return path_join(dir_, kUsageFileName); }

    std::string serialize() const;

private:
    void evict();

    IFileSystem& fs_;
    IClock& clock_;
    std::string dir_;
    std::map<std::string, UsageEntry, std::less<>> items_;
    bool dirty_ = false;
    std::int64_t last_save_ = -1;
};

}  // namespace kt::ws
