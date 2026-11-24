#include "agents/context_tracker.h"

#include <algorithm>

namespace kt::agents {
namespace {
constexpr std::size_t kHead = 256;
}

bool ContextTracker::follow(const IContextProbe* probe, const std::optional<ContextTarget>& t, std::int64_t now_ms) {
    if (t && target_ && *t == *target_ && reducer_) return false;
    const bool had = reducer_ != nullptr;
    target_.reset();
    reducer_.reset();
    offset_ = 0;
    file_id_ = 0;
    head_.clear();
    bytes_read_ = 0;
    pending_ = false;
    if (!t || !probe) return had;
    target_ = t;
    reducer_ = probe->reducer(*t);
    if (!reducer_) {
        target_.reset();
        return had;
    }
    last_read_ms_ = now_ms;
    last_check_ms_ = now_ms;
    attach();
    return true;
}

std::string ContextTracker::head_of() const {
    auto h = fs_.read_range(target_->path, 0, kHead);
    return h ? *h : std::string();
}

std::string ContextTracker::db_signature() const {
    std::string s;
    for (const std::string& p : {target_->path, target_->path + "-wal"}) {
        if (auto st = fs_.stat(p)) s += std::to_string(st->size) + ":" + std::to_string(st->mtime_ns) + ";";
        else s += "-;";
    }
    return s;
}

bool ContextTracker::attach() {
    if (reducer_->rereads()) {
        db_sig_ = db_signature();
        reducer_->reread(target_->path);
        return true;
    }
    reducer_->reset();
    offset_ = 0;
    auto st = fs_.stat(target_->path);
    if (!st) return true;
    file_id_ = st->id;
    head_ = head_of();
    const std::uint64_t size = st->size;
    std::size_t tail = o_.tail_min;
    for (;;) {
        if (size <= tail) {
            auto all = fs_.read_range(target_->path, 0, static_cast<std::size_t>(size));
            if (!all) return true;
            reducer_->reset();
            reducer_->feed(*all);
            bytes_read_ += all->size();
            offset_ = all->size();
            return true;
        }
        const std::uint64_t from = size - tail;
        auto bytes = fs_.read_range(target_->path, from, tail);
        if (!bytes) return true;
        bytes_read_ += bytes->size();
        reducer_->reset_from_tail(*bytes);
        offset_ = from + bytes->size();
        if (reducer_->saw_usage() || tail >= o_.tail_max) return true;
        tail = std::min(tail * 2, o_.tail_max);
    }
}

bool ContextTracker::read_more(const IFileSystem::FileStat& st) {
    if (st.size <= offset_) return false;
    if (st.size - offset_ > o_.max_read) {
        attach();
        return true;
    }
    bool any = false;
    while (offset_ < st.size) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(st.size - offset_, 4u * 1024 * 1024));
        auto bytes = fs_.read_range(target_->path, offset_, want);
        if (!bytes || bytes->empty()) break;
        reducer_->feed(*bytes);
        offset_ += bytes->size();
        bytes_read_ += bytes->size();
        any = true;
    }
    return any;
}

bool ContextTracker::poll(std::int64_t now_ms, bool event) {
    if (!reducer_ || !target_) return false;
    if (event) pending_ = true;
    const bool safety = now_ms - last_check_ms_ >= o_.safety_interval_ms;
    if (!safety && !(pending_ && now_ms - last_read_ms_ >= o_.min_interval_ms)) return false;
    last_check_ms_ = now_ms;
    last_read_ms_ = now_ms;
    pending_ = false;
    const ContextUsage before = reducer_->usage();
    if (reducer_->rereads()) {
        std::string sig = db_signature();
        if (sig != db_sig_) {
            db_sig_ = std::move(sig);
            reducer_->reread(target_->path);
        }
        return !(reducer_->usage() == before);
    }
    auto st = fs_.stat(target_->path);
    if (!st) return false;   // gone for now; the keeper's next locate decides
    bool replaced = st->size < offset_ || (st->id && file_id_ && st->id != file_id_);
    if (!replaced && (safety || st->size != offset_) && !head_.empty()) {
        // Same size class but other bytes at the start: the file was rewritten in place.
        const std::string h = head_of();
        replaced = h.substr(0, std::min(h.size(), head_.size())) != head_.substr(0, std::min(h.size(), head_.size()));
    }
    if (replaced) {
        attach();
    } else {
        if (head_.size() < kHead) head_ = head_of();
        read_more(*st);
    }
    return !(reducer_->usage() == before);
}

}  // namespace kt::agents
