#include "workspace/usage.h"

#include <algorithm>

#include "workspace/json.h"
#include "workspace/state.h"

namespace kt::ws {

UsageStore::UsageStore(IFileSystem& fs, IClock& clock, std::string state_dir)
    : fs_(fs), clock_(clock), dir_(std::move(state_dir)) {}

std::optional<std::string> UsageStore::load() {
    items_.clear();
    dirty_ = false;
    ReadResult r = fs_.read(file());
    if (r.status != ReadStatus::Ok) return std::nullopt;
    json::Value doc;
    bool ok = json::parse(r.data, doc) && doc.is_object();
    const json::Value* items = ok ? doc.get("items") : nullptr;
    if (ok && (!items || !items->is_object())) ok = false;
    if (ok) {
        for (const auto& [key, v] : items->members()) {
            if (!v.is_object()) continue;
            UsageEntry e;
            if (const json::Value* n = v.get("n"); n && n->as_int() && *n->as_int() > 0)
                e.n = static_cast<std::uint64_t>(*n->as_int());
            if (const json::Value* t = v.get("t"); t && t->as_array())
                for (const auto& x : *t->as_array())
                    if (x.as_int()) e.t.push_back(*x.as_int());
            std::sort(e.t.begin(), e.t.end(), std::greater<>());
            if (e.t.size() > kUsageMaxStamps) e.t.resize(kUsageMaxStamps);
            if (e.n == 0) e.n = e.t.size();
            if (e.n == 0) continue;
            items_.emplace(key, std::move(e));
        }
        evict();
        return std::nullopt;
    }
    const std::string base = file() + ".corrupt-" + file_stamp(clock_.unix_ms());
    std::string name = base;
    for (int i = 2; fs_.exists(name); ++i) name = base + "-" + std::to_string(i);
    if (!fs_.rename(file(), name)) return file();
    return name;
}

void UsageStore::record(std::string_view key) {
    if (key.empty()) return;
    auto it = items_.find(key);
    if (it == items_.end()) it = items_.emplace(std::string(key), UsageEntry{}).first;
    UsageEntry& e = it->second;
    ++e.n;
    e.t.insert(e.t.begin(), clock_.unix_ms());
    if (e.t.size() > kUsageMaxStamps) e.t.resize(kUsageMaxStamps);
    dirty_ = true;
    evict();
}

const UsageEntry* UsageStore::get(std::string_view key) const {
    auto it = items_.find(key);
    return it == items_.end() ? nullptr : &it->second;
}

void UsageStore::forget(std::string_view key) {
    auto it = items_.find(key);
    if (it == items_.end()) return;
    items_.erase(it);
    dirty_ = true;
}

void UsageStore::evict() {
    while (items_.size() > kUsageMaxKeys) {
        auto oldest = items_.begin();
        auto last = [](const UsageEntry& e) { return e.t.empty() ? std::int64_t{0} : e.t.front(); };
        for (auto it = items_.begin(); it != items_.end(); ++it)
            if (last(it->second) < last(oldest->second)) oldest = it;
        items_.erase(oldest);
        dirty_ = true;
    }
}

std::string UsageStore::serialize() const {
    json::Value doc = json::Value::object();
    doc.set("version", json::Value::integer(1));
    json::Value items = json::Value::object();
    for (const auto& [key, e] : items_) {
        json::Value v = json::Value::object();
        v.set("n", json::Value::integer(static_cast<std::int64_t>(e.n)));
        json::Value t = json::Value::array();
        for (std::int64_t x : e.t) t.push(json::Value::integer(x));
        v.set("t", std::move(t));
        items.set(key, std::move(v));
    }
    doc.set("items", std::move(items));
    return doc.dump(false);
}

void UsageStore::maybe_save() {
    if (!dirty_) return;
    const std::int64_t now = clock_.steady_ms();
    if (last_save_ >= 0 && now - last_save_ < kUsageSaveInterval) return;
    (void)flush();
}

WsResult<void> UsageStore::flush() {
    if (!dirty_) return {};
    (void)fs_.make_private_dir(dir_);
    auto r = fs_.write_atomic(file(), serialize());
    last_save_ = clock_.steady_ms();
    if (r) dirty_ = false;
    return r;
}

}  // namespace kt::ws
