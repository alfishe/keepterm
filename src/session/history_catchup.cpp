#include "session/history_catchup.h"

#include <algorithm>
#include <string_view>
#include <thread>

#include "base/fault.h"
#include "base/log.h"
#include "platform/paths.h"
#include "session/history_events.h"
#include "vt/terminal.h"

namespace kt::session {

Result<std::unique_ptr<store::LockFile>> wait_lock(const std::filesystem::path& path, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        auto l = store::LockFile::try_acquire(path);
        if (!l) return Unexpected(l.error());
        if (*l) return std::move(*l);
        if (std::chrono::steady_clock::now() >= deadline) {
            return make_error(Errc::busy, "locked by another process: " + platform::paths::to_utf8(path));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ------------------------------------------------------------------------------- commands

Result<void> CommandLog::load(uint64_t from_seq) {
    auto rows = catalog_.commands(session_);
    if (!rows) return Unexpected(rows.error());
    for (const auto& r : *rows) {
        if (r.seq > from_seq) {
            rows_.insert({r.seq, r.command});
            if (!r.ended_ms) open_rows_.push_back({{r.seq, r.command}, r.id});
        } else if (!r.ended_ms) {
            carried_ = r.id;  // rows come oldest first: the newest wins
        } else {
            carried_.reset();
        }
    }
    return store::for_each_record(store_, store::Kind::commands, from_seq, [&](const store::RecordView& v) {
        if (v.seq > from_seq) records_.insert({v.seq, std::string(v.payload)});
        return true;
    });
}

void CommandLog::started(std::string_view line, std::string_view cwd, uint64_t seq, uint64_t ts_ms) {
    carried_.reset();  // a new command: an older one without a D mark keeps no end
    std::pair<uint64_t, std::string> key{seq, std::string(line)};
    if (!line.empty() && !records_.count(key)) {
        if (auto r = store_.append(store::Kind::commands, seq, ts_ms, line); !r) {
            log::warn("history", "command append: " + r.error().what);
        }
    }
    if (rows_.count(key)) {
        ++skipped_;
        open_ = 0;
        for (const auto& [k, id] : open_rows_) {
            if (k == key) open_ = id;
        }
        return;
    }
    store::CommandRow row;
    row.session = session_;
    row.started_ms = ts_ms;
    row.cwd = std::string(cwd);
    row.command = std::string(line);
    const auto pos = store_.position(store::Kind::text);
    row.segment_id = pos.first;
    row.block = pos.second;
    row.seq = seq;
    auto id = catalog_.add_command(row);
    if (!id) {
        log::warn("history", "command row: " + id.error().what);
        open_.reset();
        return;
    }
    ++added_;
    open_ = *id;
}

void CommandLog::finished(std::optional<int> exit_code, uint64_t ts_ms) {
    std::optional<int64_t> id = open_ ? open_ : carried_;
    open_.reset();
    carried_.reset();
    if (!id || *id == 0) return;
    if (auto r = catalog_.finish_command(*id, ts_ms, exit_code); !r) log::warn("history", "command end: " + r.error().what);
}

// ------------------------------------------------------------------------------- catch-up

HistoryCatchUp::HistoryCatchUp(store::SessionStore& s, store::Catalog& c, CatchUpOptions o)
    : store_(s), commands_(s, c, s.id()), opt_(std::move(o)), tap_(*this) {}

void HistoryCatchUp::fail(const Error& e) {
    if (!err_) err_ = e;
}

void HistoryCatchUp::journal(const journal::Contents& jc) {
    // The time of the screen's lines when nothing follows the checkpoint: the journal's last write.
    if (jc.header_valid) last_ts_ = jc.header.last_write_unix_ms;
    for (const auto& r : jc.records) last_ts_ = std::max(last_ts_, r.ts_unix_ms);
    auto end = store_.end_seq(store::Kind::raw);
    if (!end) return fail(end.error());
    uint64_t cursor = *end;
    for (const auto& r : jc.records) {
        if (r.kind != journal::Kind::Output || r.payload.empty()) continue;
        if (r.stream_end() <= cursor) continue;
        if (r.seq > cursor) {
            // The journal does not reach back to the store's end: a hole nothing can fill. An
            // empty store starts where the journal does.
            if (*end != 0 || stats_.raw_bytes != 0) stats_.raw_gap += r.seq - cursor;
            cursor = r.seq;
        }
        auto bytes = r.payload.subspan(static_cast<std::size_t>(cursor - r.seq));
        const std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (auto a = store_.append(store::Kind::raw, cursor, r.ts_unix_ms, sv); !a) return fail(a.error());
        stats_.raw_bytes += bytes.size();
        cursor = r.stream_end();
        (void)KT_FAULT_POINT("catchup.raw");
    }
    store_events(jc);
}

void HistoryCatchUp::store_events(const journal::Contents& jc) {
    // The resizes the writer had not stored. At the seq of the last stored event
    // the journal may hold it and the ones after it: those after the stored one are new.
    auto end = store_.end_seq(store::Kind::events);
    if (!end) return fail(end.error());
    std::string last;
    if (*end > 0) {
        auto r = store::for_each_record(store_, store::Kind::events, *end - 1, [&](const store::RecordView& v) {
            last.assign(v.payload);
            return true;
        });
        if (!r) return fail(r.error());
    }
    bool synced = *end == 0;
    std::optional<TermSize> cur = parse_resize_event(last);   // a resize to it changes nothing
    // Nothing stored and no resize in the journal: the size the header has was the size all along.
    if (*end == 0 && jc.header_valid && jc.header.cols && jc.header.rows &&
        std::none_of(jc.records.begin(), jc.records.end(), [](const journal::Record& r) { return r.kind == journal::Kind::Resize; })) {
        const uint64_t seq = jc.records.empty() ? 0 : jc.records.front().seq;
        const uint64_t ts = jc.records.empty() ? jc.header.last_write_unix_ms : jc.records.front().ts_unix_ms;
        if (auto a = store_.append(store::Kind::events, seq, ts, resize_event({jc.header.cols, jc.header.rows})); !a) return fail(a.error());
        return;
    }
    for (const auto& r : jc.records) {
        if (r.kind != journal::Kind::Resize || r.payload.size() < 4) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(r.payload.data());
        const TermSize sz{static_cast<uint16_t>(p[0] | (p[1] << 8)), static_cast<uint16_t>(p[2] | (p[3] << 8))};
        if (!sz.cols || !sz.rows || r.seq + 1 < *end) continue;
        const std::string ev = resize_event(sz);
        if (r.seq + 1 == *end && !synced) {
            synced = ev == last;
            continue;
        }
        synced = true;
        if (cur == sz) continue;
        cur = sz;
        if (auto a = store_.append(store::Kind::events, r.seq, r.ts_unix_ms, ev); !a) return fail(a.error());
    }
}

void HistoryCatchUp::start(vt::Terminal& t, uint64_t from_seq, bool) {
    term_ = &t;
    // The stored lines after the replay's start are its first `skip_` lines.
    auto r = store::for_each_record(store_, store::Kind::text, from_seq, [&](const store::RecordView& v) {
        if (v.seq > from_seq) {
            ++skip_;
            last_stored_.assign(v.payload);
        }
        return true;
    });
    if (!r) return fail(r.error());
    if (auto c = commands_.load(from_seq); !c) return fail(c.error());
    tap_.attach(t);
}

void HistoryCatchUp::fed(vt::Terminal& t, uint64_t ts_ms) {
    last_ts_ = ts_ms;
    if (auto r = tap_.collect(t, t.stream_offset(), ts_ms); !r) fail(r.error());
}

Result<void> HistoryCatchUp::line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) {
    buf_.assign(utf8);
    for (char& ch : buf_) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
    }
    if (buf_.find_first_not_of(' ') == std::string::npos) return {};
    if (skip_ > 0) {
        --skip_;
        ++stats_.lines_skipped;
        if (skip_ == 0 && buf_ != last_stored_) stats_.text_misaligned = true;
        return {};
    }
    ++stats_.lines;
    auto r = store_.append(store::Kind::text, seq, ts_ms, buf_);
    (void)KT_FAULT_POINT("catchup.line");
    return r;
}

void HistoryCatchUp::command_started(std::u8string_view line) {
    if (!term_) return;
    const std::string_view sv(reinterpret_cast<const char*>(line.data()), line.size());
    // The end of the journal record, as the live writer stores it (session/history_writer.h).
    commands_.started(sv, opt_.cwd, record_end_, last_ts_);
}

void HistoryCatchUp::record(uint64_t end_seq, uint64_t ts_ms) {
    record_end_ = end_seq;
    last_ts_ = ts_ms;
}

// No row may leave the scrollback before the tap has read it: a byte adds at most one row.
std::size_t HistoryCatchUp::piece() const {
    return std::clamp<std::size_t>(opt_.term.scrollback_lines / 4, 1, 512);
}

void HistoryCatchUp::command_finished(std::optional<int> exit_code) {
    if (!term_) return;
    commands_.finished(exit_code, last_ts_);
}

Result<Recovered> HistoryCatchUp::run(const std::string& dir, vt::TerminalSink* sink) {
    auto rec = recover_session(dir, opt_.term, sink ? sink : this, this);
    if (!rec) return Unexpected(rec.error());
    if (err_) return Unexpected(*err_);
    if (opt_.finish_screen) {
        // An archive that died after it stored the screen: those lines are among the K too.
        vt::Terminal& t = *rec->term;
        if (auto r = tap_.finish(t, t.stream_offset(), last_ts_); !r) return Unexpected(r.error());
    }
    if (skip_ > 0) stats_.text_misaligned = true;  // the store had lines the replay never made
    skip_ = 0;
    stats_.commands = commands_.added();
    term_ = nullptr;
    return rec;
}

}  // namespace kt::session
