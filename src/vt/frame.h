// Frames and the frame publisher.
//
// The parsing thread publishes a copy of the active screen into one of two buffers; the UI
// thread acquires the newest one without a lock. One atomic word holds the front index, a
// reader-busy bit with the index of the buffer the reader holds, and a fresh bit. The writer
// never waits: if the reader still holds the back buffer, the publish is skipped and the damage
// stays pending until the next publish.
//
// Damage is tracked per row with a change generation. A frame's damage is every row changed
// since the frame the UI last released, so a skipped frame loses nothing.
//
// Not yet here: scroll_delta (the view could shift cached glyph runs), the palette, and a
// dynamic set beyond the row count (rows are a plain vector).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "vt/cell.h"
#include "vt/tables.h"
#include "vt/types.h"

namespace kt::vt {

class Screen;

struct FrameRow {
    std::span<const Cell> cells;
    uint16_t flags = 0;  // RowFlag bits
};

struct DamageSet {
    std::vector<uint8_t> rows;  // one entry per screen row; non-zero = changed
    bool full = false;          // size change or first frame: everything changed

    bool test(std::size_t r) const { return full || (r < rows.size() && rows[r] != 0); }
    std::size_t count() const;
};

struct Frame {
    uint64_t seq = 0;  // increases by one per publish
    GridSize size;
    std::span<const FrameRow> rows;  // the active screen, rows.size() == size.rows
    DamageSet damage;
    CursorState cursor;
    InputModes modes;
    StyleTable::Snapshot styles;      // immutable, refcounted versions
    LinkTable::Snapshot links;
    GraphemePool::Snapshot graphemes;
    uint64_t first_abs_line = 0;      // absolute line number of the screen's first row
    uint64_t scrollback_lines = 0;    // rows available above

    uint64_t gen = 0;  // change generation this frame contains (internal)
};

// What the terminal hands to publish().
struct FrameInput {
    const Screen* screen = nullptr;
    GridSize size;
    CursorState cursor;
    InputModes modes;
    StyleTable::Snapshot styles;
    LinkTable::Snapshot links;
    GraphemePool::Snapshot graphemes;
    uint64_t first_abs_line = 0;
    uint64_t scrollback_lines = 0;
};

class FramePublisher {  // single writer (parsing thread), single reader (UI thread)
public:
    FramePublisher();

    // UI side.
    void set_waker(std::function<void()> wake);  // called after a publish; must be cheap
    const Frame* acquire();                      // nullptr if nothing new
    void release(const Frame*);                  // marks the frame's damage as consumed

    // Parsing thread. Records the dirty rows of `in.screen` and writes the back buffer.
    // Returns false when the reader holds the back buffer; the damage stays pending.
    bool publish(const FrameInput& in);
    bool pending() const { return pending_; }

private:
    struct Buffer {
        Frame frame;
        std::vector<Cell> cells;
        std::vector<FrameRow> rows;
        uint64_t written_gen = 0;
        uint16_t cols = 0, rows_n = 0;
    };

    static constexpr uint32_t kFront = 1u << 0;
    static constexpr uint32_t kBusy = 1u << 1;
    static constexpr uint32_t kBusyIndex = 1u << 2;
    static constexpr uint32_t kFresh = 1u << 3;

    std::array<Buffer, 2> buf_;
    std::atomic<uint32_t> state_{0};
    std::atomic<uint64_t> consumed_gen_{0};
    std::function<void()> wake_;

    // Writer-only state.
    std::vector<uint64_t> row_gen_;  // per row: generation of its last change
    uint16_t cols_ = 0, rows_ = 0;
    uint64_t gen_ = 0;
    uint64_t size_gen_ = 0;  // generation of the last size change
    uint64_t seq_ = 0;
    bool pending_ = false;
};

}  // namespace kt::vt
