#include "store/projection.h"

#include <algorithm>
#include <memory>

#include "session/journal.h"
#include "store/fold.h"
#include "vt/terminal.h"

namespace kt::store {

namespace {

constexpr std::size_t kMaxLine = 16u << 10;  // a longer logical line is cut into pieces
constexpr std::size_t kFeedSlice = 4u << 10; // bounded rows per feed, so the tap never misses rows

class NullSink final : public vt::TerminalSink {
public:
    void reply(std::span<const std::byte>) override {}
};

}  // namespace

void append_cells_utf8(std::string& out, std::span<const vt::Cell> cells, const vt::GraphemePool& pool, bool trim) {
    const std::size_t start = out.size();
    if (trim) {
        // Rows are mostly trailing blanks: skip them before converting.
        std::size_t end = cells.size();
        while (end > 0) {
            const vt::Cell& c = cells[end - 1];
            if (!c.cluster() && (c.cp() == 0 || c.cp() == U' ') && c.width() != vt::CellWidth::WideTail) --end;
            else break;
        }
        cells = cells.first(end);
    }
    // Room for every cell as a 4-byte scalar; clusters may need more and grow it below.
    out.resize(start + cells.size() * 4);
    std::size_t n = start;
    auto put = [&](char32_t cp) {
        const auto c = static_cast<uint32_t>(cp);
        if (n + 4 > out.size()) out.resize(out.size() * 2 + 16);
        char* o = out.data() + n;
        if (c < 0x80) {
            o[0] = static_cast<char>(c);
            n += 1;
        } else if (c < 0x800) {
            o[0] = static_cast<char>(0xC0 | (c >> 6));
            o[1] = static_cast<char>(0x80 | (c & 0x3F));
            n += 2;
        } else if (c < 0x10000) {
            o[0] = static_cast<char>(0xE0 | (c >> 12));
            o[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o[2] = static_cast<char>(0x80 | (c & 0x3F));
            n += 3;
        } else {
            o[0] = static_cast<char>(0xF0 | (c >> 18));
            o[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            o[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o[3] = static_cast<char>(0x80 | (c & 0x3F));
            n += 4;
        }
    };
    for (const vt::Cell& c : cells) {
        const char32_t cp = c.cp();
        if (cp >= 0x20 && cp < 0x80 && !c.cluster() && c.width() == vt::CellWidth::Narrow) {
            if (n >= out.size()) out.resize(out.size() * 2 + 16);  // only after long clusters
            out[n++] = static_cast<char>(cp);
            continue;
        }
        const vt::CellWidth w = c.width();
        if (w == vt::CellWidth::WideTail || w == vt::CellWidth::Empty) continue;
        if (c.cluster()) {
            for (const char32_t g : pool[static_cast<uint32_t>(cp)]) put(g);
        } else {
            put(cp == 0 ? U' ' : cp);
        }
    }
    if (trim) {
        while (n > start && out[n - 1] == ' ') --n;
    }
    out.resize(n);
}

std::string cells_to_utf8(std::span<const vt::Cell> cells, const vt::GraphemePool& pool, bool trim) {
    std::string out;
    append_cells_utf8(out, cells, pool, trim);
    return out;
}

Result<void> TextProjector::line(std::string_view utf8, uint64_t seq, uint64_t ts_ms) {
    buf_.assign(utf8);
    for (char& ch : buf_) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
    }
    if (redact_) redact_(buf_);
    if (buf_.find_first_not_of(' ') == std::string::npos) return {};
    ++lines_;
    return store_.append(Kind::text, seq, ts_ms, buf_);
}

void ScrollbackTap::attach(const vt::Terminal& t) {
    next_ = t.scrollback().next_abs_line();
    pending_.clear();
}

Result<void> ScrollbackTap::row(std::span<const vt::Cell> cells, uint16_t flags, const vt::GraphemePool& pool,
                                uint64_t seq, uint64_t ts) {
    const bool wrapped = (flags & vt::kRowWrapped) != 0;
    append_cells_utf8(pending_, cells, pool, !wrapped);
    if (wrapped && pending_.size() < kMaxLine) return {};
    auto r = sink_.line(pending_, seq, ts);
    pending_.clear();
    return r;
}

Result<void> ScrollbackTap::collect(const vt::Terminal& t, uint64_t seq, uint64_t ts_ms, std::size_t max_rows) {
    const vt::Scrollback& sb = t.scrollback();
    const uint64_t first = sb.first_abs_line();
    const uint64_t next = sb.next_abs_line();
    if (next < next_) next_ = next;  // a resize pulled rows back onto the screen
    if (next_ < first) {
        missed_ += first - next_;
        next_ = first;
    }
    const uint64_t end = next - next_ > max_rows ? next_ + max_rows : next;
    for (uint64_t abs = next_; abs < end; ++abs) {
        const auto& l = sb.line(static_cast<std::size_t>(abs - first));
        if (auto r = row(l.cells, l.flags, t.graphemes(), seq, ts_ms); !r) return r;
    }
    next_ = end;
    more_ = end < next;
    return {};
}

Result<void> ScrollbackTap::finish(const vt::Terminal& t, uint64_t seq, uint64_t ts_ms) {
    if (auto r = collect(t, seq, ts_ms); !r) return r;
    const vt::Screen& s = t.screen();
    int last = -1;
    for (uint16_t y = 0; y < s.rows(); ++y) {
        if (!cells_to_utf8(s.row(y), t.graphemes()).empty()) last = y;
    }
    for (int y = 0; y <= last; ++y) {
        const auto yy = static_cast<uint16_t>(y);
        if (auto r = row(s.row(yy), s.flags(yy), t.graphemes(), seq, ts_ms); !r) return r;
    }
    if (!pending_.empty()) {
        auto r = sink_.line(pending_, seq, ts_ms);
        pending_.clear();
        return r;
    }
    return {};
}

Result<BackfillStats> backfill_journal(std::span<const std::byte> journal_file, SessionStore& store,
                                       const BackfillOptions& opt) {
    namespace journal = session::journal;
    auto contents = journal::read(journal_file);
    if (!contents) return Unexpected(contents.error());
    BackfillStats st;
    st.gap = contents->gap;

    vt::GridSize size;
    if (contents->header_valid && contents->header.cols && contents->header.rows) {
        size.cols = contents->header.cols;
        size.rows = contents->header.rows;
    }
    // The size at the start of the ring is unknown when resizes followed; the first resize
    // record fixes it from there on.
    NullSink null_sink;
    vt::TerminalOptions topt;
    topt.scrollback_lines = 10'000;
    auto term = vt::Terminal::create(size, topt, &null_sink);
    if (!term) return Unexpected(term.error());
    TextProjector proj(store);
    ScrollbackTap tap(proj);
    uint64_t last_seq = 0, last_ts = 0;

    for (const auto& r : contents->records) {
        ++st.records;
        last_ts = r.ts_unix_ms;
        if (r.kind == journal::Kind::Output) {
            const auto bytes = r.payload;
            st.output_bytes += bytes.size();
            if (opt.raw) {
                const std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (auto a = store.append(Kind::raw, r.seq, r.ts_unix_ms, sv); !a) return Unexpected(a.error());
            }
            if (!opt.text) continue;
            for (std::size_t off = 0; off < bytes.size(); off += kFeedSlice) {
                const auto n = std::min(kFeedSlice, bytes.size() - off);
                (*term)->feed(bytes.subspan(off, n));
                if (auto c = tap.collect(**term, r.seq + off + n, r.ts_unix_ms); !c) return Unexpected(c.error());
            }
            last_seq = r.stream_end();
        } else if (r.kind == journal::Kind::Resize && r.payload.size() >= 4 && opt.text) {
            const auto* p = reinterpret_cast<const uint8_t*>(r.payload.data());
            vt::GridSize g;
            g.cols = static_cast<uint16_t>(p[0] | (p[1] << 8));
            g.rows = static_cast<uint16_t>(p[2] | (p[3] << 8));
            if (g.cols > 0 && g.rows > 0) (*term)->resize(g);
            if (auto c = tap.collect(**term, r.seq, r.ts_unix_ms); !c) return Unexpected(c.error());
        }
    }
    if (opt.text) {
        (*term)->reset_program_modes();
        if (auto c = tap.finish(**term, last_seq, last_ts); !c) return Unexpected(c.error());
    }
    st.lines = proj.lines();
    st.missed_rows = tap.missed_rows();
    if (auto f = store.flush(true); !f) return Unexpected(f.error());
    return st;
}

}  // namespace kt::store
