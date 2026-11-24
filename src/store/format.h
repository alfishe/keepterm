// Segment file format of the warm store. Pure code over bytes.
//
//   file header   64 bytes at offset 0
//   blocks        block header (48 bytes) + one compressed frame, one after another; each block
//                 names its own codec (Codec), so codecs can be mixed inside a file
//   footer        sealed files only: block table, trigram index (store/trigram.h)
//   trailer       sealed files only: the last 32 bytes, points at the footer
//
// All integers are little-endian. A file without a valid trailer is unsealed: readers scan the
// blocks from offset 64 and stop at the first one whose magic or CRC is wrong (a torn tail).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::store {

enum class Kind : uint8_t { raw = 0, text = 1, commands = 2, events = 3 };
inline constexpr std::size_t kKindCount = 4;
std::string_view to_string(Kind);
std::optional<Kind> kind_from_string(std::string_view);
// Kinds whose blocks carry a trigram index.
constexpr bool indexed(Kind k) { return k == Kind::text || k == Kind::commands; }

inline constexpr uint32_t kFileMagic = 0x3153544B;     // "KTS1"
inline constexpr uint32_t kBlockMagic = 0x3142544B;    // "KTB1"
inline constexpr uint32_t kFooterMagic = 0x3146544B;   // "KTF1"
inline constexpr uint32_t kTrailerMagic = 0x3154544B;  // "KTT1"
// Minor 1: the block header carries the codec id (byte 45), the file header the block size
// target (bytes 44..47). Minor 0 files are all zstd.
inline constexpr uint16_t kMajor = 1, kMinor = 1;
inline constexpr std::size_t kFileHeaderBytes = 64;
inline constexpr std::size_t kBlockHeaderBytes = 48;
inline constexpr std::size_t kBlockEntryBytes = 52;
inline constexpr std::size_t kTrailerBytes = 32;
inline constexpr uint32_t kBlockRawTarget = 64u << 10;     // default: a block closes at >= 64 KiB raw
inline constexpr uint32_t kMinBlockTarget = 16u << 10;     // storage.block_size range
inline constexpr uint32_t kMaxBlockTarget = 16u << 20;
inline constexpr uint64_t kSegmentRawTarget = 64ull << 20;
inline constexpr uint32_t kMaxRecord = 256u << 10;
// Hard limit of a block's raw bytes: the target plus one record that crossed it.
inline constexpr uint32_t kMaxBlockRaw = kMaxBlockTarget + kMaxRecord + 64;

// Block codecs. The id is stored in every block header; adding a codec is a new value here
// plus its case in compress()/decompress(), with no format change. A reader meets an unknown
// id as Errc::not_supported for that block only.
enum class Codec : uint8_t { none = 0, zstd = 1, lzma = 2 };
inline constexpr std::size_t kCodecCount = 3;
std::string_view to_string(Codec);
std::optional<Codec> codec_from_string(std::string_view);  // config value names: none, zstd, lzma
// Compiled into this build (lzma needs KT_STORE_LZMA=ON).
bool codec_available(Codec);
// Valid levels: zstd -7..22, lzma 0..10 (10 is "9e": level 9 with the extreme match finder
// settings), none 0..0.
struct LevelRange {
    int min = 0, max = 0;
};
LevelRange codec_levels(Codec);

// The fresh tier: what a session host writes (provisional).
inline constexpr Codec kFreshCodec = Codec::zstd;
inline constexpr int kFreshLevel = 1;
inline constexpr int kLiveLevel = kFreshLevel;  // compatibility name
inline constexpr uint8_t kCodecZstd = static_cast<uint8_t>(Codec::zstd);

struct FileHeader {
    Kind kind = Kind::raw;
    uint8_t codec = kCodecZstd;    // the writer's codec (informational: every block names its own)
    uint16_t flags = 0;
    uint32_t series = 0;           // the segment's number in its session and kind
    std::string session;           // 16 chars (session/ids.h)
    uint64_t created_unix_ms = 0;
    int8_t level = kFreshLevel;    // the writer's level (negative: zstd fast levels)
    uint32_t block_raw = kBlockRawTarget;  // the writer's block size target (minor >= 1)
    uint16_t minor = kMinor;       // decoded: the file's minor version
};
void encode_file_header(const FileHeader&, std::byte out[kFileHeaderBytes]);
bool decode_file_header(std::span<const std::byte> in, FileHeader& out);

// On disk: u32 magic, u32 raw_len, u32 comp_len, u32 records, u64 first_seq, u64 ts_first,
// u64 ts_last, u32 crc32c, u8 level, u8 codec, 2 reserved. The CRC covers bytes 0..39 and 44..47
// of the header and the compressed payload. Minor 0 files have 0 in the codec byte and are zstd
// (SegmentReader fixes that up).
struct BlockHeader {
    uint32_t raw_len = 0, comp_len = 0, records = 0;
    uint64_t first_seq = 0, ts_first = 0, ts_last = 0;
    uint32_t crc = 0;
    int8_t level = 0;
    uint8_t codec = kCodecZstd;  // raw id: may name a codec this build does not know
};
void encode_block_header(const BlockHeader&, std::span<const std::byte> payload, std::byte out[kBlockHeaderBytes]);
// Checks the magic and the sizes only; verify_block() checks the CRC once the payload is read.
bool decode_block_header(std::span<const std::byte> in, BlockHeader& out);
bool verify_block(std::span<const std::byte> header, std::span<const std::byte> payload);

// One row of the footer block table.
struct BlockEntry {
    uint64_t offset = 0;  // of the block header
    uint32_t raw_len = 0, comp_len = 0, records = 0;
    uint64_t first_seq = 0, seq_end = 0;  // seq_end: raw kind = past the last byte, else last seq + 1
    uint64_t ts_first = 0, ts_last = 0;
};

// Records inside a block's raw bytes: varint payload_len, varint (seq - first_seq),
// varint (ts - ts_first), payload. Lines of the text kind are records without the newline.
struct RecordView {
    uint64_t seq = 0, ts = 0;
    std::string_view payload;
};
// Decodes the records of one decompressed block; false on malformed bytes.
bool decode_records(std::string_view raw, uint64_t first_seq, uint64_t ts_first, std::vector<RecordView>& out);
void append_record(std::string& raw, uint64_t seq_delta, uint64_t ts_delta, std::string_view payload);

struct Trailer {
    uint64_t footer_offset = 0;
    uint64_t index_offset = 0;   // absolute offset of the posting chunks
    uint32_t meta_len = 0;       // bytes from footer_offset up to index_offset
    uint32_t meta_crc = 0;       // CRC-32C of those bytes
};
void encode_trailer(const Trailer&, std::byte out[kTrailerBytes]);
bool decode_trailer(std::span<const std::byte> in, Trailer& out);

// Varints (LEB128, unsigned).
void put_varint(std::string& out, uint64_t v);
// Advances `p`; false on truncation or more than 10 bytes.
bool get_varint(const char*& p, const char* end, uint64_t& v);

void put_u32(std::string& out, uint32_t v);
void put_u64(std::string& out, uint64_t v);
uint32_t load_u32(const std::byte* p);
uint64_t load_u64(const std::byte* p);

// What a writer compresses with. long_match enables zstd long-distance matching (no effect on
// other codecs; the reader needs nothing extra).
struct CodecSpec {
    Codec codec = kFreshCodec;
    int level = kFreshLevel;
    bool long_match = false;
};
// The largest frame a block of raw_len bytes can have (header sanity checks).
std::size_t max_frame_bytes(std::size_t raw_len);
// compress() returns the frame of one block; decompress() checks the size. Per-thread codec
// contexts. An unknown or missing codec gives Errc::not_supported.
Result<std::string> compress(const CodecSpec& spec, std::string_view raw);
Result<void> decompress(uint8_t codec_id, std::span<const std::byte> frame, std::size_t raw_len, std::string& out);
// zstd shorthands (tests, old callers).
inline Result<std::string> compress(std::string_view raw, int level) {
    return compress(CodecSpec{Codec::zstd, level, false}, raw);
}
inline Result<void> decompress(std::span<const std::byte> frame, std::size_t raw_len, std::string& out) {
    return decompress(kCodecZstd, frame, raw_len, out);
}

}  // namespace kt::store
