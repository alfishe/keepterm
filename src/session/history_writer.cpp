#include "session/history_writer.h"

#include <algorithm>
#include <string_view>

#include "base/fault.h"
#include "base/log.h"
#include "platform/paths.h"
#include "session/cwd.h"
#include "session/history_events.h"
#include "session/journal.h"
#include "session/os.h"
#include "store/file_io.h"
#include "vt/snapshot.h"

namespace kt::session {

using Clock = std::chrono::steady_clock;

namespace {

// The shadow gets at most kPiece bytes between two collects: a byte adds at most one row, so with
// kShadowRows rows of scrollback (a resize pushes at most a screen more) no row is evicted before
// the tap has read it.
constexpr std::size_t kPiece = 512;
constexpr uint32_t kShadowRows = 2048;

}  // namespace

HistoryWriter::HistoryWriter(HistoryWriterOptions o, std::mutex& mu)
    : opt_(std::move(o)), term_mu_(mu), queue_(opt_.queue_bytes), watcher_(opt_.on_usage) {}

Result<std::unique_ptr<HistoryWriter>> HistoryWriter::start(HistoryWriterOptions o, std::mutex& mu, const vt::Terminal& term) {
    std::unique_ptr<HistoryWriter> w(new HistoryWriter(std::move(o), mu));
    if (auto r = platform::paths::make_private_dir(w->opt_.root); !r) return Unexpected(r.error());
    auto cat = store::Catalog::open(w->opt_.root / store::kCatalogFile);
    if (!cat) return Unexpected(cat.error());
    w->catalog_ = std::move(*cat);
    store::SessionStoreOptions so;
    so.writer = w->opt_.fresh;
    so.disk = w->opt_.policy.disk;
    HistoryWriter* self = w.get();
    so.on_writes_stopped = [self](bool stopped) {
        log::warn("history", stopped ? "warm history writes stopped: the disk is below storage.min_free_disk"
                                     : "warm history writes resumed");
        self->watcher_.writes_stopped(stopped);
    };
    auto st = store::SessionStore::open(w->opt_.root, *w->catalog_, w->opt_.session, so);
    if (!st) return Unexpected(st.error());
    w->store_ = std::move(*st);
    // The raw kind continues where the store ends: output stored already is not repeated.
    if (auto e = w->store_->end_seq(store::Kind::raw); e && *e > 0) w->next_raw_ = *e;
    // A restored series goes on from the size it has stored: the same again is no event.
    if (auto e = w->store_->end_seq(store::Kind::events); e && *e > 0) {
        (void)store::for_each_record(*w->store_, store::Kind::events, *e - 1, [&](const store::RecordView& v) {
            if (auto sz = parse_resize_event(v.payload)) w->last_size_ = vt::GridSize{sz->cols, sz->rows};
            return true;
        });
    }
    w->projector_ = std::make_unique<store::TextProjector>(*w->store_);
    w->pipe_sink_.w = w.get();
    w->tap_ = std::make_unique<store::ScrollbackTap>(w->pipe_sink_);
    w->batch_bytes_ = std::clamp<std::size_t>(w->opt_.pipe_bytes / 4, 4u << 10, 256u << 10);
    w->open_batch_ = std::make_unique<Batch>();
    w->command_log_ = std::make_unique<CommandLog>(*w->store_, *w->catalog_, w->opt_.session.id);
    w->cwd_ = w->opt_.session.cwd;

    vt::TerminalOptions to = w->opt_.term;
    to.scrollback_lines = kShadowRows;
    to.scrollback_max_bytes = 256u << 20;  // the row count bounds it
    {
        std::lock_guard lk(mu);
        w->term_ = &term;
        const auto snap = term.snapshot();
        auto sh = vt::Terminal::restore(snap, to, w.get(), vt::RestoreOptions{});
        if (!sh) return Unexpected(sh.error());
        w->shadow_ = std::move(*sh);
        w->fed_ = term.stream_offset();
    }
    w->record_end_ = w->fed_;
    w->stored_ = w->fed_;
    w->tap_->attach(*w->shadow_);
    w->store_thread_ = std::thread([self] { self->run_store(); });
    w->thread_ = std::thread([self] { self->run(); });
    return w;
}

HistoryWriter::~HistoryWriter() { stop(std::nullopt); }

// ------------------------------------------------------------------------------- the shadow

void HistoryWriter::cwd_changed(const vt::CwdReport& r) {
    const std::string_view v(reinterpret_cast<const char*>(r.value.data()), r.value.size());
    std::optional<std::string> path;
    if (r.form == vt::CwdReport::Form::Osc7) {
        std::string host;
        path = cwd_from_osc7(v, &host);
        if (path && !is_local_host(host)) path.reset();
    } else {
        path = cwd_from_osc9_9(v);
    }
    if (path && !path->empty()) cwd_ = *path;
}

void HistoryWriter::command_started(std::u8string_view line) {
    std::string cwd = cwd_;
    if (opt_.cwd) {
        if (std::string c = opt_.cwd(); !c.empty()) cwd = std::move(c);
    }
    emit(Op::Type::command_started, record_end_, record_ts_,
         std::string_view(reinterpret_cast<const char*>(line.data()), line.size()), cwd);
}

void HistoryWriter::command_finished(std::optional<int> exit_code) {
    Op& op = emit(Op::Type::command_finished, record_end_, record_ts_);
    op.flag = exit_code.has_value();
    op.exit_code = exit_code.value_or(0);
}

// ------------------------------------------------------------------------------- the pipe

Result<void> HistoryWriter::PipeSink::line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) {
    w->emit(Op::Type::line, seq, ts_ms, utf8);
    return {};
}

HistoryWriter::Op& HistoryWriter::emit(Op::Type t, uint64_t seq, uint64_t ts, std::string_view a, std::string_view b) {
    if (open_batch_->bytes.size() + a.size() + b.size() > batch_bytes_ && !open_batch_->ops.empty()) ship();
    Batch& bt = *open_batch_;
    Op op;
    op.type = t;
    op.seq = seq;
    op.ts = ts;
    op.off = static_cast<uint32_t>(bt.bytes.size());
    op.len = static_cast<uint32_t>(a.size());
    bt.bytes.append(a);
    op.off2 = static_cast<uint32_t>(bt.bytes.size());
    op.len2 = static_cast<uint32_t>(b.size());
    bt.bytes.append(b);
    bt.ops.push_back(op);
    return bt.ops.back();
}

void HistoryWriter::ship() {
    if (open_batch_->ops.empty()) return;
    const std::size_t n = open_batch_->bytes.size() + open_batch_->ops.size() * sizeof(Op);
    std::unique_lock lk(pipe_mu_);
    // At least one batch is always let in, however big (a raw record may exceed the pipe).
    room_cv_.wait(lk, [&] { return in_pipe_ == 0 || in_pipe_ + n <= opt_.pipe_bytes; });
    in_pipe_ += n;
    pipe_peak_ = std::max(pipe_peak_, in_pipe_);
    pipe_.push_back(std::move(open_batch_));
    if (!spare_.empty()) {
        open_batch_ = std::move(spare_.back());
        spare_.pop_back();
    } else {
        open_batch_ = std::make_unique<Batch>();
    }
    lk.unlock();
    pipe_cv_.notify_one();
}

void HistoryWriter::execute(const Batch& b) {
    (void)KT_FAULT_POINT("history.store");  // delay=<ms>: a slow store thread (disk, compression)
    auto str = [&](uint32_t off, uint32_t len) { return std::string_view(b.bytes.data() + off, len); };
    for (const Op& op : b.ops) {
        switch (op.type) {
            case Op::Type::raw:
                if (auto r = store_->append(store::Kind::raw, op.seq, op.ts, str(op.off, op.len)); !r)
                    log::warn("history", "raw append: " + r.error().what);
                break;
            case Op::Type::line:
                if (auto r = projector_->line(str(op.off, op.len), op.seq, op.ts); !r)
                    log::warn("history", "text: " + r.error().what);
                break;
            case Op::Type::command_started:
                command_log_->started(str(op.off, op.len), str(op.off2, op.len2), op.seq, op.ts);
                break;
            case Op::Type::event:
                if (auto r = store_->append(store::Kind::events, op.seq, op.ts, str(op.off, op.len)); !r)
                    log::warn("history", "event: " + r.error().what);
                break;
            case Op::Type::command_finished:
                command_log_->finished(op.flag ? std::optional<int>(op.exit_code) : std::nullopt, op.ts);
                break;
            case Op::Type::flush: {
                if (auto r = store_->flush(op.flag); !r) log::warn("history", std::string(op.flag ? "durable " : "") + "flush: " + r.error().what);
                if (op.barrier) (void)KT_FAULT_POINT("history.checkpoint.stored");
                {
                    std::lock_guard lk(mu_);
                    stored_ = std::max(stored_, op.seq);
                }
                done_cv_.notify_all();
                break;
            }
            case Op::Type::close:
                if (op.flag) {
                    if (auto r = store_->close(op.ts); !r) log::warn("history", "close: " + r.error().what);
                } else if (auto r = store_->flush(true); !r) {
                    log::warn("history", "flush: " + r.error().what);
                }
                break;
        }
    }
    std::lock_guard lk(stats_mu_);
    stats_.lines = projector_->lines();
    stats_.commands = command_log_->added();
}

void HistoryWriter::run_store() {
    auto last_usage = Clock::now() - opt_.usage_interval;  // a first check at once
    for (;;) {
        std::unique_ptr<Batch> b;
        {
            std::unique_lock lk(pipe_mu_);
            pipe_cv_.wait_for(lk, std::chrono::seconds(1), [this] { return store_stop_ || !pipe_.empty(); });
            if (!pipe_.empty()) {
                b = std::move(pipe_.front());
                pipe_.pop_front();
            } else if (store_stop_) {
                break;
            }
        }
        if (b) {
            execute(*b);
            const std::size_t n = b->bytes.size() + b->ops.size() * sizeof(Op);
            b->bytes.clear();
            b->ops.clear();
            {
                std::lock_guard lk(pipe_mu_);
                in_pipe_ -= n;
                if (spare_.size() < 4) spare_.push_back(std::move(b));
            }
            room_cv_.notify_all();
        }
        const auto now = Clock::now();
        if (now - last_usage >= opt_.usage_interval) {
            last_usage = now;
            check_usage(store::now_unix_ms());
        }
    }
}

void HistoryWriter::collect(uint64_t ts) {
    if (auto r = tap_->collect(*shadow_, fed_, ts); !r) log::warn("history", "scrollback tap: " + r.error().what);
}

void HistoryWriter::process(journal::Kind kind, uint64_t seq, uint64_t ts, std::span<const std::byte> payload) {
    if (kind == journal::Kind::Resize) {
        if (payload.size() < 4 || seq < fed_) return;
        const auto* p = reinterpret_cast<const uint8_t*>(payload.data());
        vt::GridSize g;
        g.cols = static_cast<uint16_t>(p[0] | (p[1] << 8));
        g.rows = static_cast<uint16_t>(p[2] | (p[3] << 8));
        if (g.cols > 0 && g.rows > 0) {
            shadow_->resize(g);
            collect(ts);
            size_event(g, ts);
        }
        return;
    }
    if (kind != journal::Kind::Output || payload.empty()) return;
    const uint64_t end = seq + payload.size();
    if (end <= fed_) return;
    if (seq > fed_ && !filling_) {
        // A record the queue dropped (the PTY ran out of max_stall): the journal file has it.
        ++gaps_;
        filling_ = true;
        fill_gap(fed_, seq);
        filling_ = false;
        if (end <= fed_) return;
    }
    // Raw: exactly once by stream offset.
    {
        uint64_t rseq = seq;
        auto raw = payload;
        if (next_raw_ != UINT64_MAX && rseq < next_raw_) {
            const uint64_t cut = std::min<uint64_t>(next_raw_ - rseq, raw.size());
            raw = raw.subspan(static_cast<std::size_t>(cut));
            rseq += cut;
        }
        if (!raw.empty()) {
            emit(Op::Type::raw, rseq, ts, std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
            next_raw_ = rseq + raw.size();
            raw_bytes_ += raw.size();
        }
    }
    // The shadow, piece by piece, with the tap after each.
    auto bytes = seq < fed_ ? payload.subspan(static_cast<std::size_t>(fed_ - seq)) : payload;
    fed_ = std::max(fed_, seq);
    record_end_ = end;
    record_ts_ = ts;
    while (!bytes.empty()) {
        const std::size_t n = std::min(bytes.size(), kPiece);
        shadow_->feed(bytes.first(n));
        fed_ += n;
        bytes = bytes.subspan(n);
        collect(ts);
    }
}

void HistoryWriter::size_event(vt::GridSize g, uint64_t ts) {
    if (g.cols == last_size_.cols && g.rows == last_size_.rows) return;
    last_size_ = g;
    emit(Op::Type::event, fed_, ts, resize_event(TermSize{g.cols, g.rows}));
}

void HistoryWriter::fill_gap(uint64_t from, uint64_t to) {
    uint64_t filled = 0;
    if (!opt_.journal_path.empty()) {
        if (auto file = os::read_file(opt_.journal_path, 1u << 30)) {
            if (auto c = journal::read(std::as_bytes(std::span(file->data(), file->size())))) {
                for (const auto& r : c->records) {
                    if (r.kind == journal::Kind::Resize) {
                        if (r.seq >= from && r.seq < to) process(r.kind, r.seq, r.ts_unix_ms, r.payload);
                        continue;
                    }
                    if (r.kind != journal::Kind::Output || r.payload.empty()) continue;
                    if (r.stream_end() <= fed_ || r.seq >= to) continue;
                    if (r.seq > fed_) break;  // the ring no longer has the rest
                    const uint64_t before = fed_;
                    const uint64_t end = std::min(r.stream_end(), to);
                    process(r.kind, r.seq, r.ts_unix_ms, r.payload.first(static_cast<std::size_t>(end - r.seq)));
                    filled += fed_ - before;
                    if (fed_ >= to) break;
                }
            }
        }
    }
    if (fed_ < to) {
        log::warn("history", "a hole of " + std::to_string(to - fed_) + " bytes at " + std::to_string(fed_) + " in the history");
        gap_lost_ += to - fed_;
        fed_ = to;  // the shadow goes on without it
        // The line the hole cut ends there: what follows does not glue onto it.
        static constexpr char kBreak[] = "\r\n";
        shadow_->feed(std::as_bytes(std::span(kBreak, 2)));
        collect(record_ts_);
    }
    gap_filled_ += filled;
}

bool HistoryWriter::pump(std::size_t max_bytes) {
    (void)KT_FAULT_POINT("history.pump");  // delay=<ms>: a slow disk
    const std::size_t n = queue_.drain(
        [&](journal::Kind k, uint64_t seq, uint64_t ts, std::span<const std::byte> payload) { process(k, seq, ts, payload); },
        max_bytes);
    if (n) {
        ship();
        publish_stats();
    }
    return n > 0;
}

void HistoryWriter::publish_stats() {
    std::lock_guard lk(stats_mu_);
    stats_.raw_bytes = raw_bytes_;
    stats_.missed_rows = tap_->missed_rows();
    stats_.gaps = gaps_;
    stats_.gap_bytes_filled = gap_filled_;
    stats_.gap_bytes_lost = gap_lost_;
}

// ------------------------------------------------------------------------------- the barrier

bool HistoryWriter::checkpoint(const std::function<void()>& snapshot) {
    {
        std::lock_guard lk(mu_);
        if (stopped_) {
            std::lock_guard tl(term_mu_);
            snapshot();
            return true;
        }
    }
    uint64_t at = 0;
    {
        std::lock_guard tl(term_mu_);
        snapshot();
        // Every record up to here is in the queue (or, dropped, in the journal file).
        at = term_->stream_offset();
    }
    std::unique_lock lk(mu_);
    barrier_ = std::max(barrier_, at);
    cv_.notify_all();
    const bool ok = done_cv_.wait_for(lk, opt_.max_barrier, [&] { return stopped_ || stored_ >= at; });
    if (!ok) {
        lk.unlock();
        log::warn("history", "checkpoint at " + std::to_string(at) + ": the store did not catch up in time");
        std::lock_guard sl(stats_mu_);
        ++stats_.barrier_timeouts;
    }
    return ok;
}

// ------------------------------------------------------------------------------- the thread

void HistoryWriter::check_usage(uint64_t now_ms) {
    auto u = store::usage(*catalog_, opt_.root, opt_.policy, now_ms);
    if (u) watcher_.update(*u);
}

void HistoryWriter::run() {
    // Normal priority, not lower_thread_priority(): the PTY thread waits for this thread when the
    // queue is full, so a starved writer (nice 19 on a busy machine) would stall the PTY itself.
    auto last_flush = Clock::now();
    auto last_durable = Clock::now();
    auto last_output = Clock::now();
    bool dirty = false, undurable = false;
    // The size the output starts at (a replay of the raw kind needs it).
    size_event(shadow_->size(), store::now_unix_ms());
    for (;;) {
        uint64_t barrier = 0;
        {
            std::lock_guard lk(mu_);
            if (stop_) break;
            barrier = barrier_;
        }
        // Bounded, so a checkpoint, a flush and stop() get their turn under a flood.
        const bool any = pump(4u << 20);
        dirty = dirty || any;
        const auto now = Clock::now();
        if (any) last_output = now;
        bool flushed = false;
        const bool at_barrier = barrier > 0 && fed_ >= barrier;
        if ((dirty && now - last_flush >= opt_.flush_interval) || at_barrier) {
            (void)KT_FAULT_POINT("history.flush");
            // The store thread writes the open blocks after everything before, then moves stored_.
            Op& op = emit(Op::Type::flush, fed_, 0);
            op.barrier = at_barrier;
            last_flush = now;
            dirty = false;
            undurable = true;
            flushed = true;
        }
        if (undurable && now - last_durable >= opt_.durable_interval) {
            // Bounds what a power loss or an OS crash can take.
            Op& op = emit(Op::Type::flush, fed_, 0);
            op.flag = true;
            last_durable = now;
            undurable = false;
            flushed = true;
        }
        if (flushed) {
            ship();
            std::lock_guard lk(mu_);
            if (barrier_ <= fed_) barrier_ = 0;
        }
        if (!any) {
            // An idle pane holds none of the queue's pages (at most one release per idle spell).
            if (now - last_output >= opt_.queue_idle) (void)queue_.release(64u << 10);
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return stop_ || (barrier_ > 0 && fed_ >= barrier_) || !queue_.empty(); });
        }
    }
}

void HistoryWriter::stop(std::optional<uint64_t> ended_ms) {
    {
        std::lock_guard lk(mu_);
        if (stopped_) return;
        stop_ = true;
    }
    queue_.abort_waits();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (store_) {
        while (pump(SIZE_MAX)) {
        }
        const uint64_t now = store::now_unix_ms();
        if (ended_ms) {
            if (auto r = tap_->finish(*shadow_, fed_, now); !r) log::warn("history", "scrollback tap: " + r.error().what);
        }
        Op& op = emit(Op::Type::close, 0, ended_ms.value_or(0));
        op.flag = ended_ms.has_value();
        ship();
        publish_stats();
    }
    {
        std::lock_guard lk(pipe_mu_);
        store_stop_ = true;
    }
    pipe_cv_.notify_all();
    if (store_thread_.joinable()) store_thread_.join();  // it runs what is in the pipe first
    {
        std::lock_guard lk(mu_);
        stopped_ = true;
        stored_ = fed_;
    }
    done_cv_.notify_all();
    if (!store_) return;
    const auto s = stats();
    log::info("history", "stored " + std::to_string(s.raw_bytes) + " raw bytes, " + std::to_string(s.lines) + " lines, " +
                             std::to_string(s.commands) + " commands; dropped " + std::to_string(s.dropped_records) + " records, " +
                             std::to_string(s.gaps) + " gaps (" + std::to_string(s.gap_bytes_lost) + " bytes lost), " +
                             std::to_string(s.waits) + " waits, " + std::to_string(s.stalls) + " stalls, " +
                             std::to_string(s.missed_rows) + " rows missed, " + std::to_string(s.barrier_timeouts) +
                             " checkpoint timeouts");
}

void HistoryWriter::release() {
    {
        std::lock_guard lk(mu_);
        if (!stopped_) return;
    }
    command_log_.reset();
    tap_.reset();
    projector_.reset();
    store_.reset();
    catalog_.reset();
}

HistoryWriterStats HistoryWriter::stats() const {
    std::lock_guard lk(stats_mu_);
    HistoryWriterStats s = stats_;
    s.dropped_records = queue_.dropped_records();
    s.dropped_bytes = queue_.dropped_bytes();
    s.waits = queue_.waits();
    s.stalls = queue_.stalls();
    s.queue_releases = queue_.releases();
    s.queue_resident = queue_.memory().resident_bytes();
    std::lock_guard pl(pipe_mu_);
    s.pipe_peak = pipe_peak_;
    return s;
}

}  // namespace kt::session
