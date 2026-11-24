#include "store/format.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <zstd.h>

#if KT_STORE_HAVE_LZMA
#include <LzmaDec.h>
#include <LzmaEnc.h>
#endif

#include "base/crc32c.h"

namespace kt::store {

namespace {

void store_u16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFFu);
    p[1] = static_cast<std::byte>(v >> 8);
}
void store_u32(std::byte* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void store_u64(std::byte* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
uint16_t load_u16(const std::byte* p) {
    return static_cast<uint16_t>(std::to_integer<uint16_t>(p[0]) | (std::to_integer<uint16_t>(p[1]) << 8));
}

struct CCtxFree {
    void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DCtxFree {
    void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); }
};

}  // namespace

uint32_t load_u32(const std::byte* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | std::to_integer<uint32_t>(p[i]);
    return v;
}
uint64_t load_u64(const std::byte* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | std::to_integer<uint64_t>(p[i]);
    return v;
}

std::string_view to_string(Kind k) {
    switch (k) {
        case Kind::raw: return "raw";
        case Kind::text: return "text";
        case Kind::commands: return "commands";
        case Kind::events: return "events";
    }
    return "?";
}

std::string_view to_string(Codec c) {
    switch (c) {
        case Codec::none: return "none";
        case Codec::zstd: return "zstd";
        case Codec::lzma: return "lzma";
    }
    return "?";
}

std::optional<Codec> codec_from_string(std::string_view s) {
    for (uint8_t i = 0; i < kCodecCount; ++i) {
        if (to_string(static_cast<Codec>(i)) == s) return static_cast<Codec>(i);
    }
    return std::nullopt;
}

bool codec_available(Codec c) {
    switch (c) {
        case Codec::none:
        case Codec::zstd: return true;
        case Codec::lzma: return KT_STORE_HAVE_LZMA != 0;
    }
    return false;
}

LevelRange codec_levels(Codec c) {
    switch (c) {
        case Codec::none: return {0, 0};
        case Codec::zstd: return {-7, 22};
        case Codec::lzma: return {0, 10};
    }
    return {};
}

std::optional<Kind> kind_from_string(std::string_view s) {
    for (uint8_t i = 0; i < kKindCount; ++i) {
        if (to_string(static_cast<Kind>(i)) == s) return static_cast<Kind>(i);
    }
    return std::nullopt;
}

void encode_file_header(const FileHeader& h, std::byte out[kFileHeaderBytes]) {
    std::memset(out, 0, kFileHeaderBytes);
    store_u32(out, kFileMagic);
    store_u16(out + 4, kMajor);
    store_u16(out + 6, kMinor);
    out[8] = static_cast<std::byte>(h.kind);
    out[9] = static_cast<std::byte>(h.codec);
    store_u16(out + 10, h.flags);
    store_u32(out + 12, h.series);
    for (std::size_t i = 0; i < 16 && i < h.session.size(); ++i) out[16 + i] = static_cast<std::byte>(h.session[i]);
    store_u64(out + 32, h.created_unix_ms);
    out[40] = static_cast<std::byte>(static_cast<uint8_t>(h.level));
    store_u32(out + 44, h.block_raw);
    store_u32(out + 60, crc32c(std::span<const std::byte>(out, 60)));
}

bool decode_file_header(std::span<const std::byte> in, FileHeader& out) {
    if (in.size() < kFileHeaderBytes) return false;
    const std::byte* p = in.data();
    if (load_u32(p) != kFileMagic || load_u16(p + 4) != kMajor) return false;
    if (load_u32(p + 60) != crc32c(in.first(60))) return false;
    const auto kind = std::to_integer<uint8_t>(p[8]);
    if (kind >= kKindCount) return false;
    out.kind = static_cast<Kind>(kind);
    out.codec = std::to_integer<uint8_t>(p[9]);
    out.flags = load_u16(p + 10);
    out.series = load_u32(p + 12);
    out.session.clear();
    for (std::size_t i = 0; i < 16 && p[16 + i] != std::byte{0}; ++i) out.session.push_back(static_cast<char>(p[16 + i]));
    out.created_unix_ms = load_u64(p + 32);
    out.level = static_cast<int8_t>(std::to_integer<uint8_t>(p[40]));
    out.minor = load_u16(p + 6);
    out.block_raw = out.minor >= 1 ? load_u32(p + 44) : kBlockRawTarget;
    if (out.minor == 0) out.codec = kCodecZstd;
    return true;
}

void encode_block_header(const BlockHeader& h, std::span<const std::byte> payload, std::byte out[kBlockHeaderBytes]) {
    std::memset(out, 0, kBlockHeaderBytes);
    store_u32(out, kBlockMagic);
    store_u32(out + 4, h.raw_len);
    store_u32(out + 8, h.comp_len);
    store_u32(out + 12, h.records);
    store_u64(out + 16, h.first_seq);
    store_u64(out + 24, h.ts_first);
    store_u64(out + 32, h.ts_last);
    out[44] = static_cast<std::byte>(static_cast<uint8_t>(h.level));
    out[45] = static_cast<std::byte>(h.codec);
    uint32_t c = crc32c(std::span<const std::byte>(out, 40));
    c = crc32c(std::span<const std::byte>(out + 44, 4), c);
    c = crc32c(payload, c);
    store_u32(out + 40, c);
}

bool decode_block_header(std::span<const std::byte> in, BlockHeader& out) {
    if (in.size() < kBlockHeaderBytes) return false;
    const std::byte* p = in.data();
    if (load_u32(p) != kBlockMagic) return false;
    out.raw_len = load_u32(p + 4);
    out.comp_len = load_u32(p + 8);
    out.records = load_u32(p + 12);
    out.first_seq = load_u64(p + 16);
    out.ts_first = load_u64(p + 24);
    out.ts_last = load_u64(p + 32);
    out.crc = load_u32(p + 40);
    out.level = static_cast<int8_t>(std::to_integer<uint8_t>(p[44]));
    out.codec = std::to_integer<uint8_t>(p[45]);
    if (out.raw_len == 0 || out.raw_len > kMaxBlockRaw) return false;
    if (out.comp_len == 0 || out.comp_len > max_frame_bytes(out.raw_len)) return false;
    return out.ts_last >= out.ts_first && out.records > 0;
}

bool verify_block(std::span<const std::byte> header, std::span<const std::byte> payload) {
    if (header.size() < kBlockHeaderBytes) return false;
    uint32_t c = crc32c(header.first(40));
    c = crc32c(header.subspan(44, 4), c);
    c = crc32c(payload, c);
    return c == load_u32(header.data() + 40);
}

void put_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

bool get_varint(const char*& p, const char* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 70 && p < end; shift += 7) {
        const auto b = static_cast<uint8_t>(*p++);
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void append_record(std::string& raw, uint64_t seq_delta, uint64_t ts_delta, std::string_view payload) {
    put_varint(raw, payload.size());
    put_varint(raw, seq_delta);
    put_varint(raw, ts_delta);
    raw.append(payload);
}

bool decode_records(std::string_view raw, uint64_t first_seq, uint64_t ts_first, std::vector<RecordView>& out) {
    out.clear();
    const char* p = raw.data();
    const char* end = p + raw.size();
    while (p < end) {
        uint64_t len = 0, ds = 0, dt = 0;
        if (!get_varint(p, end, len) || !get_varint(p, end, ds) || !get_varint(p, end, dt)) return false;
        if (len > static_cast<uint64_t>(end - p)) return false;
        out.push_back({first_seq + ds, ts_first + dt, std::string_view(p, static_cast<std::size_t>(len))});
        p += len;
    }
    return true;
}

void encode_trailer(const Trailer& t, std::byte out[kTrailerBytes]) {
    std::memset(out, 0, kTrailerBytes);
    store_u32(out, kTrailerMagic);
    store_u32(out + 4, t.meta_len);
    store_u64(out + 8, t.footer_offset);
    store_u64(out + 16, t.index_offset);
    store_u32(out + 24, t.meta_crc);
    store_u32(out + 28, crc32c(std::span<const std::byte>(out, 28)));
}

bool decode_trailer(std::span<const std::byte> in, Trailer& t) {
    if (in.size() < kTrailerBytes) return false;
    const std::byte* p = in.data();
    if (load_u32(p) != kTrailerMagic || load_u32(p + 28) != crc32c(in.first(28))) return false;
    t.meta_len = load_u32(p + 4);
    t.footer_offset = load_u64(p + 8);
    t.index_offset = load_u64(p + 16);
    t.meta_crc = load_u32(p + 24);
    return t.index_offset >= t.footer_offset && t.index_offset - t.footer_offset == t.meta_len;
}

std::size_t max_frame_bytes(std::size_t raw_len) {
    // zstd's bound, and LZMA's worst case on incompressible input (about raw / 64 + a few
    // bytes) with margin.
    return std::max(ZSTD_compressBound(raw_len), raw_len + raw_len / 32 + 1024);
}

namespace {

Result<std::string> zstd_compress(const CodecSpec& spec, std::string_view raw) {
    thread_local std::unique_ptr<ZSTD_CCtx, CCtxFree> ctx(ZSTD_createCCtx());
    if (!ctx) return make_error(Errc::unavailable, "zstd: no context");
    ZSTD_CCtx* c = ctx.get();
    ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, std::clamp(spec.level, -7, 22));
    ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 0);  // the block CRC covers the frame
    if (spec.long_match) {
        // The window covers the block, capped at 2^24 so every reader decodes it with the
        // default window limit.
        int wlog = 10;
        while (wlog < 24 && (std::size_t{1} << wlog) < raw.size()) ++wlog;
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, wlog);
        ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
    }
    std::string out(ZSTD_compressBound(raw.size()), '\0');
    const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), raw.data(), raw.size());
    if (ZSTD_isError(n)) return make_error(Errc::invalid_argument, std::string("zstd: ") + ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

Result<void> zstd_decompress(std::span<const std::byte> frame, std::size_t raw_len, std::string& out) {
    thread_local std::unique_ptr<ZSTD_DCtx, DCtxFree> ctx(ZSTD_createDCtx());
    if (!ctx) return make_error(Errc::unavailable, "zstd: no context");
    out.resize(raw_len);
    const std::size_t n = ZSTD_decompressDCtx(ctx.get(), out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(n) || n != raw_len) return make_error(Errc::corrupt, "zstd: bad block");
    return {};
}

#if KT_STORE_HAVE_LZMA
// LZMA (the LZMA SDK, raw LZMA1 stream): the frame is the 5 property bytes, then the stream
// without an end marker (the block header has the size).
void* lzma_alloc(ISzAllocPtr, size_t n) { return std::malloc(n); }
void lzma_free(ISzAllocPtr, void* p) { std::free(p); }
const ISzAlloc kLzmaAlloc = {lzma_alloc, lzma_free};

Result<std::string> lzma_compress(const CodecSpec& spec, std::string_view raw) {
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    const int level = std::clamp(spec.level, 0, 10);
    props.level = std::min(level, 9);
    // The dictionary never needs to exceed the block.
    uint32_t dict = 1u << 12;
    while (dict < raw.size() && dict < (1u << 27)) dict <<= 1;
    props.dictSize = dict;
    if (level == 10) {  // xz -9e: the longest match length and a deep match finder
        props.fb = 273;
        props.mc = 1u << 30;
        props.algo = 1;
        props.btMode = 1;
        props.numHashBytes = 4;
    }
    props.numThreads = 1;
    props.writeEndMark = 0;
    props.reduceSize = raw.size();
    std::string out(LZMA_PROPS_SIZE + max_frame_bytes(raw.size()), '\0');
    SizeT dest_len = out.size() - LZMA_PROPS_SIZE;
    SizeT props_len = LZMA_PROPS_SIZE;
    const SRes r = LzmaEncode(reinterpret_cast<Byte*>(out.data() + LZMA_PROPS_SIZE), &dest_len,
                              reinterpret_cast<const Byte*>(raw.data()), raw.size(), &props,
                              reinterpret_cast<Byte*>(out.data()), &props_len, 0, nullptr, &kLzmaAlloc, &kLzmaAlloc);
    if (r != SZ_OK || props_len != LZMA_PROPS_SIZE) return make_error(Errc::invalid_argument, "lzma: encode failed");
    out.resize(LZMA_PROPS_SIZE + dest_len);
    return out;
}

Result<void> lzma_decompress(std::span<const std::byte> frame, std::size_t raw_len, std::string& out) {
    if (frame.size() < LZMA_PROPS_SIZE) return make_error(Errc::corrupt, "lzma: short block");
    out.resize(raw_len);
    SizeT dest_len = raw_len;
    SizeT src_len = frame.size() - LZMA_PROPS_SIZE;
    ELzmaStatus status;
    const auto* src = reinterpret_cast<const Byte*>(frame.data());
    const SRes r = LzmaDecode(reinterpret_cast<Byte*>(out.data()), &dest_len, src + LZMA_PROPS_SIZE, &src_len, src,
                              LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status, &kLzmaAlloc);
    if (r != SZ_OK || dest_len != raw_len) return make_error(Errc::corrupt, "lzma: bad block");
    return {};
}
#endif

}  // namespace

Result<std::string> compress(const CodecSpec& spec, std::string_view raw) {
    switch (spec.codec) {
        case Codec::none: return std::string(raw);
        case Codec::zstd: return zstd_compress(spec, raw);
        case Codec::lzma:
#if KT_STORE_HAVE_LZMA
            return lzma_compress(spec, raw);
#else
            break;
#endif
    }
    return make_error(Errc::not_supported, "codec " + std::string(to_string(spec.codec)) + " is not in this build");
}

Result<void> decompress(uint8_t codec_id, std::span<const std::byte> frame, std::size_t raw_len, std::string& out) {
    switch (codec_id) {
        case static_cast<uint8_t>(Codec::none):
            if (frame.size() != raw_len) return make_error(Errc::corrupt, "stored block: size mismatch");
            out.assign(reinterpret_cast<const char*>(frame.data()), frame.size());
            return {};
        case static_cast<uint8_t>(Codec::zstd): return zstd_decompress(frame, raw_len, out);
        case static_cast<uint8_t>(Codec::lzma):
#if KT_STORE_HAVE_LZMA
            return lzma_decompress(frame, raw_len, out);
#else
            return make_error(Errc::not_supported, "codec lzma is not in this build");
#endif
        default: break;
    }
    return make_error(Errc::not_supported, "unknown codec id " + std::to_string(codec_id));
}

}  // namespace kt::store
