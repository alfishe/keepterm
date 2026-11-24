// History journal `journal.ktj`: a fixed-size ring of output records
// behind two alternating header copies.
//
//   offset 0      header copy A (4 KiB page)
//   offset 4096   header copy B (4 KiB page)
//   offset 8192   ring: records, 8-byte aligned, never wrapping (a pad record fills the end)
//
// Record header (32 bytes): u64 seq, u64 ts (wall clock, ms since the Unix epoch), u32 length,
// u8 kind, 3 reserved, u32 crc32c, u32 recno. The timestamp lets the warm storage tier keep time
// ranges. `recno` counts records from 0, so the reader
// can order records across the wrap and find the newest contiguous chain. The CRC-32C covers
// bytes 0..23 and 28..31 of the record header and the payload (pad records: the header only).
//
// Core code: the writer talks to a JournalStore (the mapping, or the buffered file of the
// portable fallback); both produce byte-identical files. The reader works on a byte span.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/ids.h"

namespace kt::session::journal {

inline constexpr uint32_t kMagic = 0x314A544B;  // "KTJ1"
inline constexpr uint16_t kMajor = 1, kMinor = 0;
inline constexpr std::size_t kHeaderPage = 4096;
inline constexpr std::size_t kRingOffset = 2 * kHeaderPage;
inline constexpr std::size_t kHeaderBytes = 80;
inline constexpr std::size_t kRecordHeader = 32;
inline constexpr uint32_t kMaxPayload = 64u << 10;
inline constexpr uint32_t kMinRing = 256u << 10;
inline constexpr uint32_t kDefaultRing = 8u << 20;

enum class Kind : uint8_t { Output = 0, Resize = 1, Option = 2, Separator = 3, Pad = 0xFF };

enum HeaderFlags : uint32_t { kFlagFallback = 1u << 0, kFlagRestored = 1u << 1 };

struct Header {
    uint16_t major = kMajor, minor = kMinor;
    uint64_t generation = 0;
    SessionId session;
    uint32_t ring_size = 0;
    uint32_t flags = 0;
    uint64_t write_pos = 0;
    uint64_t next_seq = 0;
    uint64_t checkpoint_seq = 0;
    uint64_t last_write_unix_ms = 0;
    uint16_t cols = 0, rows = 0;
};

void encode_header(const Header&, std::byte out[kHeaderBytes]);
// nullopt-like: false when the magic, the CRC or the version is wrong.
bool decode_header(std::span<const std::byte> page, Header& out);

// Where the writer puts bytes. Offsets are file offsets.
class JournalStore {
public:
    virtual ~JournalStore() = default;
    virtual void write(std::size_t offset, std::span<const std::byte> bytes) = 0;
    // Makes everything written so far durable (msync + fsync, or write + fsync).
    virtual Result<void> flush() = 0;
    // Fallback only: writes the buffered bytes without fsync (every 250 ms). Mapping: no-op.
    virtual Result<void> pump() { return {}; }
    virtual bool mapped() const = 0;
};

// A copy of every record the writer appends, for the warm history store (storage
// is fed from the journal). Called on the appending thread under the writer's lock: it must
// not block or allocate per call (session/record_queue.h is the host's implementation).
class Tap {
public:
    virtual ~Tap() = default;
    virtual void record(Kind kind, uint64_t seq, uint64_t unix_ms, std::span<const std::byte> payload) = 0;
};

// Thread-safe: the PTY thread appends, the history writer thread flushes and sets checkpoints.
class Writer {
public:
    Writer(std::unique_ptr<JournalStore> store, const SessionId& id, uint32_t ring_size, uint32_t flags);
    // Every append from now on is also handed to `tap` (nullptr: none). The tap outlives the
    // writer or is reset before it goes.
    void set_tap(Tap* tap);

    // Appends one record (payload <= kMaxPayload; longer output is split by the caller).
    void append(Kind kind, uint64_t seq, std::span<const std::byte> payload, uint64_t unix_ms);
    void set_size(uint16_t cols, uint16_t rows);
    void set_checkpoint(uint64_t seq);
    Result<void> flush();
    Result<void> pump();
    // Releases the store (its file and mapping), so the session folder can be removed while
    // other threads still hold the writer: every call after this does nothing.
    void close();
    bool mapped() const;
    uint64_t next_seq() const;
    uint32_t ring_size() const { return ring_; }

private:
    void write_header_locked();

    mutable std::mutex mu_;
    std::unique_ptr<JournalStore> store_;
    Header h_;
    uint32_t ring_;
    uint32_t recno_ = 0;
    std::vector<std::byte> scratch_;
    Tap* tap_ = nullptr;
};

struct Record {
    Kind kind = Kind::Output;
    uint64_t seq = 0;
    uint64_t ts_unix_ms = 0;
    uint32_t recno = 0;
    std::span<const std::byte> payload;

    uint64_t stream_end() const { return seq + (kind == Kind::Output ? payload.size() : 0); }
};

struct Contents {
    bool header_valid = false;
    Header header;
    std::vector<Record> records;   // the newest contiguous chain, oldest first (no pads)
    bool gap = false;              // older valid records exist before a gap
};

// Reads a whole journal file. The reusable record reader: crash recovery uses it, and a future
// segment writer of the warm tier can read sealed records from a copy of the ring with it. Pure
// function over bytes; never touches the file. Errc::corrupt when the file is not a journal at all (no valid
// header and no valid record); a damaged ring gives the valid chain that is left.
Result<Contents> read(std::span<const std::byte> file);

}  // namespace kt::session::journal
