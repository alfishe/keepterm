#include "store/session_store.h"

#include <system_error>

#include "platform/paths.h"
#include "store/file_io.h"

namespace kt::store {

bool disk_below_min(const std::filesystem::path& root, const DiskGuard& g) {
    std::error_code ec;
    const auto sp = std::filesystem::space(root, ec);
    if (ec || sp.capacity == 0) return false;
    const auto pct = static_cast<uint64_t>(static_cast<double>(sp.capacity) * g.min_free_percent / 100.0);
    uint64_t limit = g.min_free_bytes;
    if (g.min_free_percent > 0 && (g.min_free_bytes == 0 || pct < limit)) limit = pct;
    return sp.available < limit;
}

Result<std::unique_ptr<SessionStore>> SessionStore::open(const std::filesystem::path& root, Catalog& catalog,
                                                         const SessionRow& session, const SessionStoreOptions& o) {
    if (session.id.empty() || session.id.find_first_of("/\\.") != std::string::npos) {
        return make_error(Errc::invalid_argument, "bad session id");
    }
    std::unique_ptr<SessionStore> s(new SessionStore());
    s->root_ = root;
    s->dir_ = root / session.id;
    s->catalog_ = &catalog;
    s->session_ = session;
    s->opt_ = o;
    if (auto r = platform::paths::make_private_dir(s->dir_); !r) return Unexpected(r.error());
    if (auto r = catalog.upsert_session(session); !r) return Unexpected(r.error());
    for (uint8_t k = 0; k < kKindCount; ++k) {
        if (auto r = s->resume(static_cast<Kind>(k)); !r) return Unexpected(r.error());
    }
    s->check_disk(now_unix_ms());
    return s;
}

SessionStore::~SessionStore() {
    // Leave open segments unsealed but flushed: the next open resumes them.
    (void)flush(true);
}

Result<void> SessionStore::resume(Kind kind) {
    auto row = catalog_->open_segment(session_.id, kind);
    if (!row) return Unexpected(row.error());
    if (!*row) return {};
    const auto path = root_ / platform::paths::from_utf8((*row)->file);
    auto w = SegmentWriter::resume(path, opt_.writer);
    if (w) {
        stats_.truncated_bytes += (*w)->truncated_bytes();
        open_[static_cast<std::size_t>(kind)] = Open{std::move(*w), (*row)->id};
        return {};
    }
    if (w.error().code == Errc::busy) {
        // Sealed, but the catalog missed it (a crash between the two): record the final state.
        auto rd = SegmentReader::open(path);
        if (rd) return catalog_->update_segment((*row)->id, (*rd)->info());
    }
    // Missing or unreadable: drop the row; a leftover file is an orphan for the janitor.
    return catalog_->delete_segments({(*row)->id});
}

void SessionStore::check_disk(uint64_t now) {
    if (!opt_.disk) return;
    const bool low = disk_below_min(root_, *opt_.disk);
    const bool was = stopped_.exchange(low);
    if (was != low && opt_.on_writes_stopped) opt_.on_writes_stopped(low);
    next_retry_ms_ = now + opt_.disk_retry_ms;
    since_check_ = 0;
}

Result<SessionStore::Open*> SessionStore::writer(Kind kind, uint64_t now) {
    Open& o = open_[static_cast<std::size_t>(kind)];
    if (o.w && o.w->full()) {
        if (auto r = seal(o); !r) return Unexpected(r.error());
    }
    if (o.w) return &o;
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto row = catalog_->begin_segment(session_.id, kind, now);
        if (!row) return Unexpected(row.error());
        FileHeader h;
        h.kind = kind;
        h.series = row->info.series;
        h.session = session_.id;
        h.created_unix_ms = now;
        auto w = SegmentWriter::create(root_ / platform::paths::from_utf8(row->file), h, opt_.writer);
        if (w) {
            o = Open{std::move(*w), row->id};
            return &o;
        }
        (void)catalog_->delete_segments({row->id});
        // A leftover file with this name (an orphan of a crash): try the next series number.
        std::error_code ec;
        if (!std::filesystem::exists(root_ / platform::paths::from_utf8(row->file), ec)) return Unexpected(w.error());
    }
    return make_error(Errc::busy, "cannot create a segment file");
}

Result<void> SessionStore::seal(Open& o) {
    if (!o.w) return {};
    if (o.w->empty()) {
        // Nothing was written: drop the row first, then the file.
        const auto path = o.w->path();
        o.w.reset();
        if (auto r = catalog_->delete_segments({o.row}); !r) return r;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        o.row = 0;
        return {};
    }
    auto info = o.w->seal();
    if (!info) return Unexpected(info.error());
    auto r = catalog_->update_segment(o.row, *info);
    o.w.reset();
    o.row = 0;
    ++stats_.segments_sealed;
    return r;
}

Result<void> SessionStore::append(Kind kind, uint64_t seq, uint64_t ts_ms, std::string_view payload) {
    std::lock_guard lk(mu_);
    const uint64_t now = now_unix_ms();
    if (stopped_.load()) {
        if (now >= next_retry_ms_) check_disk(now);
        if (stopped_.load()) {
            stats_.dropped_bytes += payload.size();
            return {};
        }
    }
    since_check_ += payload.size();
    if (since_check_ >= opt_.disk_check_bytes) {
        check_disk(now);
        if (stopped_.load()) {
            stats_.dropped_bytes += payload.size();
            // Make what was written readable before stopping.
            for (auto& o : open_) {
                if (o.w) (void)o.w->flush(true);
            }
            return {};
        }
    }
    auto o = writer(kind, now);
    if (!o) return Unexpected(o.error());
    stats_.appended_bytes += payload.size();
    const uint32_t blocks_before = (*o)->w->info().blocks;
    if (auto r = (*o)->w->append(seq, ts_ms, payload); !r) return r;
    // Publish the first block of a segment at once, so other processes see the time range.
    if (blocks_before == 0 && (*o)->w->info().blocks == 1) return catalog_->update_segment((*o)->row, (*o)->w->info());
    return {};
}

Result<void> SessionStore::flush(bool durable) {
    std::lock_guard lk(mu_);
    for (auto& o : open_) {
        if (!o.w) continue;
        if (auto r = o.w->flush(durable); !r) return r;
        if (!o.w->empty()) {
            if (auto r = catalog_->update_segment(o.row, o.w->info()); !r) return r;
        }
    }
    return {};
}

Result<void> SessionStore::close(std::optional<uint64_t> ended_ms) {
    std::lock_guard lk(mu_);
    for (auto& o : open_) {
        if (auto r = seal(o); !r) return r;
    }
    if (ended_ms) return catalog_->end_session(session_.id, *ended_ms);
    return {};
}

SessionStoreStats SessionStore::stats() const {
    std::lock_guard lk(mu_);
    return stats_;
}

std::pair<int64_t, trigram::Bits> SessionStore::open_candidates(Kind kind, const trigram::Query& q) const {
    std::lock_guard lk(mu_);
    const Open& o = open_[static_cast<std::size_t>(kind)];
    if (!o.w) return {0, trigram::Bits()};
    return {o.row, o.w->candidates(q)};
}

}  // namespace kt::store

namespace kt::store {

std::pair<int64_t, uint32_t> SessionStore::position(Kind kind) const {
    std::lock_guard lk(mu_);
    const Open& o = open_[static_cast<std::size_t>(kind)];
    if (!o.w) return {0, 0};
    return {o.row, o.w->info().blocks};
}

}  // namespace kt::store
