#include "session/journal.h"

#include <algorithm>
#include <cstring>

#include "base/crc32c.h"

namespace kt::session::journal {
namespace {

void put16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFFu);
    p[1] = static_cast<std::byte>(v >> 8);
}
void put32(std::byte* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void put64(std::byte* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
uint16_t get16(const std::byte* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}
uint32_t get32(const std::byte* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint32_t>(p[i]);
    return v;
}
uint64_t get64(const std::byte* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

constexpr std::size_t align8(std::size_t n) { return (n + 7u) & ~std::size_t{7}; }

uint32_t record_crc(const std::byte* hdr, std::span<const std::byte> payload) {
    uint32_t c = crc32c(std::span(hdr, 24));
    c = crc32c(std::span(hdr + 28, 4), c);
    return crc32c(payload, c);
}

bool known_kind(uint8_t k) { return k <= 3 || k == 0xFF; }

}  // namespace

void encode_header(const Header& h, std::byte out[kHeaderBytes]) {
    std::memset(out, 0, kHeaderBytes);
    put32(out + 0, kMagic);
    put16(out + 4, h.major);
    put16(out + 6, h.minor);
    put64(out + 8, h.generation);
    std::memcpy(out + 16, h.session.chars.data(), SessionId::kLength);
    put32(out + 32, h.ring_size);
    put32(out + 36, h.flags);
    put64(out + 40, h.write_pos);
    put64(out + 48, h.next_seq);
    put64(out + 56, h.checkpoint_seq);
    put64(out + 64, h.last_write_unix_ms);
    put16(out + 72, h.cols);
    put16(out + 74, h.rows);
    put32(out + 76, crc32c(std::span<const std::byte>(out, 76)));
}

bool decode_header(std::span<const std::byte> page, Header& h) {
    if (page.size() < kHeaderBytes) return false;
    const std::byte* p = page.data();
    if (get32(p) != kMagic) return false;
    if (get32(p + 76) != crc32c(page.first(76))) return false;
    h.major = get16(p + 4);
    h.minor = get16(p + 6);
    if (h.major != kMajor) return false;
    h.generation = get64(p + 8);
    std::memcpy(h.session.chars.data(), p + 16, SessionId::kLength);
    h.ring_size = get32(p + 32);
    h.flags = get32(p + 36);
    h.write_pos = get64(p + 40);
    h.next_seq = get64(p + 48);
    h.checkpoint_seq = get64(p + 56);
    h.last_write_unix_ms = get64(p + 64);
    h.cols = get16(p + 72);
    h.rows = get16(p + 74);
    return true;
}

// ------------------------------------------------------------------------------------ Writer

Writer::Writer(std::unique_ptr<JournalStore> store, const SessionId& id, uint32_t ring_size, uint32_t flags)
    : store_(std::move(store)), ring_(ring_size) {
    h_.session = id;
    h_.ring_size = ring_size;
    h_.flags = flags | (store_->mapped() ? 0u : kFlagFallback);
    std::lock_guard lk(mu_);
    write_header_locked();   // copy A (generation 0)
    write_header_locked();   // copy B (generation 1)
}

void Writer::write_header_locked() {
    std::byte buf[kHeaderBytes];
    encode_header(h_, buf);
    store_->write((h_.generation % 2) * kHeaderPage, std::span<const std::byte>(buf, kHeaderBytes));
    ++h_.generation;
}

void Writer::append(Kind kind, uint64_t seq, std::span<const std::byte> payload, uint64_t unix_ms) {
    if (payload.size() > kMaxPayload) payload = payload.first(kMaxPayload);
    std::lock_guard lk(mu_);
    if (!store_) return;
    const std::size_t total = kRecordHeader + align8(payload.size());
    if (h_.write_pos + total > ring_) {
        const std::size_t left = ring_ - h_.write_pos;
        if (left >= kRecordHeader) {
            std::byte pad[kRecordHeader] = {};
            put64(pad, seq);
            put64(pad + 8, unix_ms);
            put32(pad + 16, static_cast<uint32_t>(left - kRecordHeader));
            pad[20] = static_cast<std::byte>(Kind::Pad);
            put32(pad + 28, recno_++);
            put32(pad + 24, record_crc(pad, {}));
            store_->write(kRingOffset + h_.write_pos, std::span<const std::byte>(pad, kRecordHeader));
        }
        h_.write_pos = 0;
    }
    scratch_.assign(total, std::byte{0});
    std::byte* r = scratch_.data();
    put64(r, seq);
    put64(r + 8, unix_ms);
    put32(r + 16, static_cast<uint32_t>(payload.size()));
    r[20] = static_cast<std::byte>(kind);
    put32(r + 28, recno_++);
    if (!payload.empty()) std::memcpy(r + kRecordHeader, payload.data(), payload.size());
    put32(r + 24, record_crc(r, payload));
    store_->write(kRingOffset + h_.write_pos, scratch_);
    h_.write_pos += total;
    if (kind == Kind::Output) {
        h_.next_seq = seq + payload.size();
    } else if (h_.next_seq < seq) {
        h_.next_seq = seq;
    }
    if (kind == Kind::Resize && payload.size() >= 4) {
        h_.cols = get16(payload.data());
        h_.rows = get16(payload.data() + 2);
    }
    h_.last_write_unix_ms = unix_ms;
    write_header_locked();
    if (tap_) tap_->record(kind, seq, unix_ms, payload);
}

void Writer::set_tap(Tap* tap) {
    std::lock_guard lk(mu_);
    tap_ = tap;
}

void Writer::set_size(uint16_t cols, uint16_t rows) {
    std::lock_guard lk(mu_);
    if (!store_) return;
    h_.cols = cols;
    h_.rows = rows;
    write_header_locked();
}

void Writer::set_checkpoint(uint64_t seq) {
    std::lock_guard lk(mu_);
    if (!store_) return;
    h_.checkpoint_seq = seq;
    write_header_locked();
}

Result<void> Writer::flush() {
    std::lock_guard lk(mu_);
    return store_ ? store_->flush() : Result<void>{};
}

Result<void> Writer::pump() {
    std::lock_guard lk(mu_);
    return store_ ? store_->pump() : Result<void>{};
}

void Writer::close() {
    std::lock_guard lk(mu_);
    store_.reset();
    tap_ = nullptr;
}

bool Writer::mapped() const {
    std::lock_guard lk(mu_);
    return store_ && store_->mapped();
}

uint64_t Writer::next_seq() const {
    std::lock_guard lk(mu_);
    return h_.next_seq;
}

// ------------------------------------------------------------------------------------ reader

Result<Contents> read(std::span<const std::byte> file) {
    Contents out;
    if (file.size() < kRingOffset + kRecordHeader) return make_error(Errc::corrupt, "journal: file too small");
    Header a, b;
    const bool va = decode_header(file.subspan(0, kHeaderPage), a);
    const bool vb = decode_header(file.subspan(kHeaderPage, kHeaderPage), b);
    if (va || vb) {
        out.header_valid = true;
        out.header = (va && vb) ? (a.generation >= b.generation ? a : b) : (va ? a : b);
    }
    std::size_t ring = file.size() - kRingOffset;
    if (out.header_valid && out.header.ring_size >= kRecordHeader && out.header.ring_size <= ring) {
        ring = out.header.ring_size;
    }
    const std::span<const std::byte> r = file.subspan(kRingOffset, ring);

    struct Found {
        Record rec;
        bool pad;
    };
    std::vector<Found> found;
    std::size_t off = 0;
    while (off + kRecordHeader <= ring) {
        const std::byte* p = r.data() + off;
        const uint32_t len = get32(p + 16);
        const auto kind = static_cast<uint8_t>(p[20]);
        const bool reserved_zero = p[21] == std::byte{0} && p[22] == std::byte{0} && p[23] == std::byte{0};
        bool ok = false;
        std::size_t total = 0;
        if (known_kind(kind) && reserved_zero) {
            if (kind == 0xFF) {
                total = kRecordHeader + len;
                ok = off + total == ring && get32(p + 24) == record_crc(p, {});
            } else if (len <= kMaxPayload) {
                total = kRecordHeader + align8(len);
                ok = off + total <= ring && get32(p + 24) == record_crc(p, r.subspan(off + kRecordHeader, len));
            }
        }
        if (!ok) {
            off += 8;
            continue;
        }
        Found f;
        f.pad = kind == 0xFF;
        f.rec.kind = static_cast<Kind>(kind);
        f.rec.seq = get64(p);
        f.rec.ts_unix_ms = get64(p + 8);
        f.rec.recno = get32(p + 28);
        if (!f.pad) f.rec.payload = r.subspan(off + kRecordHeader, len);
        found.push_back(f);
        off += total;
    }
    if (found.empty()) {
        if (!out.header_valid) return make_error(Errc::corrupt, "journal: no header and no records");
        return out;
    }
    std::sort(found.begin(), found.end(), [](const Found& x, const Found& y) { return x.rec.recno < y.rec.recno; });
    // The newest contiguous chain: consecutive recnos and a continuous stream.
    std::size_t first = found.size() - 1;
    while (first > 0) {
        const Found& prev = found[first - 1];
        const Found& cur = found[first];
        if (prev.rec.recno + 1 != cur.rec.recno) break;
        const uint64_t prev_end = prev.pad ? prev.rec.seq : prev.rec.stream_end();
        if (prev_end != cur.rec.seq) break;
        --first;
    }
    out.gap = first > 0;
    for (std::size_t i = first; i < found.size(); ++i) {
        if (!found[i].pad) out.records.push_back(found[i].rec);
    }
    return out;
}

}  // namespace kt::session::journal
