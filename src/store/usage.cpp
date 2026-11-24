#include "store/usage.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace kt::store {

namespace fs = std::filesystem;

namespace {

LimitUsage make_limit(uint64_t used, uint64_t limit) {
    LimitUsage l;
    l.used = used;
    l.limit = limit;
    l.remaining = used < limit ? limit - used : 0;
    l.percent = limit ? 100.0 * static_cast<double>(used) / static_cast<double>(limit) : (used ? 100.0 : 0.0);
    return l;
}

bool overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 <= b1 && b0 <= a1; }

const char* age_reason(Kind k, const Policy& p) {
    if (k == Kind::events) return "events_max_age";
    if (k == Kind::raw && p.raw_max_age_ms < p.max_age_ms) return "raw_max_age";
    return "max_age";
}

uint64_t size_of(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(n);
}

}  // namespace

Result<Usage> usage(Catalog& catalog, const fs::path& root, const Policy& policy, uint64_t now_ms,
                    const Janitor::PolicyFor& per_session) {
    auto sessions = catalog.sessions();
    if (!sessions) return Unexpected(sessions.error());
    auto segs = catalog.segments();
    if (!segs) return Unexpected(segs.error());
    auto pins = catalog.pins();
    if (!pins) return Unexpected(pins.error());

    Usage u;
    u.now_ms = now_ms;
    u.policy = policy;
    std::map<std::string, const SessionRow*> by_id;
    for (const auto& s : *sessions) by_id[s.id] = &s;
    auto policy_of = [&](const std::string& id) {
        const auto it = by_id.find(id);
        return per_session && it != by_id.end() ? per_session(*it->second) : policy;
    };
    auto is_protected = [&](const SegmentRow& r) {
        const auto it = by_id.find(r.session);
        if (it != by_id.end() && it->second->pinned) return true;
        for (const auto& p : *pins) {
            if ((p.session.empty() || p.session == r.session) && overlaps(p.from_ms, p.to_ms, r.info.ts_first, r.info.ts_last)) {
                return true;
            }
        }
        return false;
    };

    std::map<std::string, SessionUsage> per;
    for (const auto& s : *sessions) {
        SessionUsage& su = per[s.id];
        su.id = s.id;
        su.project = s.project;
        su.live = !s.ended_ms.has_value();
        su.pinned = s.pinned;
    }
    uint64_t total = 0;
    for (const auto& r : *segs) {
        const auto k = static_cast<std::size_t>(r.info.kind);
        const uint64_t idx = std::min(r.info.index_bytes, r.info.file_bytes);
        total += r.info.file_bytes;
        u.kind_bytes[k] += r.info.file_bytes - idx;
        u.index_bytes += idx;
        if (r.info.kind == Kind::raw) u.raw_payload_bytes += r.info.raw_bytes;
        if (r.info.kind == Kind::text) u.text_payload_bytes += r.info.raw_bytes;
        if (is_protected(r)) u.pinned_bytes += r.info.file_bytes;
        SessionUsage& su = per[r.session];
        if (su.id.empty()) su.id = r.session;  // a row of an unknown session (orphan)
        su.kind_bytes[k] += r.info.file_bytes - idx;
        su.index_bytes += idx;
        su.cap.used += r.info.file_bytes;
        ++su.segments;
    }
    u.total = make_limit(total, policy.max_total);
    for (auto& [id, su] : per) {
        su.cap = make_limit(su.cap.used, policy_of(id).max_per_session);
        u.sessions.push_back(su);
    }
    std::sort(u.sessions.begin(), u.sessions.end(), [](const SessionUsage& a, const SessionUsage& b) {
        return a.cap.used != b.cap.used ? a.cap.used > b.cap.used : a.id < b.id;
    });

    // The catalog files.
    const fs::path db = root / kCatalogFile;
    u.catalog_bytes = size_of(db) + size_of(fs::path(db).concat("-wal")) + size_of(fs::path(db).concat("-shm"));

    // The volume.
    {
        std::error_code ec;
        const auto sp = fs::space(root, ec);
        if (!ec && sp.capacity > 0) {
            DiskUsage& d = u.disk;
            d.capacity = sp.capacity;
            d.available = sp.available;
            const auto pct = static_cast<uint64_t>(static_cast<double>(sp.capacity) * policy.disk.min_free_percent / 100.0);
            uint64_t limit = policy.disk.min_free_bytes;
            if (policy.disk.min_free_percent > 0 && (policy.disk.min_free_bytes == 0 || pct < limit)) limit = pct;
            d.threshold = limit;
            d.below = sp.available < limit;
            const uint64_t used = sp.capacity > sp.available ? sp.capacity - sp.available : 0;
            d.fill = make_limit(used, sp.capacity > limit ? sp.capacity - limit : 0);
        }
    }

    // The nearest age expiry (what a pass removes first by age).
    auto consider = [&](Eviction e) {
        if (!u.next_expiry || e.at_ms < u.next_expiry->at_ms) u.next_expiry = std::move(e);
    };
    for (const auto& r : *segs) {
        if (!r.info.sealed || is_protected(r)) continue;
        const Policy pol = policy_of(r.session);
        const uint64_t age = pol.max_age_of(r.info.kind);
        // The first block expires at ts_first + age (a trim), the whole segment at ts_last + age.
        Eviction e;
        e.session = r.session;
        e.segment_id = r.id;
        e.kind = r.info.kind;
        e.bytes = r.info.file_bytes;
        e.at_ms = r.info.ts_first + age;
        e.reason = age_reason(r.info.kind, pol);
        consider(e);
    }
    for (const auto& s : *sessions) {
        if (!s.ended_ms || s.pinned) continue;
        Eviction e;
        e.session = s.id;
        e.at_ms = *s.ended_ms + policy_of(s.id).keep_closed_ms;
        e.reason = "keep_closed";
        const auto it = per.find(s.id);
        e.bytes = it != per.end() ? it->second.cap.used : 0;
        consider(e);
    }

    // Over a cap now: the order of Janitor::apply_retention (per session, then the total:
    // closed sessions first, oldest first).
    std::set<int64_t> gone;
    auto oldest = [](const SegmentRow* a, const SegmentRow* b) { return a->info.ts_last < b->info.ts_last; };
    uint64_t remaining_total = total;
    for (const auto& su : u.sessions) {
        if (su.cap.used <= su.cap.limit) continue;
        std::vector<const SegmentRow*> cand;
        for (const auto& r : *segs) {
            if (r.session == su.id && r.info.sealed && !is_protected(r)) cand.push_back(&r);
        }
        std::sort(cand.begin(), cand.end(), oldest);
        uint64_t t = su.cap.used;
        for (const auto* r : cand) {
            if (t <= su.cap.limit) break;
            t -= r->info.file_bytes;
            remaining_total -= r->info.file_bytes;
            gone.insert(r->id);
            if (u.over_cap.size() < 16) {
                u.over_cap.push_back({r->session, r->id, r->info.kind, r->info.file_bytes, now_ms, "max_per_session"});
            }
        }
    }
    if (remaining_total > policy.max_total) {
        std::vector<const SegmentRow*> closed, live;
        for (const auto& r : *segs) {
            if (gone.count(r.id) || !r.info.sealed || is_protected(r)) continue;
            const auto it = by_id.find(r.session);
            const bool is_closed = it == by_id.end() || it->second->ended_ms.has_value();
            (is_closed ? closed : live).push_back(&r);
        }
        std::sort(closed.begin(), closed.end(), oldest);
        std::sort(live.begin(), live.end(), oldest);
        for (const auto* list : {&closed, &live}) {
            for (const auto* r : *list) {
                if (remaining_total <= policy.max_total) break;
                remaining_total -= r->info.file_bytes;
                if (u.over_cap.size() < 16) {
                    u.over_cap.push_back({r->session, r->id, r->info.kind, r->info.file_bytes, now_ms, "max_total"});
                }
            }
        }
    }
    return u;
}

void UsageWatcher::level(const std::string& key, const std::string& limit, const std::string& session,
                         const LimitUsage& at) {
    const int now = at.percent >= 95.0 ? 95 : at.percent >= 80.0 ? 80 : 0;
    int& was = levels_[key];
    if (now == was) return;
    // Rising: report each new level; falling: only once it is back below 80 %.
    if (now > was || now == 0) {
        UsageEvent e;
        e.type = UsageEvent::Type::threshold;
        e.limit = limit;
        e.session = session;
        e.level = now;
        e.at = at;
        if (on_event_) on_event_(e);
    }
    was = now;
}

void UsageWatcher::update(const Usage& u) {
    level("max_total", "max_total", "", u.total);
    for (const auto& s : u.sessions) {
        if (!s.live) continue;  // closed sessions do not grow
        level("session:" + s.id, "max_per_session", s.id, s.cap);
    }
    if (u.disk.capacity) {
        level("min_free_disk", "min_free_disk", "", u.disk.fill);
        writes_stopped(u.disk.below);
    }
}

void UsageWatcher::writes_stopped(bool stopped) {
    if (stopped_ && *stopped_ == stopped) return;
    const bool first = !stopped_.has_value();
    stopped_ = stopped;
    if (first && !stopped) return;  // the normal state needs no event
    UsageEvent e;
    e.type = stopped ? UsageEvent::Type::writes_stopped : UsageEvent::Type::writes_resumed;
    e.limit = "min_free_disk";
    if (on_event_) on_event_(e);
}

}  // namespace kt::store
