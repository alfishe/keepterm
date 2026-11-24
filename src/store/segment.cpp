#include "store/segment.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "base/crc32c.h"
#include "store/fold.h"

namespace kt::store {

std::string segment_file_name(Kind k, uint32_t series, std::string_view suffix) {
    char num[16];
    std::snprintf(num, sizeof num, "%08u", static_cast<unsigned>(series));
    std::string s = std::string(to_string(k)) + "-" + num;
    if (!suffix.empty()) {
        s += ".";
        s += suffix;
    }
    s += kSegmentExt;
    return s;
}

namespace {

std::span<const std::byte> bytes_of(std::string_view s) { return std::as_bytes(std::span(s.data(), s.size())); }

void encode_entry(std::string& out, const BlockEntry& e) {
    put_u64(out, e.offset);
    put_u32(out, e.raw_len);
    put_u32(out, e.comp_len);
    put_u32(out, e.records);
    put_u64(out, e.first_seq);
    put_u64(out, e.seq_end);
    put_u64(out, e.ts_first);
    put_u64(out, e.ts_last);
}

BlockEntry decode_entry(const std::byte* p) {
    BlockEntry e;
    e.offset = load_u64(p);
    e.raw_len = load_u32(p + 8);
    e.comp_len = load_u32(p + 12);
    e.records = load_u32(p + 16);
    e.first_seq = load_u64(p + 20);
    e.seq_end = load_u64(p + 28);
    e.ts_first = load_u64(p + 36);
    e.ts_last = load_u64(p + 44);
    return e;
}

uint64_t record_end(Kind k, uint64_t seq, std::size_t len) { return k == Kind::raw ? seq + len : seq + 1; }

}  // namespace

// ---------------------------------------------------------------- writer

Result<std::unique_ptr<SegmentWriter>> SegmentWriter::create(const std::filesystem::path& p, const FileHeader& h,
                                                             const WriterOptions& o) {
    if (!codec_available(o.codec)) {
        return make_error(Errc::not_supported, "codec " + std::string(to_string(o.codec)) + " is not in this build");
    }
    auto f = File::open(p, File::Mode::create_new);
    if (!f) return Unexpected(f.error());
    std::unique_ptr<SegmentWriter> w(new SegmentWriter());
    w->path_ = p;
    w->file_ = std::move(*f);
    w->opt_ = o;
    w->opt_.block_raw = std::clamp(o.block_raw, kMinBlockTarget, kMaxBlockTarget);
    const LevelRange lr = codec_levels(o.codec);
    w->opt_.level = std::clamp(o.level, lr.min, lr.max);
    w->header_ = h;
    w->header_.codec = static_cast<uint8_t>(o.codec);
    w->header_.level = static_cast<int8_t>(w->opt_.level);
    w->header_.block_raw = w->opt_.block_raw;
    w->info_.kind = h.kind;
    w->info_.series = h.series;
    w->info_.level = w->opt_.level;
    w->info_.codec = o.codec;
    w->info_.block_raw = w->opt_.block_raw;
    std::byte hb[kFileHeaderBytes];
    encode_file_header(w->header_, hb);
    if (auto r = w->file_->write_at(0, hb); !r) return Unexpected(r.error());
    sync_dir(p.parent_path());  // the new name survives a crash once its first blocks do
    w->info_.file_bytes = kFileHeaderBytes;
    if (indexed(h.kind)) w->trigrams_ = std::make_unique<trigram::BlockSetBuilder>();
    return w;
}

Result<std::unique_ptr<SegmentWriter>> SegmentWriter::resume(const std::filesystem::path& p, const WriterOptions& o) {
    auto rd = SegmentReader::open(p);
    if (!rd) return Unexpected(rd.error());
    if ((*rd)->sealed()) return make_error(Errc::busy, "segment is sealed");
    auto f = File::open(p, File::Mode::write_existing);
    if (!f) return Unexpected(f.error());
    std::unique_ptr<SegmentWriter> w(new SegmentWriter());
    w->path_ = p;
    w->file_ = std::move(*f);
    w->opt_ = o;
    w->opt_.block_raw = std::clamp(o.block_raw, kMinBlockTarget, kMaxBlockTarget);
    const LevelRange lr = codec_levels(o.codec);
    w->opt_.level = std::clamp(o.level, lr.min, lr.max);
    if (!codec_available(o.codec)) w->opt_.codec = Codec::zstd;
    // New blocks follow the current options; every block names its codec, so a file may mix.
    w->header_ = (*rd)->header();
    w->info_ = (*rd)->info();
    w->info_.codec = w->opt_.codec;
    w->info_.level = w->opt_.level;
    w->info_.block_raw = w->opt_.block_raw;
    w->blocks_ = (*rd)->blocks();
    w->end_ = (*rd)->valid_end();
    if ((*rd)->file_bytes() > w->end_) {
        w->truncated_ = (*rd)->file_bytes() - w->end_;
        if (auto r = w->file_->truncate(w->end_); !r) return Unexpected(r.error());
        if (auto r = w->file_->sync(); !r) return Unexpected(r.error());
    }
    w->info_.file_bytes = w->end_;
    if (!w->blocks_.empty()) {
        w->last_seq_ = w->blocks_.back().seq_end;
        w->last_ts_ = w->blocks_.back().ts_last;
        w->any_ = true;
    }
    if (indexed(w->header_.kind)) w->trigrams_ = std::make_unique<trigram::BlockSetBuilder>();
    if (w->live_index()) {
        std::string raw;
        for (std::size_t i = 0; i < w->blocks_.size(); ++i) {
            if (auto r = (*rd)->read_block(i, raw); !r) return Unexpected(r.error());
            w->index_records(raw, w->blocks_[i].first_seq, w->blocks_[i].ts_first);
            w->index_.add_block(w->trigrams_->take_sorted());
        }
    }
    return w;
}

void SegmentWriter::index_records(std::string_view raw, uint64_t first_seq, uint64_t ts_first) {
    std::vector<RecordView> recs;
    if (!decode_records(raw, first_seq, ts_first, recs)) return;
    for (const auto& r : recs) {
        fold_buf_.clear();
        fold_utf8(r.payload, fold_buf_);
        trigrams_->add(fold_buf_);
    }
}

Result<void> SegmentWriter::append(uint64_t seq, uint64_t ts, std::string_view payload) {
    if (sealed_) return make_error(Errc::invalid_argument, "segment is sealed");
    if (payload.size() > kMaxRecord) payload = payload.substr(0, kMaxRecord);
    if (any_) {
        seq = std::max(seq, last_seq_);
        ts = std::max(ts, last_ts_);
    }
    any_ = true;
    if (pending_records_ == 0) {
        block_seq_ = seq;
        block_ts_ = ts;
        block_seq_end_ = seq;
    }
    append_record(pending_, seq - block_seq_, ts - block_ts_, payload);
    if (live_index()) {
        fold_buf_.clear();
        fold_utf8(payload, fold_buf_);
        trigrams_->add(fold_buf_);
    }
    ++pending_records_;
    pending_raw_ = pending_.size();
    block_ts_last_ = ts;
    block_seq_end_ = std::max(block_seq_end_, record_end(header_.kind, seq, payload.size()));
    last_seq_ = seq;
    last_ts_ = ts;
    if (pending_.size() >= opt_.block_raw) return write_block();
    return {};
}

Result<void> SegmentWriter::write_block() {
    if (pending_records_ == 0) return {};
    auto comp = compress(CodecSpec{opt_.codec, opt_.level, opt_.long_match}, pending_);
    if (!comp) return Unexpected(comp.error());
    BlockHeader h;
    h.codec = static_cast<uint8_t>(opt_.codec);
    if (comp->size() >= pending_.size()) {  // incompressible: store it
        *comp = pending_;
        h.codec = static_cast<uint8_t>(Codec::none);
    }
    h.raw_len = static_cast<uint32_t>(pending_.size());
    h.comp_len = static_cast<uint32_t>(comp->size());
    h.records = pending_records_;
    h.first_seq = block_seq_;
    h.ts_first = block_ts_;
    h.ts_last = block_ts_last_;
    h.level = static_cast<int8_t>(opt_.level);
    std::string buf(kBlockHeaderBytes, '\0');
    encode_block_header(h, bytes_of(*comp), reinterpret_cast<std::byte*>(buf.data()));
    buf += *comp;
    if (auto r = file_->write_at(end_, bytes_of(buf)); !r) return r;

    BlockEntry e;
    e.offset = end_;
    e.raw_len = h.raw_len;
    e.comp_len = h.comp_len;
    e.records = h.records;
    e.first_seq = h.first_seq;
    e.seq_end = block_seq_end_;
    e.ts_first = h.ts_first;
    e.ts_last = h.ts_last;
    blocks_.push_back(e);
    end_ += buf.size();

    if (info_.blocks == 0) {
        info_.first_seq = e.first_seq;
        info_.ts_first = e.ts_first;
    }
    ++info_.blocks;
    info_.seq_end = std::max(info_.seq_end, e.seq_end);
    info_.ts_last = e.ts_last;
    info_.raw_bytes += h.raw_len;
    info_.comp_bytes += h.comp_len;
    info_.file_bytes = end_;
    if (live_index()) index_.add_block(trigrams_->take_sorted());

    pending_.clear();
    pending_raw_ = 0;
    pending_records_ = 0;
    return {};
}

Result<void> SegmentWriter::flush(bool durable) {
    if (sealed_) return {};
    if (auto r = write_block(); !r) return r;
    if (durable) return file_->sync();
    return {};
}

Result<SegmentInfo> SegmentWriter::seal() {
    if (sealed_) return info_;
    if (auto r = write_block(); !r) return Unexpected(r.error());
    if (trigrams_ && !live_index()) {
        if (auto r = build_index(); !r) return Unexpected(r.error());
    }
    std::string meta, postings;
    put_u32(meta, kFooterMagic);
    put_u32(meta, static_cast<uint32_t>(blocks_.size()));
    for (const auto& e : blocks_) encode_entry(meta, e);
    if (trigrams_) {
        index_.serialize(meta, postings);
    } else {
        put_u32(meta, 0);
        put_u32(meta, 0);
    }
    Trailer t;
    t.footer_offset = end_;
    t.meta_len = static_cast<uint32_t>(meta.size());
    t.index_offset = end_ + meta.size();
    t.meta_crc = crc32c(bytes_of(meta));
    std::string all = meta + postings;
    all.resize(all.size() + kTrailerBytes);
    encode_trailer(t, reinterpret_cast<std::byte*>(all.data() + all.size() - kTrailerBytes));
    if (auto r = file_->write_at(end_, bytes_of(all)); !r) return Unexpected(r.error());
    if (auto r = file_->sync(); !r) return Unexpected(r.error());
    end_ += all.size();
    info_.file_bytes = end_;
    info_.index_bytes = trigrams_ ? (meta.size() - 8 - blocks_.size() * kBlockEntryBytes) + postings.size() : 0;
    info_.sealed = true;
    sealed_ = true;
    index_.clear();
    file_.reset();
    return info_;
}

Result<void> SegmentWriter::build_index() {
    index_.clear();
    std::string frame, raw;
    for (const auto& e : blocks_) {
        frame.resize(kBlockHeaderBytes + e.comp_len);
        auto all = std::as_writable_bytes(std::span(frame.data(), frame.size()));
        if (auto r = file_->read_exact(e.offset, all); !r) return r;
        BlockHeader h;
        if (!decode_block_header(all.first(kBlockHeaderBytes), h)) return make_error(Errc::corrupt, "block header");
        if (auto r = decompress(h.codec, all.subspan(kBlockHeaderBytes), e.raw_len, raw); !r) return r;
        index_records(raw, e.first_seq, e.ts_first);
        index_.add_block(trigrams_->take_sorted());
    }
    return {};
}

// ---------------------------------------------------------------- reader

Result<std::unique_ptr<SegmentReader>> SegmentReader::open(const std::filesystem::path& p) {
    auto f = File::open(p, File::Mode::read);
    if (!f) return Unexpected(f.error());
    std::unique_ptr<SegmentReader> r(new SegmentReader());
    r->file_ = std::move(*f);
    auto size = r->file_->size();
    if (!size) return Unexpected(size.error());
    r->file_bytes_ = *size;
    std::byte hb[kFileHeaderBytes];
    if (*size < kFileHeaderBytes || !r->file_->read_exact(0, hb) || !decode_file_header(hb, r->header_)) {
        return make_error(Errc::corrupt, "not a segment file");
    }
    if (*size >= kFileHeaderBytes + kTrailerBytes) {
        std::byte tb[kTrailerBytes];
        Trailer t;
        if (r->file_->read_exact(*size - kTrailerBytes, tb) && decode_trailer(tb, t) &&
            t.index_offset <= *size - kTrailerBytes && t.footer_offset >= kFileHeaderBytes) {
            if (r->load_footer(t, *size)) return r;
            // A bad footer: fall back to the block scan; the file is treated as unsealed.
            r->blocks_.clear();
        }
    }
    if (auto s = r->scan_blocks(*size); !s) return Unexpected(s.error());
    return r;
}

Result<void> SegmentReader::load_footer(const Trailer& t, uint64_t size) {
    std::string meta(t.meta_len, '\0');
    if (auto r = file_->read_exact(t.footer_offset, std::as_writable_bytes(std::span(meta.data(), meta.size()))); !r) {
        return r;
    }
    if (crc32c(bytes_of(meta)) != t.meta_crc || meta.size() < 16) return make_error(Errc::corrupt, "footer checksum");
    const auto* b = reinterpret_cast<const std::byte*>(meta.data());
    if (load_u32(b) != kFooterMagic) return make_error(Errc::corrupt, "footer magic");
    const uint32_t n = load_u32(b + 4);
    if (meta.size() < 8 + std::size_t{n} * kBlockEntryBytes + 8) return make_error(Errc::corrupt, "footer size");
    blocks_.clear();
    blocks_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        blocks_.push_back(decode_entry(b + 8 + std::size_t{i} * kBlockEntryBytes));
        const auto& e = blocks_.back();
        if (e.offset + kBlockHeaderBytes + e.comp_len > t.footer_offset) return make_error(Errc::corrupt, "block table");
    }
    const char* p = meta.data() + 8 + std::size_t{n} * kBlockEntryBytes;
    const char* idx_meta = p;
    if (!index_.parse(p, meta.data() + meta.size(), n)) return make_error(Errc::corrupt, "index table");
    index_offset_ = t.index_offset;
    index_bytes_ = static_cast<uint64_t>(p - idx_meta) + (size - kTrailerBytes - t.index_offset);
    sealed_ = true;
    valid_end_ = t.footer_offset;
    return {};
}

Result<void> SegmentReader::scan_blocks(uint64_t size) {
    uint64_t pos = kFileHeaderBytes;
    std::string payload;
    blocks_.clear();
    std::vector<RecordView> recs;
    while (pos + kBlockHeaderBytes <= size) {
        std::byte hb[kBlockHeaderBytes];
        BlockHeader h;
        if (!file_->read_exact(pos, hb) || !decode_block_header(hb, h)) break;
        if (pos + kBlockHeaderBytes + h.comp_len > size) break;
        payload.resize(h.comp_len);
        auto pb = std::as_writable_bytes(std::span(payload.data(), payload.size()));
        if (!file_->read_exact(pos + kBlockHeaderBytes, pb) || !verify_block(hb, pb)) break;
        BlockEntry e;
        e.offset = pos;
        e.raw_len = h.raw_len;
        e.comp_len = h.comp_len;
        e.records = h.records;
        e.first_seq = h.first_seq;
        e.ts_first = h.ts_first;
        e.ts_last = h.ts_last;
        // seq_end needs the records; decompress (unsealed segments are short-lived).
        std::string raw;
        const uint8_t codec = header_.minor == 0 ? kCodecZstd : h.codec;
        auto d = decompress(codec, pb, h.raw_len, raw);
        if (!d && d.error().code == Errc::not_supported) {
            // A codec this build does not have: the CRC holds, so the block is kept (and never
            // cut as a torn tail); its end is unknown here.
            e.seq_end = h.first_seq;
        } else {
            if (!d || !decode_records(raw, h.first_seq, h.ts_first, recs) || recs.empty()) break;
            e.seq_end = 0;
            for (const auto& r : recs) e.seq_end = std::max(e.seq_end, record_end(header_.kind, r.seq, r.payload.size()));
        }
        blocks_.push_back(e);
        pos += kBlockHeaderBytes + h.comp_len;
    }
    valid_end_ = pos;
    tail_garbage_ = pos < size;
    sealed_ = false;
    return {};
}

Result<void> SegmentReader::read_block(std::size_t i, std::string& raw, std::string& scratch) const {
    if (i >= blocks_.size()) return make_error(Errc::invalid_argument, "block index");
    const auto& e = blocks_[i];
    scratch.resize(kBlockHeaderBytes + e.comp_len);
    auto all = std::as_writable_bytes(std::span(scratch.data(), scratch.size()));
    if (auto r = file_->read_exact(e.offset, all); !r) return r;
    BlockHeader h;
    if (!decode_block_header(all.first(kBlockHeaderBytes), h) || h.comp_len != e.comp_len || h.raw_len != e.raw_len ||
        !verify_block(all.first(kBlockHeaderBytes), all.subspan(kBlockHeaderBytes))) {
        return make_error(Errc::corrupt, "block checksum mismatch");
    }
    const uint8_t codec = header_.minor == 0 ? kCodecZstd : h.codec;
    auto r = decompress(codec, all.subspan(kBlockHeaderBytes), e.raw_len, raw);
    if (!r && r.error().code == Errc::not_supported) {
        return make_error(Errc::not_supported, "block " + std::to_string(i) + ": " + r.error().what);
    }
    return r;
}

Result<trigram::Bits> SegmentReader::candidates(const trigram::Query& q) {
    if (!sealed_ || !indexed(header_.kind)) return trigram::Bits(blocks_.size(), true);
    return index_.candidates(q, [this](uint64_t off, uint32_t len, std::string& out) -> Result<void> {
        out.resize(len);
        return file_->read_exact(index_offset_ + off, std::as_writable_bytes(std::span(out.data(), out.size())));
    });
}

Result<void> SegmentReader::for_each_posting(
    const std::function<void(trigram::Key, const std::vector<uint32_t>&)>& fn) {
    if (!sealed_ || !indexed(header_.kind)) return {};
    return index_.for_each(
        [this](uint64_t off, uint32_t len, std::string& out) -> Result<void> {
            out.resize(len);
            return file_->read_exact(index_offset_ + off, std::as_writable_bytes(std::span(out.data(), out.size())));
        },
        fn);
}

SegmentInfo SegmentReader::info() const {
    SegmentInfo s;
    s.kind = header_.kind;
    s.series = header_.series;
    s.blocks = static_cast<uint32_t>(blocks_.size());
    s.sealed = sealed_;
    s.file_bytes = sealed_ ? file_bytes_ : valid_end_;
    s.index_bytes = index_bytes_;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const auto& e = blocks_[i];
        if (i == 0) {
            s.first_seq = e.first_seq;
            s.ts_first = e.ts_first;
        }
        s.seq_end = std::max(s.seq_end, e.seq_end);
        s.ts_last = std::max(s.ts_last, e.ts_last);
        s.raw_bytes += e.raw_len;
        s.comp_bytes += e.comp_len;
    }
    s.level = header_.level;
    s.codec = header_.codec < kCodecCount ? static_cast<Codec>(header_.codec) : Codec::zstd;
    s.block_raw = header_.block_raw;
    return s;
}

}  // namespace kt::store
