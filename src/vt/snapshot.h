// KTSN snapshot format: options, versions and the meta reader.
//
// Terminal::snapshot() writes it, Terminal::restore() reads it (vt/terminal.h). The encoder and
// decoder live in vt/snapshot.cpp.
//
// Envelope, little-endian:
//   0   4  magic "KTSN"
//   4   2  major            6   2  minor
//   8   4  flags            bit0 has scrollback, bit1 screen-only, bit2 scrollback-only, bit3 text-only
//   12  8  stream_offset    20  4  header_crc32c (of bytes 0..19)
//   24  .. sections: u16 type, u16 flags (bit1 required), u32 length, u32 crc32c, payload
//   ..  16 trailer: u16 0xFFFF, u16 0, u32 section_count, u32 total_length (whole snapshot),
//          u32 crc32c (of every byte before the trailer)
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "base/error.h"
#include "vt/types.h"

namespace kt::vt {

struct SnapshotOptions {
    bool screen_only = false;      // no SCROLLBACK section
    bool scrollback_only = false;  // META, the tables the scrollback needs, SCROLLBACK
    bool text_only = false;        // no styles, links or palette; cells keep text and width
    uint32_t scrollback_lines = UINT32_MAX;  // newest N scrollback lines; ignored if screen_only
};

struct RestoreOptions {
    // Crash recovery: leave the alternate screen and reset the modes that only make
    // sense for a running program (see Terminal::reset_program_modes).
    bool primary_only = false;
};

inline constexpr uint16_t kSnapshotMajor = 1, kSnapshotMinor = 0;

enum SnapshotFlag : uint32_t {
    kSnapHasScrollback = 1u << 0,
    kSnapScreenOnly = 1u << 1,
    kSnapScrollbackOnly = 1u << 2,
    kSnapTextOnly = 1u << 3,
};

// Receives the encoded snapshot in order, on the parsing thread. Must not block.
class SnapshotChunkSink {
public:
    virtual ~SnapshotChunkSink() = default;
    virtual void chunk(std::span<const std::byte> bytes, bool last) = 0;
};

struct SnapshotMeta {
    uint16_t major = 0, minor = 0;
    uint32_t flags = 0;
    uint64_t stream_offset = 0;
    GridSize size;
    uint32_t width_table_version = 0;
    uint64_t scrollback_rows = 0;  // rows in the SCROLLBACK section
};

// Validates the header, every section CRC and the trailer (CRC and length). Errc::corrupt on any
// mismatch, Errc::not_supported for an unknown major version. Any thread.
Result<SnapshotMeta> read_snapshot_meta(std::span<const std::byte>);

}  // namespace kt::vt
