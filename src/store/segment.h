// Segment writer and reader. One file per session, kind and series
// number. Files are append-only until sealed, immutable afterwards.
//
// Threading: a SegmentWriter is used by one thread at a time (SessionStore locks). A
// SegmentReader is read-only and may be used by one thread; open one per search thread.
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "base/error.h"
#include "store/file_io.h"
#include "store/format.h"
#include "store/trigram.h"

namespace kt::store {

// What the catalog keeps about a segment (store/catalog.h SegmentRow).
struct SegmentInfo {
    Kind kind = Kind::raw;
    uint32_t series = 0;
    uint32_t blocks = 0;
    uint64_t first_seq = 0, seq_end = 0;
    uint64_t ts_first = 0, ts_last = 0;
    uint64_t raw_bytes = 0, comp_bytes = 0;
    uint64_t file_bytes = 0;
    uint64_t index_bytes = 0;  // footer index part (sealed)
    bool sealed = false;
    int level = kFreshLevel;           // the writer's codec, level and block size target
    Codec codec = kFreshCodec;
    uint32_t block_raw = kBlockRawTarget;
};

// When the trigram index of an indexed kind is built.
enum class IndexMode : uint8_t {
    at_seal,    // the session host: appends do no index work; seal() reads the blocks back once
    on_append,  // compaction: the records pass through anyway, so index them as they come
};

struct WriterOptions {
    uint32_t block_raw = kBlockRawTarget;    // clamped to kMinBlockTarget..kMaxBlockTarget
    uint64_t segment_raw = kSegmentRawTarget;
    Codec codec = kFreshCodec;
    int level = kFreshLevel;                 // clamped to codec_levels(codec)
    bool long_match = false;                 // zstd long-distance matching
    IndexMode index = IndexMode::at_seal;
};

class SegmentWriter {
public:
    static Result<std::unique_ptr<SegmentWriter>> create(const std::filesystem::path& p, const FileHeader& h,
                                                         const WriterOptions& o = {});
    // Opens an unsealed segment for appending: drops a torn tail (truncates the file after the
    // last valid block) and rebuilds the in-memory index. Errc::busy when the file is sealed.
    static Result<std::unique_ptr<SegmentWriter>> resume(const std::filesystem::path& p, const WriterOptions& o = {});

    // Appends one record. `seq` and `ts` are clamped to be non-decreasing inside the segment.
    // Full blocks are compressed and written at once; the open block stays in memory.
    Result<void> append(uint64_t seq, uint64_t ts, std::string_view payload);
    // Writes the open block (a short block); with `durable` also fsyncs.
    Result<void> flush(bool durable);
    // True once the segment holds segment_raw raw bytes: the owner seals and starts the next.
    bool full() const { return info_.raw_bytes + pending_raw_ >= opt_.segment_raw; }
    bool empty() const { return info_.blocks == 0 && pending_records_ == 0; }
    // The seq past the last appended record (raw: past its last byte; other kinds: last seq + 1),
    // the open block included; 0 when empty.
    uint64_t seq_end() const { return pending_records_ ? std::max(info_.seq_end, block_seq_end_) : info_.seq_end; }
    // Flushes, writes the footer and the trailer, fsyncs. The writer is unusable afterwards.
    Result<SegmentInfo> seal();

    const SegmentInfo& info() const { return info_; }
    const std::vector<BlockEntry>& blocks() const { return blocks_; }
    const std::filesystem::path& path() const { return path_; }
    // Candidate blocks of the written blocks (the open block is not searchable yet). With
    // IndexMode::at_seal there is no index before the seal: every block is a candidate.
    trigram::Bits candidates(const trigram::Query& q) const {
        return live_index() ? index_.candidates(q) : trigram::Bits(blocks_.size(), true);
    }
    std::size_t index_memory() const { return index_.bytes(); }
    uint64_t truncated_bytes() const { return truncated_; }

private:
    SegmentWriter() = default;
    Result<void> write_block();
    void index_records(std::string_view raw, uint64_t first_seq, uint64_t ts_first);
    bool live_index() const { return trigrams_ && opt_.index == IndexMode::on_append; }
    // IndexMode::at_seal: reads every written block back and builds index_.
    Result<void> build_index();

    std::filesystem::path path_;
    std::unique_ptr<File> file_;
    WriterOptions opt_;
    FileHeader header_;
    SegmentInfo info_;
    std::vector<BlockEntry> blocks_;
    uint64_t end_ = kFileHeaderBytes;
    uint64_t truncated_ = 0;
    bool sealed_ = false;

    // The open block.
    std::string pending_;
    uint64_t pending_raw_ = 0;
    uint32_t pending_records_ = 0;
    uint64_t block_seq_ = 0, block_ts_ = 0, block_ts_last_ = 0, block_seq_end_ = 0;
    uint64_t last_seq_ = 0, last_ts_ = 0;
    bool any_ = false;

    std::unique_ptr<trigram::BlockSetBuilder> trigrams_;
    trigram::MemoryIndex index_;
    std::string fold_buf_;
};

class SegmentReader {
public:
    static Result<std::unique_ptr<SegmentReader>> open(const std::filesystem::path& p);

    const FileHeader& header() const { return header_; }
    bool sealed() const { return sealed_; }
    const std::vector<BlockEntry>& blocks() const { return blocks_; }
    // Unsealed: offset after the last valid block, and whether bytes follow it (a torn tail
    // or a block being written right now).
    uint64_t valid_end() const { return valid_end_; }
    bool tail_garbage() const { return tail_garbage_; }
    uint64_t file_bytes() const { return file_bytes_; }
    uint64_t index_bytes() const { return index_bytes_; }
    const trigram::SealedIndex* index() const { return sealed_ && indexed(header_.kind) ? &index_ : nullptr; }

    // Reads, verifies (CRC) and decompresses block i. Errc::corrupt on a damaged block.
    Result<void> read_block(std::size_t i, std::string& raw) { return read_block(i, raw, block_buf_); }
    // The same with a caller's buffer: safe to call from several threads at once (positional
    // reads, per-thread decompression contexts).
    Result<void> read_block(std::size_t i, std::string& raw, std::string& scratch) const;
    // Candidate blocks for `q`: from the footer index when sealed, else every block.
    Result<trigram::Bits> candidates(const trigram::Query& q);
    Result<void> for_each_posting(const std::function<void(trigram::Key, const std::vector<uint32_t>&)>& fn);
    SegmentInfo info() const;
    void drop_cache() { file_->drop_cache(); }

private:
    SegmentReader() = default;
    Result<void> load_footer(const Trailer& t, uint64_t size);
    Result<void> scan_blocks(uint64_t size);

    std::unique_ptr<File> file_;
    FileHeader header_;
    bool sealed_ = false;
    std::vector<BlockEntry> blocks_;
    uint64_t valid_end_ = kFileHeaderBytes;
    bool tail_garbage_ = false;
    uint64_t file_bytes_ = 0;
    uint64_t index_offset_ = 0;
    uint64_t index_bytes_ = 0;
    trigram::SealedIndex index_;
    std::string block_buf_;
};

// The file name of a series member: "<kind>-<series:08>.kts"; compaction output adds
// ".<suffix>" before the extension.
std::string segment_file_name(Kind k, uint32_t series, std::string_view suffix = {});
inline constexpr std::string_view kSegmentExt = ".kts";

}  // namespace kt::store
