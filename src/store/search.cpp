#include "store/search.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <pcre2.h>

#include "platform/paths.h"
#include "store/fold.h"
#include "store/regex_query.h"

namespace kt::store {

namespace {

struct MatchDataFree {
    void operator()(pcre2_match_data* m) const { pcre2_match_data_free(m); }
};

// Verifies one record. One per worker thread (PCRE2 match data is per thread).
class Matcher {
public:
    static Result<std::shared_ptr<const pcre2_code>> compile(const SearchQuery& q) {
        int err = 0;
        PCRE2_SIZE off = 0;
        uint32_t opts = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | PCRE2_UCP;
        if (!q.case_sensitive) opts |= PCRE2_CASELESS;
        pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(q.pattern.data()), q.pattern.size(), opts, &err,
                                         &off, nullptr);
        if (!code) {
            PCRE2_UCHAR msg[256];
            pcre2_get_error_message(err, msg, sizeof msg);
            return make_error(Errc::invalid_argument, "regex at offset " + std::to_string(off) + ": " +
                                                          reinterpret_cast<const char*>(msg));
        }
        return std::shared_ptr<const pcre2_code>(code, [](const pcre2_code* c) { pcre2_code_free(const_cast<pcre2_code*>(c)); });
    }

    Matcher(const SearchQuery& q, std::shared_ptr<const pcre2_code> code)
        : regex_(q.regex), ci_(!q.case_sensitive), code_(std::move(code)) {
        needle_ = ci_ ? folded(q.pattern) : q.pattern;
        if (code_) md_.reset(pcre2_match_data_create_from_pattern(code_.get(), nullptr));
    }

    bool match(std::string_view line, std::size_t& off, std::size_t& len) {
        if (regex_) {
            const int rc = pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(line.data()), line.size(), 0, 0,
                                       md_.get(), nullptr);
            if (rc < 0) return false;
            const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md_.get());
            off = ov[0];
            len = ov[1] >= ov[0] ? ov[1] - ov[0] : 0;
            return true;
        }
        std::string_view hay = line;
        if (ci_) {
            buf_.clear();
            fold_utf8(line, buf_);
            hay = buf_;
        }
        const auto at = hay.find(needle_);
        if (at == std::string_view::npos) return false;
        off = at;
        len = needle_.size();
        return true;
    }

private:
    bool regex_, ci_;
    std::string needle_;
    std::string buf_;
    std::shared_ptr<const pcre2_code> code_;
    std::unique_ptr<pcre2_match_data, MatchDataFree> md_;
};

trigram::Query plan_for(const SearchQuery& q) {
    return q.regex ? regex_trigram_query(q.pattern) : trigram::literal_query(folded(q.pattern));
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// A segment opened for the query, with its candidate blocks.
struct Opened {
    std::unique_ptr<SegmentReader> reader;
    std::vector<uint32_t> blocks;  // candidates inside the time range, newest first
    SearchStats st;                // counters only
};

// A run of candidate blocks of one segment: the unit of parallel work and of ordered output.
struct Unit {
    std::size_t seg = 0;
    std::size_t first = 0, count = 0;  // into Opened::blocks
};

struct UnitResult {
    std::vector<Hit> hits;
    SearchStats st;  // counters only
    bool done = false;
};

void open_segment(const std::filesystem::path& root, const SegmentRow& row, const SearchQuery& q,
                  const trigram::Query& plan, Opened& out) {
    auto& st = out.st;
    auto rd = SegmentReader::open(root / platform::paths::from_utf8(row.file));
    if (!rd) {
        ++st.segments_missing;  // deleted by the janitor meanwhile, or unreadable
        return;
    }
    auto& r = **rd;
    const auto& blocks = r.blocks();
    st.blocks_total += blocks.size();
    trigram::Bits cand(blocks.size(), true);
    if (r.index() != nullptr) {
        auto c = r.candidates(plan);
        if (c) cand = std::move(*c);
        else ++st.blocks_corrupt;  // a damaged index: scan everything
    } else {
        ++st.segments_full_scan;
    }
    st.blocks_candidate += cand.count();
    for (std::size_t bi = blocks.size(); bi-- > 0;) {
        if (!cand.test(bi)) continue;
        const auto& e = blocks[bi];
        if ((q.since_ms && e.ts_last < *q.since_ms) || (q.until_ms && e.ts_first > *q.until_ms)) continue;
        out.blocks.push_back(static_cast<uint32_t>(bi));
    }
    out.reader = std::move(*rd);
}

void scan_unit(const SegmentRow& row, Opened& seg, const Unit& u, const SearchQuery& q, Matcher& m,
               const std::atomic<bool>& stop, std::string& raw, std::string& scratch, std::vector<RecordView>& recs,
               UnitResult& out) {
    auto& st = out.st;
    const auto& blocks = seg.reader->blocks();
    for (std::size_t k = u.first; k < u.first + u.count; ++k) {
        if (stop.load(std::memory_order_relaxed)) return;
        const uint32_t bi = seg.blocks[k];
        const auto& e = blocks[bi];
        ++st.blocks_read;
        if (!seg.reader->read_block(bi, raw, scratch) || !decode_records(raw, e.first_seq, e.ts_first, recs)) {
            ++st.blocks_corrupt;
            continue;
        }
        st.raw_bytes_scanned += raw.size();
        for (std::size_t ri = recs.size(); ri-- > 0;) {
            const auto& rec = recs[ri];
            if ((q.since_ms && rec.ts < *q.since_ms) || (q.until_ms && rec.ts > *q.until_ms)) continue;
            std::size_t off = 0, len = 0;
            if (!m.match(rec.payload, off, len)) continue;
            Hit h;
            h.session = row.session;
            h.kind = row.info.kind;
            h.ts_ms = rec.ts;
            h.seq = rec.seq;
            h.text = std::string(rec.payload.substr(0, q.max_text));
            h.match_offset = off;
            h.match_len = len;
            h.segment_id = row.id;
            h.file = row.file;
            h.block = bi;
            h.record = static_cast<uint32_t>(ri);
            out.hits.push_back(std::move(h));
            if (out.hits.size() >= q.limit) return;
        }
    }
}

void add_counts(SearchStats& a, const SearchStats& b) {
    a.segments_full_scan += b.segments_full_scan;
    a.segments_missing += b.segments_missing;
    a.blocks_total += b.blocks_total;
    a.blocks_candidate += b.blocks_candidate;
    a.blocks_read += b.blocks_read;
    a.blocks_corrupt += b.blocks_corrupt;
    a.raw_bytes_scanned += b.raw_bytes_scanned;
}

}  // namespace

Result<std::string> explain(const SearchQuery& q) {
    if (q.pattern.empty()) return make_error(Errc::invalid_argument, "empty query");
    if (q.regex) {
        auto c = Matcher::compile(q);
        if (!c) return Unexpected(c.error());
    }
    return trigram::describe(plan_for(q));
}

Result<SearchStats> search(Catalog& catalog, const std::filesystem::path& root, const SearchQuery& q,
                           const std::function<bool(const Hit&)>& on_hit, const std::atomic<bool>* cancel) {
    const auto t0 = Clock::now();
    if (q.pattern.empty()) return make_error(Errc::invalid_argument, "empty query");
    std::shared_ptr<const pcre2_code> code;
    if (q.regex) {
        auto c = Matcher::compile(q);
        if (!c) return Unexpected(c.error());
        code = std::move(*c);
    }
    const trigram::Query plan = plan_for(q);

    SegmentFilter f;
    f.since_ms = q.since_ms;
    f.until_ms = q.until_ms;
    f.sessions = q.sessions;
    f.project = q.project;
    f.kinds = q.kinds;
    auto segs = catalog.segments(f);
    if (!segs) return Unexpected(segs.error());
    std::map<std::string, std::pair<std::string, std::string>> meta;  // session -> project, cwd
    if (auto ss = catalog.sessions()) {
        for (const auto& s : *ss) meta[s.id] = {s.project, s.cwd};
    }

    SearchStats st;
    st.plan = trigram::describe(plan);
    st.segments = static_cast<uint32_t>(segs->size());
    const std::size_t nseg = segs->size();
    unsigned threads = q.threads;
    if (threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 1 ? hw - 1 : 1;
    }
    std::atomic<bool> stop{false};
    auto is_cancelled = [&] { return cancel && cancel->load(); };

    // Segments are taken in groups, newest first: a group is opened (footer, index) in
    // parallel, then its candidate blocks are scanned in parallel and emitted in order. A query
    // that hits the limit early never opens the older groups.
    const std::size_t group = std::max<std::size_t>(std::size_t{threads} * 2, 8);
    for (std::size_t g0 = 0; g0 < nseg && !stop.load(); g0 += group) {
        const std::size_t g1 = std::min(nseg, g0 + group);
        std::vector<Opened> opened(g1 - g0);
        {
            std::atomic<std::size_t> next{g0};
            auto opener = [&] {
                for (std::size_t i; (i = next.fetch_add(1)) < g1 && !is_cancelled();) {
                    open_segment(root, (*segs)[i], q, plan, opened[i - g0]);
                }
            };
            std::vector<std::thread> pool;
            const auto nt = static_cast<unsigned>(std::min<std::size_t>(threads, g1 - g0));
            for (unsigned t = 1; t < nt; ++t) pool.emplace_back(opener);
            opener();
            for (auto& t : pool) t.join();
        }
        if (is_cancelled()) {
            st.cancelled = true;
            break;
        }
        for (const auto& o : opened) add_counts(st, o.st);

        // Units of work, newest first: small at first for a quick first hit, larger later.
        std::vector<Unit> units;
        for (std::size_t si = 0; si < opened.size(); ++si) {
            const std::size_t nb = opened[si].blocks.size();
            for (std::size_t k = 0; k < nb;) {
                const std::size_t want = (g0 == 0 && units.size() < threads) ? 2 : 16;
                const std::size_t c = std::min(want, nb - k);
                units.push_back({si, k, c});
                k += c;
            }
        }
        const std::size_t n = units.size();
        if (n == 0) continue;
        const auto nthreads = static_cast<unsigned>(std::min<std::size_t>(threads, n));
        const std::size_t window = std::size_t{nthreads} * 4;

        std::vector<UnitResult> slots(n);
        std::mutex mu;
        std::condition_variable cv;
        std::size_t next = 0, emitted = 0;

        auto worker = [&] {
            Matcher m(q, code);
            std::string raw, scratch;
            std::vector<RecordView> recs;
            for (;;) {
                std::size_t i = 0;
                {
                    std::unique_lock lk(mu);
                    cv.wait(lk, [&] { return stop.load() || next >= n || next < emitted + window; });
                    if (stop.load() || next >= n) return;
                    i = next++;
                }
                UnitResult res;
                const Unit& u = units[i];
                scan_unit((*segs)[g0 + u.seg], opened[u.seg], u, q, m, stop, raw, scratch, recs, res);
                {
                    std::lock_guard lk(mu);
                    slots[i] = std::move(res);
                    slots[i].done = true;
                }
                cv.notify_all();
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);

        for (std::size_t e = 0; e < n && !stop.load(); ++e) {
            UnitResult res;
            {
                std::unique_lock lk(mu);
                while (!slots[e].done) {
                    cv.wait_for(lk, std::chrono::milliseconds(20));
                    if (is_cancelled()) {
                        st.cancelled = true;
                        stop.store(true);
                        break;
                    }
                }
                if (stop.load()) break;
                res = std::move(slots[e]);
                emitted = e + 1;
            }
            cv.notify_all();
            add_counts(st, res.st);
            for (auto& h : res.hits) {
                auto it = meta.find(h.session);
                if (it != meta.end()) {
                    h.project = it->second.first;
                    h.cwd = it->second.second;
                }
                if (st.hits == 0) st.first_hit_ms = ms_since(t0);
                ++st.hits;
                const bool more = on_hit(h);
                if (st.hits >= q.limit) st.limit_reached = true;
                if (!more || st.limit_reached) {
                    stop.store(true);
                    break;
                }
            }
            if (is_cancelled()) {
                st.cancelled = true;
                stop.store(true);
            }
        }
        cv.notify_all();
        for (auto& t : pool) t.join();
    }
    st.total_ms = ms_since(t0);
    return st;
}

}  // namespace kt::store
