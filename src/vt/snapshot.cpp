// KTSN snapshot encoder and decoder. Format summary in vt/snapshot.h.
//
// Deviations from the full snapshot design at this stage:
//   - no LZ4: sections are never compressed (the compressed flag is never set; a reader that
//     sees it refuses the snapshot as not supported);
//   - SCROLLBACK carries plain rows (flags + cells, trailing default blanks trimmed), because the
//     packed page storage is not built yet;
//   - MARKS is not written (prompt marks live only in row flags so far);
//   - PALETTE holds the theme colors set with set_theme, then (trailing, optional) the colors
//     programs set with OSC 4/10/11/12. TITLES ends with the optional XTWINOPS title stack.
#include "vt/snapshot.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "base/crc32c.h"
#include "vt/terminal.h"
#include "vt/width.h"

#ifndef KT_VERSION
#define KT_VERSION "0.0.0"
#endif

namespace kt::vt {
namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kSectionHeaderSize = 12;
constexpr std::size_t kTrailerSize = 16;
constexpr std::size_t kChunkSize = 64u * 1024u;
constexpr uint16_t kTrailerType = 0xFFFF;
constexpr uint16_t kSecCompressed = 1u << 0;
constexpr uint16_t kSecRequired = 1u << 1;

enum SectionType : uint16_t {
    kMeta = 1,
    kParser = 2,
    kModes = 3,
    kCursor = 4,
    kMarginsTabs = 5,
    kPalette = 6,
    kTitles = 7,
    kStyles = 8,
    kLinks = 9,
    kGraphemes = 10,
    kScreenPrimary = 11,
    kScreenAlt = 12,
    kActive = 13,
    kMarks = 14,
    kScrollback = 15,
    kLastKnownSection = 15,
};

constexpr uint32_t kCellBitsMask = Cell::kCpMask | (3u << Cell::kWidthShift) | Cell::kClusterBit | Cell::kProtBit;
constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

// ------------------------------------------------------------------------------ byte I/O

class Writer {
public:
    std::vector<std::byte> b;

    void u8(uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
    void u16(uint16_t v) { le(v, 2); }
    void u32(uint32_t v) { le(v, 4); }
    void u64(uint64_t v) { le(v, 8); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void varint(uint32_t v) {
        while (v >= 0x80u) {
            b.push_back(static_cast<std::byte>((v & 0x7Fu) | 0x80u));
            v >>= 7;
        }
        b.push_back(static_cast<std::byte>(v));
    }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void str(std::u8string_view s) { str(std::string_view(reinterpret_cast<const char*>(s.data()), s.size())); }
    void raw(const void* p, std::size_t n) {
        const auto* q = static_cast<const std::byte*>(p);
        b.insert(b.end(), q, q + n);
    }

private:
    void le(uint64_t v, int n) {
        std::byte tmp[8];
        for (int i = 0; i < n; ++i) tmp[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
        b.insert(b.end(), tmp, tmp + n);
    }
};

// Bounds-checked reader: any read past the end yields zeros and clears `ok`.
class Reader {
public:
    explicit Reader(std::span<const std::byte> d) : d_(d) {}

    bool ok = true;

    bool need(std::size_t n) {
        if (!ok || d_.size() - p_ < n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8() { return need(1) ? static_cast<uint8_t>(d_[p_++]) : uint8_t{0}; }
    uint16_t u16() { return static_cast<uint16_t>(le(2)); }
    uint32_t u32() { return static_cast<uint32_t>(le(4)); }
    uint64_t u64() { return le(8); }
    // LEB128, at most 5 bytes for 32 bits.
    uint32_t varint() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint8_t byte = u8();
            if (!ok) return 0;
            if (shift == 28 && byte > 0x0F) break;
            v |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0) return v;
        }
        ok = false;
        return 0;
    }
    bool boolean() {
        const uint8_t v = u8();
        if (v > 1) ok = false;
        return v == 1;
    }
    std::string str(std::size_t max) {
        const uint32_t n = u32();
        if (n > max) {
            ok = false;
            return {};
        }
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(d_.data() + p_), n);
        p_ += n;
        return s;
    }
    std::size_t remaining() const { return d_.size() - p_; }
    std::size_t pos() const { return p_; }

private:
    uint64_t le(int n) {
        if (!need(static_cast<std::size_t>(n))) return 0;
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(d_[p_ + static_cast<std::size_t>(i)])) << (8 * i);
        p_ += static_cast<std::size_t>(n);
        return v;
    }

    std::span<const std::byte> d_;
    std::size_t p_ = 0;
};

void put_le32(std::byte* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

uint32_t get_le32(const std::byte* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    return v;
}

uint16_t get_le16(const std::byte* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
                                 (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8));
}

// Streams the encoded snapshot to the sink in 64 KiB chunks and keeps the running CRC.
class Emitter {
public:
    explicit Emitter(SnapshotChunkSink& sink) : sink_(sink) {}

    void put(std::span<const std::byte> bytes) {
        crc_ = crc32c(bytes, crc_);
        total_ += bytes.size();
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
        // Keep at least one byte back so that the last chunk is never empty.
        std::size_t off = 0;
        while (buf_.size() - off > kChunkSize) {
            sink_.chunk(std::span(buf_.data() + off, kChunkSize), false);
            off += kChunkSize;
        }
        if (off > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off));
    }

    void section(uint16_t type, uint16_t flags, const Writer& payload) {
        std::byte h[kSectionHeaderSize];
        h[0] = static_cast<std::byte>(type & 0xFFu);
        h[1] = static_cast<std::byte>(type >> 8);
        h[2] = static_cast<std::byte>(flags & 0xFFu);
        h[3] = static_cast<std::byte>(flags >> 8);
        put_le32(h + 4, static_cast<uint32_t>(payload.b.size()));
        put_le32(h + 8, crc32c(payload.b));
        put(h);
        put(payload.b);
        ++sections_;
    }

    void finish() {
        std::byte t[kTrailerSize];
        t[0] = t[1] = std::byte{0xFF};
        t[2] = t[3] = std::byte{0};
        put_le32(t + 4, sections_);
        put_le32(t + 8, static_cast<uint32_t>(total_ + kTrailerSize));
        put_le32(t + 12, crc_);
        buf_.insert(buf_.end(), t, t + kTrailerSize);
        std::size_t off = 0;
        while (buf_.size() - off > kChunkSize) {
            sink_.chunk(std::span(buf_.data() + off, kChunkSize), false);
            off += kChunkSize;
        }
        sink_.chunk(std::span(buf_.data() + off, buf_.size() - off), true);
        buf_.clear();
    }

private:
    SnapshotChunkSink& sink_;
    std::vector<std::byte> buf_;
    uint32_t crc_ = 0;
    uint64_t total_ = 0;
    uint32_t sections_ = 0;
};

class CollectSink final : public SnapshotChunkSink {
public:
    std::vector<std::byte> out;
    void chunk(std::span<const std::byte> bytes, bool) override { out.insert(out.end(), bytes.begin(), bytes.end()); }
};

// ------------------------------------------------------------------------------ envelope

struct Section {
    uint16_t type = 0;
    uint16_t flags = 0;
    std::span<const std::byte> payload;
};

struct Envelope {
    uint16_t major = 0, minor = 0;
    uint32_t flags = 0;
    uint64_t stream_offset = 0;
    std::vector<Section> sections;

    const Section* find(uint16_t type) const {
        for (const Section& s : sections) {
            if (s.type == type) return &s;
        }
        return nullptr;
    }
};

Unexpected<Error> corrupt(const char* what) { return make_error(Errc::corrupt, std::string("snapshot: ") + what); }

Result<Envelope> parse_envelope(std::span<const std::byte> d) {
    if (d.size() < kHeaderSize + kTrailerSize) return corrupt("too short");
    if (std::memcmp(d.data(), "KTSN", 4) != 0) return corrupt("bad magic");
    if (crc32c(d.first(20)) != get_le32(d.data() + 20)) return corrupt("header checksum");
    Envelope env;
    Reader h(d.first(20));
    h.u32();
    env.major = h.u16();
    env.minor = h.u16();
    env.flags = h.u32();
    env.stream_offset = h.u64();
    if (env.major != kSnapshotMajor) return make_error(Errc::not_supported, "snapshot: unknown major version");

    std::size_t pos = kHeaderSize;
    uint32_t seen = 0;  // bit per known section type, to refuse duplicates
    uint32_t total_sections = 0;
    for (;;) {
        if (d.size() - pos < 2) return corrupt("truncated");
        const uint16_t type = get_le16(d.data() + pos);
        if (type == kTrailerType) break;
        if (d.size() - pos < kSectionHeaderSize) return corrupt("truncated section header");
        Section s;
        s.type = type;
        s.flags = get_le16(d.data() + pos + 2);
        const uint32_t len = get_le32(d.data() + pos + 4);
        const uint32_t crc = get_le32(d.data() + pos + 8);
        pos += kSectionHeaderSize;
        if (d.size() - pos < len) return corrupt("truncated section");
        s.payload = d.subspan(pos, len);
        pos += len;
        if (crc32c(s.payload) != crc) return corrupt("section checksum");
        ++total_sections;
        if (s.flags & kSecCompressed) return make_error(Errc::not_supported, "snapshot: compressed section");
        if (type == 0 || type > kLastKnownSection) {
            if (s.flags & kSecRequired) return make_error(Errc::not_supported, "snapshot: unknown required section");
            continue;  // unknown optional section: skipped
        }
        if (seen & (1u << type)) return corrupt("duplicate section");
        seen |= 1u << type;
        env.sections.push_back(s);
    }
    if (d.size() - pos != kTrailerSize) return corrupt("trailer position");
    const std::byte* t = d.data() + pos;
    if (get_le16(t + 2) != 0) return corrupt("trailer");
    if (get_le32(t + 4) != total_sections) return corrupt("section count");
    if (get_le32(t + 8) != d.size()) return corrupt("total length");
    if (crc32c(d.first(pos)) != get_le32(t + 12)) return corrupt("trailer checksum");
    if (!env.find(kMeta)) return corrupt("no META section");
    return env;
}

struct MetaInfo {
    GridSize size;
    uint32_t width_table_version = 0;
    bool ambiguous_is_wide = false;
    uint64_t scrollback_rows = 0;
    uint64_t next_abs_line = 0;
};

Result<MetaInfo> read_meta(const Section& s) {
    Reader r(s.payload);
    MetaInfo m;
    m.size.cols = r.u16();
    m.size.rows = r.u16();
    m.size.cell_w_px = r.u16();
    m.size.cell_h_px = r.u16();
    m.width_table_version = r.u32();
    m.ambiguous_is_wide = r.boolean();
    (void)r.str(64);   // Unicode version
    (void)r.str(256);  // writer build
    m.scrollback_rows = r.u64();
    m.next_abs_line = r.u64();
    if (!r.ok) return corrupt("META");
    if (m.size.cols == 0 || m.size.rows == 0) return corrupt("META size");
    return m;
}

// ------------------------------------------------------------------------- value codecs

void write_style(Writer& w, const Style& s) {
    w.u32(s.fg.v);
    w.u32(s.bg.v);
    w.u32(s.underline_color.v);
    w.u16(s.attrs);
    w.u8(static_cast<uint8_t>(s.underline));
    w.u8(s.spare);
}

bool valid_color(Color c) { return c.tag() <= Color::Rgb; }

Style read_style(Reader& r) {
    Style s;
    s.fg.v = r.u32();
    s.bg.v = r.u32();
    s.underline_color.v = r.u32();
    s.attrs = r.u16();
    const uint8_t ul = r.u8();
    s.spare = r.u8();
    if (ul > static_cast<uint8_t>(Underline::Dashed) || !valid_color(s.fg) || !valid_color(s.bg) ||
        !valid_color(s.underline_color)) {
        r.ok = false;
        return Style{};
    }
    s.underline = static_cast<Underline>(ul);
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------- parser

struct ParserCodec {
    static void save(const Parser& p, Writer& w) {
        w.u8(static_cast<uint8_t>(p.state_));
        w.u8(p.seq_.params.count);
        w.u32(p.seq_.params.starts);
        for (uint16_t v : p.seq_.params.values) w.u16(v);
        w.u8(static_cast<uint8_t>(p.seq_.prefix));
        w.u8(p.seq_.intermediate_count);
        w.u8(static_cast<uint8_t>(p.seq_.intermediates[0]));
        w.u8(static_cast<uint8_t>(p.seq_.intermediates[1]));
        w.u8(static_cast<uint8_t>(p.seq_.final));
        w.boolean(p.seq_overflow_);
        w.boolean(p.param_started_);
        w.u32(p.param_acc_);
        w.boolean(p.after_string_);
        w.boolean(p.osc_overflow_);
        w.str(p.osc_);
        w.u32(static_cast<uint32_t>(p.u_cp_));
        w.u8(p.u_need_);
        w.u8(p.u_lo_);
        w.u8(p.u_hi_);
    }

    static bool load(Parser& p, Reader& r) {
        const uint8_t state = r.u8();
        if (state > static_cast<uint8_t>(State::sos_pm_apc_string)) return false;
        Sequence seq{};
        seq.params.count = r.u8();
        seq.params.starts = r.u32();
        for (uint16_t& v : seq.params.values) v = r.u16();
        seq.prefix = static_cast<char>(r.u8());
        seq.intermediate_count = r.u8();
        seq.intermediates[0] = static_cast<char>(r.u8());
        seq.intermediates[1] = static_cast<char>(r.u8());
        seq.final = static_cast<char>(r.u8());
        if (seq.params.count > Params::kMax + 1 || seq.intermediate_count > seq.intermediates.size()) return false;
        const bool seq_overflow = r.boolean();
        const bool param_started = r.boolean();
        const uint32_t param_acc = r.u32();
        const bool after_string = r.boolean();
        bool osc_overflow = r.boolean();
        std::string osc = r.str(std::numeric_limits<uint32_t>::max());
        const uint32_t u_cp = r.u32();
        const uint8_t u_need = r.u8();
        const uint8_t u_lo = r.u8();
        const uint8_t u_hi = r.u8();
        if (!r.ok || u_need > 3 || param_acc > 0xFFFF || u_cp > 0x10FFFF) return false;
        if (osc.size() > p.opts_.max_osc_bytes) {
            osc.clear();
            osc_overflow = true;
        }
        p.state_ = static_cast<State>(state);
        p.seq_ = seq;
        p.seq_overflow_ = seq_overflow;
        p.param_started_ = param_started;
        p.param_acc_ = param_acc;
        p.after_string_ = after_string;
        p.osc_overflow_ = osc_overflow;
        p.osc_ = std::move(osc);
        p.u_cp_ = static_cast<char32_t>(u_cp);
        p.u_need_ = u_need;
        p.u_lo_ = u_lo;
        p.u_hi_ = u_hi;
        return true;
    }
};

// ----------------------------------------------------------------------------- terminal

struct SnapshotCodec {
    using SavedCursor = Terminal::SavedCursor;
    using Charset = Terminal::Charset;

    // Snapshot table index -> index in the restored terminal's tables.
    struct Tables {
        std::vector<uint16_t> styles{0}, links{0};
        std::vector<uint32_t> graphemes{0};
    };

    // Old table index -> dense index in the snapshot.
    struct Remap {
        std::vector<uint32_t> styles, links, graphemes;
        std::vector<uint16_t> style_list, link_list;
        std::vector<uint32_t> grapheme_list;
        bool text_only = false;

        void mark(const Cell& c) {
            if (!text_only) {
                if (styles[c.style] == kNoIndex) {
                    styles[c.style] = static_cast<uint32_t>(style_list.size() + 1);
                    style_list.push_back(c.style);
                }
                if (links[c.link] == kNoIndex) {
                    links[c.link] = static_cast<uint32_t>(link_list.size() + 1);
                    link_list.push_back(c.link);
                }
            }
            if (c.cluster() && graphemes[c.cp()] == kNoIndex) {
                graphemes[c.cp()] = static_cast<uint32_t>(grapheme_list.size() + 1);
                grapheme_list.push_back(c.cp());
            }
        }
        void mark_link(uint16_t l) {
            if (!text_only && links[l] == kNoIndex) {
                links[l] = static_cast<uint32_t>(link_list.size() + 1);
                link_list.push_back(l);
            }
        }
    };

    static void write_cells(Writer& w, std::span<const Cell> cells, const Remap& m) {
        const std::size_t base = w.b.size();
        w.b.resize(base + cells.size() * 8);
        std::byte* p = w.b.data() + base;
        for (const Cell& c : cells) {
            uint32_t bits = c.bits;
            if (c.cluster()) bits = (bits & ~Cell::kCpMask) | (m.graphemes[c.cp()] & Cell::kCpMask);
            const uint16_t style = m.text_only ? uint16_t{0} : static_cast<uint16_t>(m.styles[c.style]);
            const uint16_t link = m.text_only ? uint16_t{0} : static_cast<uint16_t>(m.links[c.link]);
            put_le32(p, bits);
            p[4] = static_cast<std::byte>(style & 0xFFu);
            p[5] = static_cast<std::byte>(style >> 8);
            p[6] = static_cast<std::byte>(link & 0xFFu);
            p[7] = static_cast<std::byte>(link >> 8);
            p += 8;
        }
    }

    static void write_saved(Writer& w, const SavedCursor& s) {
        w.boolean(s.valid);
        w.u16(s.x);
        w.u16(s.y);
        w.boolean(s.pending_wrap);
        write_style(w, s.style);
        w.boolean(s.prot);
        w.boolean(s.origin);
        for (Charset c : s.g) w.u8(static_cast<uint8_t>(c));
        w.u8(s.gl);
    }

    static void write_screen(Writer& w, const Screen& s, const Remap& m) {
        w.u16(s.cols());
        w.u16(s.rows());
        w.b.reserve(w.b.size() + std::size_t{s.rows()} * (2 + std::size_t{s.cols()} * 8));
        for (uint16_t y = 0; y < s.rows(); ++y) {
            w.u16(s.flags(y));
            write_cells(w, s.row(y), m);
        }
    }

    static Result<void> encode(const Terminal& t, const SnapshotOptions& o, SnapshotChunkSink& sink) {
        const bool sb_only = o.scrollback_only;
        const bool with_screen = !sb_only;
        const bool with_sb = !o.screen_only;
        const Scrollback& sb = t.scrollback_;
        const std::size_t sb_first =
            with_sb && sb.size() > o.scrollback_lines ? sb.size() - o.scrollback_lines : 0;
        const std::size_t sb_rows = with_sb ? sb.size() - sb_first : 0;

        Remap m;
        m.text_only = o.text_only;
        m.styles.assign(t.styles_.size(), kNoIndex);
        m.links.assign(t.links_.size(), kNoIndex);
        m.graphemes.assign(t.graphemes_.size(), kNoIndex);
        m.styles[0] = 0;
        m.links[0] = 0;
        if (with_screen) {
            const auto mark_screen = [&](const Screen& s) {
                for (uint16_t y = 0; y < s.rows(); ++y) {
                    for (const Cell& c : s.row(y)) m.mark(c);
                }
            };
            mark_screen(t.primary_);
            if (t.alt_) mark_screen(*t.alt_);
            m.mark_link(t.link_);
        }
        for (std::size_t i = sb_first; i < sb.size() && with_sb; ++i) {
            for (const Cell& c : sb.line(i).cells) m.mark(c);
        }

        uint32_t flags = 0;
        if (sb_rows > 0 || (with_sb && sb_only)) flags |= kSnapHasScrollback;
        if (o.screen_only) flags |= kSnapScreenOnly;
        if (sb_only) flags |= kSnapScrollbackOnly;
        if (o.text_only) flags |= kSnapTextOnly;

        Emitter e(sink);
        {
            Writer h;
            h.raw("KTSN", 4);
            h.u16(kSnapshotMajor);
            h.u16(kSnapshotMinor);
            h.u32(flags);
            h.u64(t.stream_offset_);
            h.u32(crc32c(h.b));
            e.put(h.b);
        }
        {
            Writer w;
            w.u16(t.size_.cols);
            w.u16(t.size_.rows);
            w.u16(t.size_.cell_w_px);
            w.u16(t.size_.cell_h_px);
            w.u32(width::kTableVersion);
            w.boolean(t.opts_.ambiguous_is_wide);
            w.str(std::string_view(width::unicode_version()));
            w.str(std::string_view("keepterm " KT_VERSION));
            w.u64(sb_rows);
            w.u64(sb.next_abs_line());
            e.section(kMeta, kSecRequired, w);
        }
        if (with_screen) {
            {
                Writer w;
                ParserCodec::save(t.parser_, w);
                e.section(kParser, kSecRequired, w);
            }
            {
                Writer w;
                std::bitset<Terminal::kDecModes.size()> dec = t.dec_;
                dec.reset(static_cast<std::size_t>(Terminal::dec_slot(2026)));  // written as off
                w.u32(static_cast<uint32_t>(dec.to_ulong()));
                w.boolean(t.insert_mode_);
                w.boolean(t.lnm_);
                const InputModes& im = t.modes_;
                w.boolean(im.cursor_keys_app);
                w.boolean(im.keypad_app);
                w.boolean(im.bracketed_paste);
                w.boolean(im.focus_events);
                w.boolean(im.alt_scroll);
                w.boolean(im.win32_input);
                w.boolean(im.backarrow_sends_bs);
                w.u8(im.modify_other_keys);
                w.u8(static_cast<uint8_t>(im.mouse_tracking));
                w.u8(static_cast<uint8_t>(im.mouse_format));
                w.boolean(im.alt_screen);
                w.u8(im.kitty_flags);
                // Trailing, optional: u8 flags for modes outside the 32-slot table (bit 0: 1045),
                // then the XTSAVE list (u8 count, then u16 mode and bool value each).
                if (t.rev_wrap_ext_ || !t.saved_dec_modes_.empty()) {
                    w.u8(t.rev_wrap_ext_ ? 1 : 0);
                    if (!t.saved_dec_modes_.empty()) {
                        w.u8(static_cast<uint8_t>(t.saved_dec_modes_.size()));
                        for (const auto& [mode, on] : t.saved_dec_modes_) {
                            w.u16(mode);
                            w.boolean(on);
                        }
                    }
                }
                e.section(kModes, kSecRequired, w);
            }
            {
                Writer w;
                w.u16(t.x_);
                w.u16(t.y_);
                w.boolean(t.pending_wrap_);
                w.u8(static_cast<uint8_t>(t.shape_));
                write_style(w, o.text_only ? Style{} : t.pen_.style);
                w.boolean(t.pen_.prot);
                w.u16(o.text_only ? uint16_t{0} : static_cast<uint16_t>(m.links[t.link_]));
                for (Charset c : t.g_) w.u8(static_cast<uint8_t>(c));
                w.u8(t.gl_);
                w.u8(t.single_shift_);
                w.u32(static_cast<uint32_t>(t.last_cp_));
                write_saved(w, t.saved_[0]);
                write_saved(w, t.saved_[1]);
                e.section(kCursor, kSecRequired, w);
            }
            {
                Writer w;
                w.u16(t.top_);
                w.u16(t.bottom_);
                w.u16(t.left_);
                w.u16(t.right_);
                w.u16(static_cast<uint16_t>(t.tabs_.size()));
                w.raw(t.tabs_.data(), t.tabs_.size());
                e.section(kMarginsTabs, kSecRequired, w);
            }
            if (!o.text_only) {
                Writer w;
                // The theme, then the program's overrides (a trailing block that older readers
                // do not read; it is absent when there are none).
                write_colors(w, t.theme_);
                if (t.color_overrides_ != ThemeColors{}) write_colors(w, t.color_overrides_);
                e.section(kPalette, kSecRequired, w);
            }
            {
                Writer w;
                w.str(t.title_);
                w.str(t.icon_name_);
                w.str(t.cwd_);
                // XTWINOPS title stack, a trailing block: u8 count, then (title, icon name).
                if (!t.title_stack_.empty()) {
                    w.u8(static_cast<uint8_t>(t.title_stack_.size()));
                    for (const auto& [title, icon] : t.title_stack_) {
                        w.str(title);
                        w.str(icon);
                    }
                }
                e.section(kTitles, 0, w);
            }
        }
        if (!o.text_only) {
            Writer w;
            w.u32(static_cast<uint32_t>(m.style_list.size()));
            for (uint16_t i : m.style_list) write_style(w, t.styles_[i]);
            e.section(kStyles, kSecRequired, w);
            Writer l;
            l.u32(static_cast<uint32_t>(m.link_list.size()));
            for (uint16_t i : m.link_list) {
                l.str(t.links_[i].id);
                l.str(t.links_[i].uri);
            }
            e.section(kLinks, kSecRequired, l);
        }
        {
            Writer w;
            w.u32(static_cast<uint32_t>(m.grapheme_list.size()));
            for (uint32_t i : m.grapheme_list) {
                const std::u32string_view cps = t.graphemes_[i];
                w.u8(static_cast<uint8_t>(cps.size()));
                for (char32_t cp : cps) w.u32(static_cast<uint32_t>(cp));
            }
            e.section(kGraphemes, kSecRequired, w);
        }
        if (with_screen) {
            {
                Writer w;
                write_screen(w, t.primary_, m);
                e.section(kScreenPrimary, kSecRequired, w);
            }
            if (t.alt_) {
                Writer w;
                write_screen(w, *t.alt_, m);
                e.section(kScreenAlt, kSecRequired, w);
            }
            {
                Writer w;
                w.boolean(t.alt_active());
                w.u64(sb.next_abs_line());
                e.section(kActive, kSecRequired, w);
            }
        }
        if (with_sb) {
            // The section header carries the length and CRC, so the scrollback is encoded whole
            // and then streamed in chunks by the emitter.
            e.section(kScrollback, 0, encode_scrollback(sb, sb_first, sb_rows, m));
        }
        e.finish();
        return {};
    }

    // Scrollback rows, packed: u16 flags, varint cell count, varint run count, runs of
    // (varint length, varint style, varint link), then one varint of cell bits per cell (one byte
    // for narrow ASCII).
    static Writer encode_scrollback(const Scrollback& sb, std::size_t first, std::size_t rows, const Remap& m) {
        Writer w;
        w.u32(static_cast<uint32_t>(rows));
        std::vector<uint32_t> runs;
        for (std::size_t i = first; i < sb.size(); ++i) {
            const Scrollback::Line& line = sb.line(i);
            const std::span<const Cell> cells = line.cells;
            w.u16(line.flags);
            w.varint(static_cast<uint32_t>(cells.size()));
            runs.clear();
            for (std::size_t k = 0; k < cells.size();) {
                const uint32_t st = m.text_only ? 0u : m.styles[cells[k].style];
                const uint32_t ln = m.text_only ? 0u : m.links[cells[k].link];
                std::size_t e = k + 1;
                while (e < cells.size() && (m.text_only || (cells[e].style == cells[k].style && cells[e].link == cells[k].link))) ++e;
                runs.push_back(static_cast<uint32_t>(e - k));
                runs.push_back(st);
                runs.push_back(ln);
                k = e;
            }
            w.varint(static_cast<uint32_t>(runs.size() / 3));
            for (uint32_t v : runs) w.varint(v);
            for (const Cell& c : cells) {
                uint32_t bits = c.bits;
                if (c.cluster()) bits = (bits & ~Cell::kCpMask) | (m.graphemes[c.cp()] & Cell::kCpMask);
                w.varint(bits);
            }
        }
        return w;
    }

    static bool decode_cell_bits(uint32_t bits, Cell& c, const Tables& tb) {
        bits &= kCellBitsMask;
        if (bits & Cell::kClusterBit) {
            const uint32_t idx = bits & Cell::kCpMask;
            if (idx == 0 || idx >= tb.graphemes.size()) return false;
            const uint32_t mapped = tb.graphemes[idx];
            bits = mapped != 0 ? ((bits & ~Cell::kCpMask) | mapped)
                               : ((bits & ~(Cell::kCpMask | Cell::kClusterBit)) | 0xFFFDu);
        } else if ((bits & Cell::kCpMask) > 0x10FFFF) {
            return false;
        }
        c.bits = bits;
        return true;
    }

    static bool read_packed_row(Reader& r, std::vector<Cell>& cells, uint16_t& flags, const Tables& tb) {
        flags = r.u16();
        const uint32_t n = r.varint();
        if (!r.ok || n > 0xFFFF || !r.need(n)) return false;  // at least one byte per cell
        cells.resize(n);
        const uint32_t nruns = r.varint();
        if (!r.ok || nruns > n) return false;
        std::size_t at = 0;
        for (uint32_t i = 0; i < nruns; ++i) {
            const uint32_t len = r.varint(), st = r.varint(), ln = r.varint();
            if (!r.ok || len == 0 || len > n - at || st >= tb.styles.size() || ln >= tb.links.size()) return false;
            for (uint32_t k = 0; k < len; ++k) {
                cells[at + k].style = tb.styles[st];
                cells[at + k].link = tb.links[ln];
            }
            at += len;
        }
        if (at != n) return false;
        for (Cell& c : cells) {
            if (!decode_cell_bits(r.varint(), c, tb) || !r.ok) return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------- decode


    static bool read_cells(Reader& r, std::span<Cell> out, const Tables& tb) {
        if (!r.need(out.size() * 8)) return false;
        for (Cell& c : out) {
            const uint32_t bits = r.u32();
            const uint16_t style = r.u16();
            const uint16_t link = r.u16();
            if (style >= tb.styles.size() || link >= tb.links.size()) return false;
            if (!decode_cell_bits(bits, c, tb)) return false;
            c.style = tb.styles[style];
            c.link = tb.links[link];
        }
        return true;
    }

    // Wide pairs from an untrusted snapshot: a lead without its tail, or a tail without its lead,
    // becomes a blank, so the screen invariant (both halves or neither) holds after a restore.
    static void repair_pairs(std::span<Cell> row) {
        const std::size_t n = row.size();
        for (std::size_t x = 0; x < n; ++x) {
            Cell& c = row[x];
            if (c.width() == CellWidth::WideLead) {
                if (x + 1 == n || row[x + 1].width() != CellWidth::WideTail) c = Cell::blank(c.style);
            } else if (c.width() == CellWidth::WideTail) {
                if (x == 0 || row[x - 1].width() != CellWidth::WideLead) c = Cell::blank(c.style);
            }
        }
    }

    // PALETTE block: u8 present (fg, bg, cursor), those u32 values, a 256-bit mask, the masked
    // palette entries.
    static void write_colors(Writer& w, const ThemeColors& th) {
        const uint8_t present = static_cast<uint8_t>((th.fg ? 1u : 0u) | (th.bg ? 2u : 0u) | (th.cursor ? 4u : 0u));
        w.u8(present);
        if (th.fg) w.u32(*th.fg);
        if (th.bg) w.u32(*th.bg);
        if (th.cursor) w.u32(*th.cursor);
        uint8_t mask[32] = {};
        for (std::size_t i = 0; i < 256; ++i) {
            if (th.palette[i]) mask[i / 8] = static_cast<uint8_t>(mask[i / 8] | (1u << (i % 8)));
        }
        w.raw(mask, sizeof mask);
        for (std::size_t i = 0; i < 256; ++i) {
            if (th.palette[i]) w.u32(*th.palette[i]);
        }
    }

    static void read_colors(Reader& r, ThemeColors& th) {
        const uint8_t present = r.u8();
        if (present & 1u) th.fg = r.u32() & 0xFFFFFFu;
        if (present & 2u) th.bg = r.u32() & 0xFFFFFFu;
        if (present & 4u) th.cursor = r.u32() & 0xFFFFFFu;
        uint8_t mask[32];
        for (uint8_t& b : mask) b = r.u8();
        for (std::size_t i = 0; i < 256; ++i) {
            if (mask[i / 8] & (1u << (i % 8))) th.palette[i] = r.u32() & 0xFFFFFFu;
        }
    }

    static bool read_saved(Reader& r, SavedCursor& s) {
        s.valid = r.boolean();
        s.x = r.u16();
        s.y = r.u16();
        s.pending_wrap = r.boolean();
        s.style = read_style(r);
        s.prot = r.boolean();
        s.origin = r.boolean();
        for (Charset& c : s.g) {
            const uint8_t v = r.u8();
            if (v > static_cast<uint8_t>(Charset::Uk)) return false;
            c = static_cast<Charset>(v);
        }
        s.gl = r.u8();
        return r.ok && s.gl <= 3;
    }

    static bool read_screen(Reader& r, Screen& s, const Tables& tb) {
        if (r.u16() != s.cols() || r.u16() != s.rows()) return false;
        for (uint16_t y = 0; y < s.rows(); ++y) {
            const uint16_t f = r.u16();
            if (!read_cells(r, s.row(y), tb)) return false;
            repair_pairs(s.row(y));
            s.set_flags(y, f);
        }
        s.mark_all_dirty();
        return r.ok && r.remaining() == 0;
    }

    static Result<std::unique_ptr<Terminal>> decode(std::span<const std::byte> data, const TerminalOptions& opts,
                                                    TerminalSink* sink, const RestoreOptions& ro) {
        auto env = parse_envelope(data);
        if (!env) return Unexpected(env.error());
        if (env->flags & (kSnapScrollbackOnly | kSnapTextOnly)) {
            return make_error(Errc::not_supported, "snapshot: scrollback-only or text-only cannot be restored");
        }
        auto meta = read_meta(*env->find(kMeta));
        if (!meta) return Unexpected(meta.error());
        for (uint16_t req : {kParser, kModes, kCursor, kMarginsTabs, kStyles, kLinks, kGraphemes, kScreenPrimary, kActive}) {
            if (!env->find(req)) return corrupt("missing section");
        }
        // The screens must have the size META gives before anything is allocated for it: the
        // grid size is untrusted, and a forged one must not reserve gigabytes (fuzzing).
        const std::size_t screen_bytes =
            4u + std::size_t{meta->size.rows} * (2u + std::size_t{meta->size.cols} * 8u);
        if (env->find(kScreenPrimary)->payload.size() != screen_bytes) return corrupt("SCREEN_PRIMARY size");
        if (const Section* s = env->find(kScreenAlt); s && s->payload.size() != screen_bytes) {
            return corrupt("SCREEN_ALT size");
        }
        std::unique_ptr<Terminal> t(new Terminal(meta->size, opts, nullptr));
        Terminal& T = *t;

        // Tables first: the screens and the cursor reference them.
        Tables tb;
        {
            Reader r(env->find(kStyles)->payload);
            const uint32_t n = r.u32();
            if (n > StyleTable::kCapacity || !r.need(std::size_t{n} * 16)) return corrupt("STYLES");
            for (uint32_t i = 0; i < n; ++i) tb.styles.push_back(T.styles_.intern(read_style(r)));
            if (!r.ok) return corrupt("STYLES");
        }
        {
            Reader r(env->find(kLinks)->payload);
            const uint32_t n = r.u32();
            if (n > LinkTable::kMaxLive + 1 || !r.need(std::size_t{n} * 8)) return corrupt("LINKS");
            for (uint32_t i = 0; i < n; ++i) {
                const std::string id = r.str(LinkTable::kMaxUri);
                const std::string uri = r.str(LinkTable::kMaxUri);
                if (!r.ok) return corrupt("LINKS");
                tb.links.push_back(T.links_.intern(id, uri));
            }
        }
        {
            Reader r(env->find(kGraphemes)->payload);
            const uint32_t n = r.u32();
            if (n >= GraphemePool::kMaxEntries || !r.need(std::size_t{n} * 9)) return corrupt("GRAPHEMES");
            std::u32string cps;
            for (uint32_t i = 0; i < n; ++i) {
                const uint8_t k = r.u8();
                if (k < 2 || k > GraphemePool::kMaxCodePoints) return corrupt("GRAPHEMES");
                cps.clear();
                for (uint8_t j = 0; j < k; ++j) {
                    const uint32_t cp = r.u32();
                    if (cp > 0x10FFFF) return corrupt("GRAPHEMES");
                    cps.push_back(static_cast<char32_t>(cp));
                }
                if (!r.ok) return corrupt("GRAPHEMES");
                tb.graphemes.push_back(T.graphemes_.intern(cps));
            }
        }
        // Screens.
        {
            Reader r(env->find(kScreenPrimary)->payload);
            if (!read_screen(r, T.primary_, tb)) return corrupt("SCREEN_PRIMARY");
        }
        if (const Section* s = env->find(kScreenAlt)) {
            T.alt_ = std::make_unique<Screen>(meta->size.cols, meta->size.rows);
            Reader r(s->payload);
            if (!read_screen(r, *T.alt_, tb)) return corrupt("SCREEN_ALT");
        }
        uint64_t next_abs = meta->next_abs_line;
        {
            Reader r(env->find(kActive)->payload);
            const bool alt = r.boolean();
            next_abs = r.u64();
            if (!r.ok || (alt && !T.alt_)) return corrupt("ACTIVE");
            T.active_ = alt ? T.alt_.get() : &T.primary_;
        }
        if (const Section* s = env->find(kScrollback)) {
            Reader r(s->payload);
            const uint32_t n = r.u32();
            if (!r.need(std::size_t{n} * 4)) return corrupt("SCROLLBACK");
            std::vector<Cell> cells;
            for (uint32_t i = 0; i < n; ++i) {
                uint16_t f = 0;
                if (!read_packed_row(r, cells, f, tb)) return corrupt("SCROLLBACK");
                repair_pairs(cells);
                T.scrollback_.push(cells, f);
            }
            if (!r.ok || r.remaining() != 0) return corrupt("SCROLLBACK");
        }
        if (next_abs < T.scrollback_.size()) return corrupt("ACTIVE line number");
        T.scrollback_.set_next_abs_line(next_abs);

        // Modes.
        {
            Reader r(env->find(kModes)->payload);
            const uint32_t dec = r.u32();
            T.dec_ = std::bitset<Terminal::kDecModes.size()>(dec);
            T.insert_mode_ = r.boolean();
            T.lnm_ = r.boolean();
            InputModes im;
            im.cursor_keys_app = r.boolean();
            im.keypad_app = r.boolean();
            im.bracketed_paste = r.boolean();
            im.focus_events = r.boolean();
            im.alt_scroll = r.boolean();
            im.win32_input = r.boolean();
            im.backarrow_sends_bs = r.boolean();
            im.modify_other_keys = r.u8();
            const uint8_t tracking = r.u8();
            const uint8_t format = r.u8();
            im.alt_screen = r.boolean();
            im.kitty_flags = r.u8();
            if (r.ok && r.remaining() > 0) T.rev_wrap_ext_ = (r.u8() & 1u) != 0;
            if (r.ok && r.remaining() > 0) {
                const uint8_t n = r.u8();
                for (uint8_t i = 0; i < n && r.ok; ++i) {
                    const uint16_t mode = r.u16();
                    const bool on = r.boolean();
                    const bool known = mode == 1045 || Terminal::dec_slot(mode) >= 0;
                    const bool dup = std::any_of(T.saved_dec_modes_.begin(), T.saved_dec_modes_.end(),
                                                 [mode](const auto& e) { return e.first == mode; });
                    if (!known || dup) return corrupt("MODES");
                    T.saved_dec_modes_.emplace_back(mode, on);
                }
            }
            if (!r.ok || tracking > static_cast<uint8_t>(MouseTracking::Any) ||
                format > static_cast<uint8_t>(MouseFormat::SgrPixels) || im.modify_other_keys > 2) {
                return corrupt("MODES");
            }
            im.mouse_tracking = static_cast<MouseTracking>(tracking);
            im.mouse_format = static_cast<MouseFormat>(format);
            im.alt_screen = T.alt_active();
            T.modes_ = im;
            T.sync_ = false;
        }
        // Cursor.
        {
            Reader r(env->find(kCursor)->payload);
            const uint16_t x = r.u16(), y = r.u16();
            T.pending_wrap_ = r.boolean();
            const uint8_t shape = r.u8();
            T.pen_.style = read_style(r);
            T.pen_.prot = r.boolean();
            const uint16_t link = r.u16();
            for (Charset& c : T.g_) {
                const uint8_t v = r.u8();
                if (v > static_cast<uint8_t>(Charset::Uk)) return corrupt("CURSOR");
                c = static_cast<Charset>(v);
            }
            T.gl_ = r.u8();
            T.single_shift_ = r.u8();
            const uint32_t last = r.u32();
            if (!read_saved(r, T.saved_[0]) || !read_saved(r, T.saved_[1])) return corrupt("CURSOR");
            if (!r.ok || shape > static_cast<uint8_t>(CursorShape::Bar) || link >= tb.links.size() || T.gl_ > 3 ||
                (T.single_shift_ != 0 && T.single_shift_ != 2 && T.single_shift_ != 3) || last > 0x10FFFF) {
                return corrupt("CURSOR");
            }
            T.x_ = std::min<uint16_t>(x, static_cast<uint16_t>(T.cols() - 1u));
            T.y_ = std::min<uint16_t>(y, static_cast<uint16_t>(T.rows() - 1u));
            for (SavedCursor& sc : T.saved_) {
                // Later code indexes rows with the saved position (resize, DECRC): keep it inside.
                sc.x = std::min<uint16_t>(sc.x, static_cast<uint16_t>(T.cols() - 1u));
                sc.y = std::min<uint16_t>(sc.y, static_cast<uint16_t>(T.rows() - 1u));
            }
            T.shape_ = static_cast<CursorShape>(shape);
            T.shape_default_ = T.shape_ == T.default_shape_;
            T.link_ = tb.links[link];
            T.last_cp_ = static_cast<char32_t>(last);
        }
        // Margins and tabs.
        {
            Reader r(env->find(kMarginsTabs)->payload);
            const uint16_t top = r.u16(), bottom = r.u16(), left = r.u16(), right = r.u16();
            const uint16_t n = r.u16();
            if (!r.ok || !r.need(n)) return corrupt("MARGINS_TABS");
            if (top <= bottom && bottom < T.rows() && left <= right && right < T.cols()) {
                T.top_ = top;
                T.bottom_ = bottom;
                T.left_ = left;
                T.right_ = right;
            } else {
                T.reset_margins();
            }
            std::vector<uint8_t> tabs(n);
            for (uint8_t& v : tabs) v = r.u8() != 0 ? 1 : 0;
            if (n == T.cols()) {
                T.tabs_ = std::move(tabs);
            } else {
                T.reset_tabs();
            }
        }
        if (const Section* s = env->find(kPalette)) {
            Reader r(s->payload);
            ThemeColors th, over;
            read_colors(r, th);
            if (r.ok && r.remaining() > 0) read_colors(r, over);
            if (!r.ok) return corrupt("PALETTE");
            T.theme_ = th;
            T.color_overrides_ = over;
        }
        if (const Section* s = env->find(kTitles)) {
            Reader r(s->payload);
            const auto u8s = [](const std::string& v) { return std::u8string(reinterpret_cast<const char8_t*>(v.data()), v.size()); };
            T.title_ = u8s(r.str(1u << 20));
            T.icon_name_ = u8s(r.str(1u << 20));
            T.cwd_ = u8s(r.str(1u << 20));
            if (r.ok && r.remaining() > 0) {
                const std::size_t n = std::min<std::size_t>(r.u8(), Terminal::kTitleStackMax);
                for (std::size_t i = 0; i < n && r.ok; ++i) {
                    std::u8string title = u8s(r.str(1u << 20));
                    std::u8string icon = u8s(r.str(1u << 20));
                    T.title_stack_.emplace_back(std::move(title), std::move(icon));
                }
            }
            if (!r.ok) return corrupt("TITLES");
        }
        {
            Reader r(env->find(kParser)->payload);
            if (!ParserCodec::load(T.parser_, r)) return corrupt("PARSER");
        }

        T.update_pen();
        T.stream_offset_ = env->stream_offset;
        T.sink_ = sink;
        T.active_->mark_all_dirty();
        T.force_publish_ = true;
        if (ro.primary_only) T.reset_program_modes();
        T.notify_modes();
        return t;
    }
};

Result<void> Terminal::snapshot(const SnapshotOptions& o, SnapshotChunkSink& sink) const {
    return SnapshotCodec::encode(*this, o, sink);
}

std::vector<std::byte> Terminal::snapshot(const SnapshotOptions& o) const {
    CollectSink s;
    (void)SnapshotCodec::encode(*this, o, s);
    return std::move(s.out);
}

Result<std::unique_ptr<Terminal>> Terminal::restore(std::span<const std::byte> data, const TerminalOptions& opts,
                                                    TerminalSink* sink, const RestoreOptions& ro) {
    return SnapshotCodec::decode(data, opts, sink, ro);
}

Result<SnapshotMeta> read_snapshot_meta(std::span<const std::byte> data) {
    auto env = parse_envelope(data);
    if (!env) return Unexpected(env.error());
    auto m = read_meta(*env->find(kMeta));
    if (!m) return Unexpected(m.error());
    SnapshotMeta out;
    out.major = env->major;
    out.minor = env->minor;
    out.flags = env->flags;
    out.stream_offset = env->stream_offset;
    out.size = m->size;
    out.width_table_version = m->width_table_version;
    out.scrollback_rows = m->scrollback_rows;
    return out;
}

}  // namespace kt::vt
