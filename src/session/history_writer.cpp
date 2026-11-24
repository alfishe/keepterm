#include "session/history_writer.h"

#include <algorithm>
#include <string_view>

#include "base/log.h"
#include "platform/paths.h"
#include "store/file_io.h"
#include "vt/terminal.h"

namespace kt::session {

using Clock = std::chrono::steady_clock;

// Collects the tap's lines under the terminal lock; they are stored after it is released.
class HistoryWriter::Lines final : public store::LineSink {
public:
    struct Line {
        std::string text;
        uint64_t seq, ts;
    };
    Result<void> line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) override {
        if (n_ == buf_.size()) buf_.emplace_back();
        Line& l = buf_[n_++];
        l.text.assign(utf8);
        l.seq = seq;
        l.ts = ts_ms;
        return {};
    }
    std::size_t size() const { return n_; }
    const Line& at(std::size_t i) const { return buf_[i]; }
    void clear() { n_ = 0; }

private:
    std::vector<Line> buf_;  // reused: the strings keep their capacity
    std::size_t n_ = 0;
};

HistoryWriter::HistoryWriter(HistoryWriterOptions o, std::mutex& mu, const vt::Terminal& term)
    : opt_(std::move(o)), term_mu_(mu), term_(term), queue_(opt_.queue_bytes), watcher_(opt_.on_usage) {}

Result<std::unique_ptr<HistoryWriter>> HistoryWriter::start(HistoryWriterOptions o, std::mutex& mu,
                                                            const vt::Terminal& term) {
    std::unique_ptr<HistoryWriter> w(new HistoryWriter(std::move(o), mu, term));
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
    w->lines_ = std::make_unique<Lines>();
    w->projector_ = std::make_unique<store::TextProjector>(*w->store_);
    w->tap_ = std::make_unique<store::ScrollbackTap>(*w->lines_);
    {
        std::lock_guard lk(mu);
        w->tap_->attach(term);
    }
    w->thread_ = std::thread([self] { self->run(); });
    return w;
}

HistoryWriter::~HistoryWriter() { stop(std::nullopt); }

void HistoryWriter::command_started(std::string command_line, std::string cwd, uint64_t seq, uint64_t ts_ms) {
    std::lock_guard lk(cmd_mu_);
    if (commands_.size() >= 256) return;  // the storage thread is far behind; keep the PTY thread cheap
    Command c;
    c.line = std::move(command_line);
    c.cwd = std::move(cwd);
    c.seq = seq;
    c.started = ts_ms;
    commands_.push_back(std::move(c));
}

void HistoryWriter::command_finished(std::optional<int> exit_code, uint64_t ts_ms) {
    std::lock_guard lk(cmd_mu_);
    if (commands_.size() >= 256) return;
    Command c;
    c.finished = true;
    c.exit_code = exit_code;
    c.ended = ts_ms;
    commands_.push_back(std::move(c));
}

bool HistoryWriter::pump_raw(std::size_t max_bytes) {
    uint64_t bytes = 0, gaps = 0;
    const std::size_t n = queue_.drain(
        [&](journal::Kind k, uint64_t seq, uint64_t ts, std::span<const std::byte> payload) {
            if (k != journal::Kind::Output || payload.empty()) return;
            if (next_seq_ != UINT64_MAX && seq != next_seq_) ++gaps;
            next_seq_ = seq + payload.size();
            bytes += payload.size();
            const std::string_view sv(reinterpret_cast<const char*>(payload.data()), payload.size());
            if (auto r = store_->append(store::Kind::raw, seq, ts, sv); !r) {
                log::warn("history", "raw append: " + r.error().what);
            }
        },
        max_bytes);
    std::lock_guard lk(stats_mu_);
    stats_.raw_bytes += bytes;
    stats_.gaps += gaps;
    return n > 0;
}

bool HistoryWriter::pump_text(uint64_t now_ms) {
    bool any = false;
    for (;;) {
        bool more = false;
        uint64_t seq = 0;
        const auto t0 = Clock::now();
        {
            std::lock_guard lk(term_mu_);
            seq = term_.stream_offset();
            if (auto r = tap_->collect(term_, seq, now_ms, opt_.rows_per_lock); !r) {
                log::warn("history", "scrollback tap: " + r.error().what);
            }
            more = tap_->more();
        }
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        const std::size_t n = lines_->size();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& l = lines_->at(i);
            if (auto r = projector_->line(l.text, l.seq, l.ts); !r) log::warn("history", "text append: " + r.error().what);
        }
        lines_->clear();
        {
            std::lock_guard lk(stats_mu_);
            ++stats_.lock_holds;
            stats_.max_lock_us = std::max(stats_.max_lock_us, us);
            stats_.lines = projector_->lines();
            stats_.missed_rows = tap_->missed_rows();
        }
        any = any || n > 0;
        if (!more) break;
        // Let the PTY thread in between two batches.
        std::this_thread::yield();
    }
    return any;
}

void HistoryWriter::pump_commands() {
    std::vector<Command> batch;
    {
        std::lock_guard lk(cmd_mu_);
        batch.swap(commands_);
    }
    auto finish_row = [&](Command& c) {
        store::CommandRow row;
        row.session = opt_.session.id;
        row.started_ms = c.started;
        row.ended_ms = c.ended;
        row.exit_code = c.exit_code;
        row.cwd = c.cwd;
        row.command = c.line;
        const auto pos = store_->position(store::Kind::text);
        row.segment_id = pos.first;
        row.block = pos.second;
        row.seq = c.seq;
        if (auto r = catalog_->add_command(row); !r) log::warn("history", "command row: " + r.error().what);
        std::lock_guard lk(stats_mu_);
        ++stats_.commands;
    };
    for (auto& c : batch) {
        if (!c.finished) {
            if (open_cmd_) finish_row(*open_cmd_);  // no D mark came: store it without an exit code
            if (!c.line.empty()) {
                if (auto r = store_->append(store::Kind::commands, c.seq, c.started, c.line); !r) {
                    log::warn("history", "command append: " + r.error().what);
                }
            }
            open_cmd_ = std::move(c);
        } else if (open_cmd_) {
            open_cmd_->ended = c.ended;
            open_cmd_->exit_code = c.exit_code;
            finish_row(*open_cmd_);
            open_cmd_.reset();
        }
    }
}

void HistoryWriter::check_usage(uint64_t now_ms) {
    auto u = store::usage(*catalog_, opt_.root, opt_.policy, now_ms);
    if (u) watcher_.update(*u);
}

void HistoryWriter::run() {
    store::lower_thread_priority();
    auto last_text = Clock::now();
    auto last_flush = Clock::now();
    auto last_usage = Clock::now() - opt_.usage_interval;  // a first check at once
    bool dirty = false;
    for (;;) {
        {
            std::unique_lock lk(mu_);
            if (stop_) break;
        }
        // Raw first, bounded, so the text and the commands keep their pace under a flood.
        const bool raw = pump_raw(4u << 20);
        dirty = dirty || raw;
        const auto now = Clock::now();
        if (now - last_text >= opt_.text_interval) {
            last_text = now;
            dirty = pump_text(store::now_unix_ms()) || dirty;
            pump_commands();
        }
        if (dirty && now - last_flush >= opt_.flush_interval) {
            if (auto r = store_->flush(false); !r) log::warn("history", "flush: " + r.error().what);
            last_flush = now;
            dirty = false;
        }
        if (now - last_usage >= opt_.usage_interval) {
            last_usage = now;
            check_usage(store::now_unix_ms());
        }
        if (!raw) {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return stop_; });
        }
    }
}

void HistoryWriter::stop(std::optional<uint64_t> ended_ms) {
    {
        std::lock_guard lk(mu_);
        if (stopped_) return;
        stop_ = true;
        stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (!store_) return;
    while (pump_raw(SIZE_MAX)) {
    }
    const uint64_t now = store::now_unix_ms();
    {
        std::lock_guard lk(term_mu_);
        const uint64_t seq = term_.stream_offset();
        if (ended_ms) (void)tap_->finish(term_, seq, now);
        else (void)tap_->collect(term_, seq, now);
    }
    for (std::size_t i = 0; i < lines_->size(); ++i) {
        const auto& l = lines_->at(i);
        (void)projector_->line(l.text, l.seq, l.ts);
    }
    lines_->clear();
    pump_commands();
    if (open_cmd_ && ended_ms) {
        // The session ended inside a command: keep it without an exit code.
        store::CommandRow row;
        row.session = opt_.session.id;
        row.started_ms = open_cmd_->started;
        row.cwd = open_cmd_->cwd;
        row.command = open_cmd_->line;
        row.seq = open_cmd_->seq;
        (void)catalog_->add_command(row);
        open_cmd_.reset();
    }
    if (ended_ms) {
        if (auto r = store_->close(ended_ms); !r) log::warn("history", "close: " + r.error().what);
    } else if (auto r = store_->flush(true); !r) {
        log::warn("history", "flush: " + r.error().what);
    }
    const auto s = stats();
    log::info("history", "stored " + std::to_string(s.raw_bytes) + " raw bytes, " + std::to_string(s.lines) +
                             " lines, " + std::to_string(s.commands) + " commands; dropped " +
                             std::to_string(s.dropped_records) + " records, " + std::to_string(s.missed_rows) +
                             " rows missed, longest lock " + std::to_string(static_cast<int>(s.max_lock_us)) + " us");
}

HistoryWriterStats HistoryWriter::stats() const {
    std::lock_guard lk(stats_mu_);
    HistoryWriterStats s = stats_;
    s.dropped_records = queue_.dropped_records();
    s.dropped_bytes = queue_.dropped_bytes();
    return s;
}

}  // namespace kt::session
