#include "store/retention.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <system_error>

#include "config/config.h"
#include "platform/paths.h"

namespace kt::store {

namespace fs = std::filesystem;

Policy policy_from_config(const config::Config& cfg) {
    using config::KeyId;
    Policy p;
    auto u = [](std::int64_t v) { return v > 0 ? static_cast<uint64_t>(v) : uint64_t{0}; };
    p.max_total = u(cfg.get_int(KeyId::storage_max_total));
    p.max_per_session = u(cfg.get_int(KeyId::storage_max_per_session));
    p.max_age_ms = u(cfg.get_int(KeyId::storage_max_age));
    p.keep_closed_ms = u(cfg.get_int(KeyId::storage_keep_closed));
    p.events_max_age_ms = u(cfg.get_int(KeyId::storage_events_max_age));
    p.raw_max_age_ms = u(cfg.get_int(KeyId::storage_raw_max_age));
    p.disk.min_free_bytes = u(cfg.get_int(KeyId::storage_min_free_disk));
    p.disk.min_free_percent = cfg.get_float(KeyId::storage_min_free_disk_percent);
    p.cold_after_ms = u(cfg.get_int(KeyId::storage_cold_after));
    const auto codec = codec_from_string(cfg.get_string(KeyId::storage_cold_codec));
    if (codec && codec_available(*codec)) {
        p.cold.codec = *codec;
        const LevelRange lr = codec_levels(*codec);
        p.cold.level = std::clamp(static_cast<int>(cfg.get_int(KeyId::storage_cold_codec_level)), lr.min, lr.max);
    }
    p.cold.block_raw = static_cast<uint32_t>(
        std::clamp<uint64_t>(u(cfg.get_int(KeyId::storage_cold_block_size)), kMinBlockTarget, kMaxBlockTarget));
    p.cold.index = IndexMode::on_append;
    return p;
}

WriterOptions fresh_options_from_config(const config::Config& cfg) {
    using config::KeyId;
    WriterOptions o;
    const auto codec = codec_from_string(cfg.get_string(KeyId::storage_codec));
    if (codec && codec_available(*codec)) {
        o.codec = *codec;
        const LevelRange lr = codec_levels(*codec);
        o.level = std::clamp(static_cast<int>(cfg.get_int(KeyId::storage_codec_level)), lr.min, lr.max);
    }
    const auto bs = cfg.get_int(KeyId::storage_block_size);
    o.block_raw = static_cast<uint32_t>(std::clamp<int64_t>(bs, kMinBlockTarget, kMaxBlockTarget));
    o.index = IndexMode::at_seal;
    return o;
}

bool same_tier(const SegmentInfo& info, const WriterOptions& o) {
    return info.codec == o.codec && info.level == o.level && info.block_raw == o.block_raw;
}

WriterOptions options_of(const SegmentInfo& info) {
    WriterOptions o;
    o.codec = codec_available(info.codec) ? info.codec : Codec::zstd;
    o.level = info.level;
    o.block_raw = info.block_raw ? info.block_raw : kBlockRawTarget;
    o.index = IndexMode::on_append;
    return o;
}

namespace {

bool overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 <= b1 && b0 <= a1; }

struct State {
    std::map<std::string, SessionRow> sessions;
    std::vector<PinRow> pins;
    std::vector<SegmentRow> segs;
};

Result<State> load_state(Catalog& c) {
    State s;
    auto ss = c.sessions();
    if (!ss) return Unexpected(ss.error());
    for (auto& r : *ss) s.sessions.emplace(r.id, std::move(r));
    auto pins = c.pins();
    if (!pins) return Unexpected(pins.error());
    s.pins = std::move(*pins);
    auto segs = c.segments();
    if (!segs) return Unexpected(segs.error());
    s.segs = std::move(*segs);
    return s;
}

bool protected_segment(const State& st, const SegmentRow& r) {
    const auto it = st.sessions.find(r.session);
    if (it != st.sessions.end() && it->second.pinned) return true;
    for (const auto& p : st.pins) {
        if ((p.session.empty() || p.session == r.session) && overlaps(p.from_ms, p.to_ms, r.info.ts_first, r.info.ts_last)) {
            return true;
        }
    }
    return false;
}

bool block_pinned(const State& st, const std::string& session, const BlockEntry& e) {
    for (const auto& p : st.pins) {
        if ((p.session.empty() || p.session == session) && overlaps(p.from_ms, p.to_ms, e.ts_first, e.ts_last)) return true;
    }
    return false;
}

bool session_closed(const State& st, const std::string& id) {
    const auto it = st.sessions.find(id);
    return it == st.sessions.end() || it->second.ended_ms.has_value();
}

bool valid_session_dir(std::string_view name) {
    return !name.empty() && name.find_first_of("/\\.") == std::string_view::npos;
}

void remove_file(const fs::path& p, JanitorReport& rep) {
    std::error_code ec;
    fs::remove(p, ec);
    if (ec) ++rep.delete_failures;
}

}  // namespace

Janitor::Janitor(Catalog& catalog, fs::path root, Policy policy, PolicyFor per_session)
    : catalog_(catalog), root_(std::move(root)), policy_(policy), per_session_(std::move(per_session)) {}

Result<void> Janitor::remove_segments(const std::vector<SegmentRow>& rows, JanitorReport& rep) {
    if (rows.empty()) return {};
    std::vector<int64_t> ids;
    for (const auto& r : rows) ids.push_back(r.id);
    if (auto d = catalog_.delete_segments(ids); !d) return d;
    for (const auto& r : rows) {
        remove_file(root_ / platform::paths::from_utf8(r.file), rep);
        rep.freed_bytes += r.info.file_bytes;
    }
    return {};
}

Result<JanitorReport> Janitor::run_pass(uint64_t now_ms) {
    JanitorReport rep;
    if (auto r = clean_orphans(now_ms, rep); !r) return Unexpected(r.error());
    if (auto r = apply_retention(now_ms, rep); !r) return Unexpected(r.error());
    if (auto r = compact(now_ms, rep); !r) return Unexpected(r.error());
    auto segs = catalog_.segments();
    if (!segs) return Unexpected(segs.error());
    rep.total_bytes = 0;
    for (const auto& s : *segs) rep.total_bytes += s.info.file_bytes;
    return rep;
}

Result<void> Janitor::clean_orphans(uint64_t now_ms, JanitorReport& rep) {
    // Files first, then the catalog: a writer inserts the row before it creates the file, so a
    // file seen here whose row is missing afterwards is a real orphan.
    struct Seen {
        fs::path path;
        std::string rel;
        bool temp;
        uint64_t age_ms;
    };
    std::vector<Seen> files;
    std::vector<fs::path> dirs;
    std::error_code ec;
    const auto now_ft = fs::file_time_type::clock::now();
    for (const auto& d : fs::directory_iterator(root_, ec)) {
        if (!d.is_directory(ec)) continue;
        const std::string dname = platform::paths::to_utf8(d.path().filename());
        if (!valid_session_dir(dname)) continue;
        dirs.push_back(d.path());
        for (const auto& f : fs::directory_iterator(d.path(), ec)) {
            if (!f.is_regular_file(ec)) continue;
            const std::string fname = platform::paths::to_utf8(f.path().filename());
            const bool temp = fname.ends_with(".tmp");
            if (!temp && !fname.ends_with(kSegmentExt)) continue;
            std::error_code tec;
            const auto mt = fs::last_write_time(f.path(), tec);
            const uint64_t age = tec || mt > now_ft
                                     ? 0
                                     : static_cast<uint64_t>(
                                           std::chrono::duration_cast<std::chrono::milliseconds>(now_ft - mt).count());
            files.push_back({f.path(), dname + "/" + fname, temp, age});
        }
    }
    auto segs = catalog_.segments();
    if (!segs) return Unexpected(segs.error());
    std::set<std::string> known;
    for (const auto& s : *segs) known.insert(s.file);
    for (const auto& f : files) {
        if (f.temp) {
            if (f.age_ms >= policy_.orphan_grace_ms) {
                remove_file(f.path, rep);
                ++rep.temp_files;
            }
        } else if (!known.count(f.rel)) {
            remove_file(f.path, rep);
            ++rep.orphan_files;
        }
    }
    // Rows without files. An unsealed row gets a grace period (its file is created right after).
    std::vector<int64_t> gone;
    for (const auto& s : *segs) {
        if (fs::exists(root_ / platform::paths::from_utf8(s.file), ec)) continue;
        if (!s.info.sealed && s.created_ms + policy_.orphan_grace_ms > now_ms) continue;
        gone.push_back(s.id);
    }
    if (auto d = catalog_.delete_segments(gone); !d) return d;
    rep.orphan_rows += static_cast<uint32_t>(gone.size());
    // Folders of sessions the catalog no longer knows, once empty.
    auto sessions = catalog_.sessions();
    if (!sessions) return Unexpected(sessions.error());
    std::set<std::string> ids;
    for (const auto& s : *sessions) ids.insert(s.id);
    for (const auto& d : dirs) {
        if (ids.count(platform::paths::to_utf8(d.filename()))) continue;
        if (fs::is_empty(d, ec) && fs::remove(d, ec)) ++rep.empty_dirs;
    }
    return {};
}

Result<void> Janitor::apply_retention(uint64_t now_ms, JanitorReport& rep) {
    auto st = load_state(catalog_);
    if (!st) return Unexpected(st.error());

    // 1. Expired by age, per kind.
    std::vector<SegmentRow> doomed;
    for (const auto& s : st->segs) {
        if (!s.info.sealed || protected_segment(*st, s)) continue;
        const auto it = st->sessions.find(s.session);
        const Policy pol = it != st->sessions.end() ? policy_for(it->second) : policy_;
        const uint64_t age = pol.max_age_of(s.info.kind);
        if (s.info.ts_last + age < now_ms) {
            doomed.push_back(s);
            if (s.info.kind == Kind::raw && age < pol.max_age_ms) ++rep.raw_expired;
        }
    }
    rep.expired += static_cast<uint32_t>(doomed.size());
    if (auto r = remove_segments(doomed, rep); !r) return r;

    // 2. Closed sessions past keep_closed.
    for (const auto& [id, s] : st->sessions) {
        if (!s.ended_ms || s.pinned) continue;
        const Policy pol = policy_for(s);
        if (*s.ended_ms + pol.keep_closed_ms >= now_ms) continue;
        std::vector<SegmentRow> mine;
        bool kept = false;
        for (const auto& seg : st->segs) {
            if (seg.session != id) continue;
            if (std::find_if(doomed.begin(), doomed.end(), [&](const SegmentRow& d) { return d.id == seg.id; }) !=
                doomed.end()) {
                continue;
            }
            if (protected_segment(*st, seg)) kept = true;
            else mine.push_back(seg);
        }
        if (auto r = remove_segments(mine, rep); !r) return r;
        if (!kept) {
            auto f = forget_session(catalog_, root_, id);
            if (!f) return Unexpected(f.error());
        }
        ++rep.closed_sessions;
    }

    // 3. Per-session caps, oldest sealed segments first.
    st = load_state(catalog_);
    if (!st) return Unexpected(st.error());
    std::map<std::string, std::vector<const SegmentRow*>> by_session;
    for (const auto& s : st->segs) by_session[s.session].push_back(&s);
    std::set<int64_t> removed;
    for (auto& [id, list] : by_session) {
        uint64_t total = 0;
        for (const auto* s : list) total += s->info.file_bytes;
        const auto it = st->sessions.find(id);
        const Policy pol = it != st->sessions.end() ? policy_for(it->second) : policy_;
        if (total <= pol.max_per_session) continue;
        std::vector<const SegmentRow*> cand;
        for (const auto* s : list) {
            if (s->info.sealed && !protected_segment(*st, *s)) cand.push_back(s);
        }
        std::sort(cand.begin(), cand.end(), [](const SegmentRow* a, const SegmentRow* b) {
            return a->info.ts_last < b->info.ts_last;
        });
        std::vector<SegmentRow> drop;
        for (const auto* s : cand) {
            if (total <= pol.max_per_session) break;
            total -= s->info.file_bytes;
            drop.push_back(*s);
            removed.insert(s->id);
        }
        rep.over_session_cap += static_cast<uint32_t>(drop.size());
        if (auto r = remove_segments(drop, rep); !r) return r;
    }

    // 4. The total cap: closed sessions' oldest segments, then live sessions'.
    uint64_t total = 0, pinned = 0;
    std::vector<const SegmentRow*> closed, live;
    for (const auto& s : st->segs) {
        if (removed.count(s.id)) continue;
        total += s.info.file_bytes;
        if (protected_segment(*st, s)) {
            pinned += s.info.file_bytes;
            continue;
        }
        if (!s.info.sealed) continue;
        (session_closed(*st, s.session) ? closed : live).push_back(&s);
    }
    auto oldest = [](const SegmentRow* a, const SegmentRow* b) { return a->info.ts_last < b->info.ts_last; };
    std::sort(closed.begin(), closed.end(), oldest);
    std::sort(live.begin(), live.end(), oldest);
    std::vector<SegmentRow> drop;
    for (const auto* list : {&closed, &live}) {
        for (const auto* s : *list) {
            if (total <= policy_.max_total) break;
            total -= s->info.file_bytes;
            drop.push_back(*s);
        }
    }
    rep.over_total_cap += static_cast<uint32_t>(drop.size());
    if (auto r = remove_segments(drop, rep); !r) return r;
    rep.pinned_bytes = pinned;
    rep.pins_over_cap = pinned > policy_.max_total;
    rep.total_bytes = total;
    return {};
}

Result<void> Janitor::compact(uint64_t now_ms, JanitorReport& rep) {
    auto st = load_state(catalog_);
    if (!st) return Unexpected(st.error());
    auto policy_of = [&](const std::string& id) {
        const auto it = st->sessions.find(id);
        return it != st->sessions.end() ? policy_for(it->second) : policy_;
    };

    // Trim partly expired segments (keep pinned blocks).
    for (const auto& s : st->segs) {
        if (!s.info.sealed) continue;
        const Policy pol = policy_of(s.session);
        if (const auto it = st->sessions.find(s.session); it != st->sessions.end() && it->second.pinned) continue;
        const uint64_t age = pol.max_age_of(s.info.kind);
        if (now_ms < age) continue;
        const uint64_t cutoff = now_ms - age;
        if (!(s.info.ts_first < cutoff && s.info.ts_last >= cutoff)) continue;
        const std::string session = s.session;
        const State& state = *st;
        auto r = rewrite_segments(
            catalog_, root_, {s}, options_of(s.info),
            [&](const BlockEntry& e) { return e.ts_last >= cutoff || block_pinned(state, session, e); }, now_ms);
        if (!r) return Unexpected(r.error());
        ++rep.trimmed;
    }

    // Merge runs of small segments of one session and kind.
    st = load_state(catalog_);
    if (!st) return Unexpected(st.error());
    std::map<std::pair<std::string, uint8_t>, std::vector<SegmentRow>> series;
    for (const auto& s : st->segs) {
        if (s.info.sealed) series[{s.session, static_cast<uint8_t>(s.info.kind)}].push_back(s);
    }
    for (auto& [key, list] : series) {
        std::sort(list.begin(), list.end(),
                  [](const SegmentRow& a, const SegmentRow& b) { return a.info.series < b.info.series; });
        const Policy pol = policy_of(key.first);
        const bool ended = session_closed(*st, key.first);
        std::vector<SegmentRow> run;
        uint64_t run_raw = 0;
        auto flush_run = [&]() -> Result<void> {
            if (run.size() >= 2) {
                auto r = rewrite_segments(catalog_, root_, run, options_of(run.front().info),
                                          [](const BlockEntry&) { return true; }, now_ms);
                if (!r) return Unexpected(r.error());
                rep.merged_inputs += static_cast<uint32_t>(run.size());
                ++rep.merged_outputs;
            }
            run.clear();
            run_raw = 0;
            return {};
        };
        for (const auto& s : list) {
            const bool small = s.info.raw_bytes < pol.small_segment_raw &&
                               (ended || s.info.ts_last + pol.merge_min_age_ms < now_ms);
            if (!small || run_raw + s.info.raw_bytes > kSegmentRawTarget) {
                if (auto r = flush_run(); !r) return r;
            }
            if (small) {
                run.push_back(s);
                run_raw += s.info.raw_bytes;
            }
        }
        if (auto r = flush_run(); !r) return r;
    }

    // Repack aged segments into the cold tier: oldest first, within the pass budget.
    auto segs = catalog_.segments({});
    if (!segs) return Unexpected(segs.error());
    std::vector<SegmentRow> todo;
    for (const auto& s : *segs) {
        const Policy pol = policy_of(s.session);
        if (!s.info.sealed || same_tier(s.info, pol.cold) || s.info.ts_last + pol.cold_after_ms >= now_ms) continue;
        if (!codec_available(pol.cold.codec)) continue;
        todo.push_back(s);
    }
    std::sort(todo.begin(), todo.end(),
              [](const SegmentRow& a, const SegmentRow& b) { return a.info.ts_last < b.info.ts_last; });
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& s : todo) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        if (stopping() || static_cast<uint64_t>(spent.count()) >= policy_.repack_budget_ms) {
            rep.repack_pending = true;
            break;
        }
        const Policy pol = policy_of(s.session);
        auto r = rewrite_segments(catalog_, root_, {s}, pol.cold, [](const BlockEntry&) { return true; }, now_ms);
        if (!r) {
            // A segment that went away meanwhile (forget, another janitor) is not an error.
            if (r.error().code == Errc::busy) continue;
            return Unexpected(r.error());
        }
        ++rep.recompressed;
    }
    return {};
}

Result<std::optional<SegmentRow>> rewrite_segments(Catalog& catalog, const fs::path& root,
                                                   const std::vector<SegmentRow>& inputs, const WriterOptions& out,
                                                   const std::function<bool(const BlockEntry&)>& keep,
                                                   uint64_t now_ms) {
    if (inputs.empty()) return std::optional<SegmentRow>();
    const SegmentRow& first = inputs.front();
    // A unique name: a merge output can be recompressed in the same pass (same now_ms).
    const fs::path sdir = root / platform::paths::from_utf8(first.session);
    fs::path path;
    std::string name;
    for (uint32_t n = 0;; ++n) {
        name = segment_file_name(first.info.kind, first.info.series,
                                 "c" + std::to_string(now_ms) + (n ? "-" + std::to_string(n) : std::string()));
        path = sdir / name;
        std::error_code ec;
        if (!fs::exists(path, ec)) break;
    }
    FileHeader h;
    h.kind = first.info.kind;
    h.series = first.info.series;
    h.session = first.session;
    h.created_unix_ms = now_ms;
    WriterOptions wo = out;
    wo.index = IndexMode::on_append;  // the records pass through here anyway
    wo.segment_raw = UINT64_MAX;
    auto w = SegmentWriter::create(path, h, wo);
    if (!w) return Unexpected(w.error());
    std::string raw;
    std::vector<RecordView> recs;
    std::vector<int64_t> ids;
    for (const auto& in : inputs) {
        ids.push_back(in.id);
        auto rd = SegmentReader::open(root / platform::paths::from_utf8(in.file));
        if (!rd) {
            std::error_code ec;
            w->reset();
            fs::remove(path, ec);
            return Unexpected(rd.error());
        }
        const auto& blocks = (*rd)->blocks();
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (!keep(blocks[i])) continue;
            // A damaged block is dropped: it could not be read back anyway.
            if (!(*rd)->read_block(i, raw) || !decode_records(raw, blocks[i].first_seq, blocks[i].ts_first, recs)) continue;
            for (const auto& r : recs) {
                if (auto a = (*w)->append(r.seq, r.ts, r.payload); !a) return Unexpected(a.error());
            }
        }
    }
    auto drop_inputs = [&]() -> Result<void> {
        if (auto d = catalog.delete_segments(ids); !d) return d;
        for (const auto& in : inputs) {
            std::error_code ec;
            fs::remove(root / platform::paths::from_utf8(in.file), ec);
        }
        return {};
    };
    if ((*w)->empty()) {
        w->reset();
        std::error_code ec;
        fs::remove(path, ec);
        if (auto d = drop_inputs(); !d) return Unexpected(d.error());
        return std::optional<SegmentRow>();
    }
    auto info = (*w)->seal();
    if (!info) return Unexpected(info.error());
    SegmentRow row;
    row.session = first.session;
    row.file = first.session + "/" + name;
    row.created_ms = now_ms;
    row.info = *info;
    auto id = catalog.replace_segments(ids, row);
    if (!id) {
        std::error_code ec;
        fs::remove(path, ec);
        return Unexpected(id.error());
    }
    row.id = *id;
    for (const auto& in : inputs) {
        std::error_code ec;
        fs::remove(root / platform::paths::from_utf8(in.file), ec);  // a failure leaves an orphan
    }
    return std::optional<SegmentRow>(std::move(row));
}

Result<uint64_t> forget_session(Catalog& catalog, const fs::path& root, const std::string& id) {
    if (!valid_session_dir(id)) return make_error(Errc::invalid_argument, "bad session id");
    auto rows = catalog.segments(SegmentFilter{std::nullopt, std::nullopt, {id}, std::nullopt, {}, false});
    if (!rows) return Unexpected(rows.error());
    uint64_t bytes = 0;
    for (const auto& r : *rows) bytes += r.info.file_bytes;
    auto files = catalog.forget_session(id);
    if (!files) return Unexpected(files.error());
    std::error_code ec;
    for (const auto& f : *files) fs::remove(root / platform::paths::from_utf8(f), ec);
    fs::remove_all(root / platform::paths::from_utf8(id), ec);
    return bytes;
}

JanitorThread::JanitorThread(fs::path root, Options o) : root_(std::move(root)), opt_(std::move(o)) {
    thread_ = std::thread([this] { run(); });
}

JanitorThread::~JanitorThread() { stop(); }

void JanitorThread::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    stop_flag_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void JanitorThread::run() {
    lower_thread_priority();
    std::unique_ptr<LockFile> lock;
    std::unique_ptr<Catalog> catalog;
    for (;;) {
        if (!lock) {
            std::error_code ec;
            fs::create_directories(root_, ec);
            auto l = LockFile::try_acquire(root_ / kJanitorLock);
            if (l && *l) lock = std::move(*l);
        }
        leader_.store(lock != nullptr);
        if (lock) {
            if (!catalog) {
                auto c = Catalog::open(root_ / kCatalogFile);
                if (c) catalog = std::move(*c);
            }
            if (catalog) {
                const Policy pol = opt_.policy ? opt_.policy() : Policy{};
                Janitor j(*catalog, root_, pol);
                j.set_stop(&stop_flag_);
                auto rep = j.run_pass(now_unix_ms());
                passes_.fetch_add(1);
                if (rep && opt_.on_report) opt_.on_report(*rep);
                if (opt_.after_pass) opt_.after_pass(*catalog, pol);
            }
        }
        std::unique_lock lk(mu_);
        if (cv_.wait_for(lk, opt_.interval, [this] { return stop_; })) break;
    }
    leader_.store(false);
}

}  // namespace kt::store
