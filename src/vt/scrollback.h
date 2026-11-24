// Scrollback storage, first version.
//
// A ring of physical rows with two limits: a line count and a byte budget. Each row keeps its
// cells (trailing default-style blanks trimmed) and its row flags. The oldest rows go first
// when either limit is reached, so a stream of any length and any line width keeps the memory
// of a session bounded. Row storage is reused once the ring is full, so steady-state scrolling
// does not allocate; the ring grows by doubling, so a push is O(1) amortized.
//
// The byte budget counts the rows' content: their cells (8 bytes each, trailing blanks trimmed)
// plus one Line per row. It depends on the rows alone, not on the history of the storage, so a
// mirror fed from a snapshot evicts exactly the rows its host evicts. The heap the rows hold
// (heap_bytes: cell capacity plus the ring slots) stays within 1.5 times the budget: a slot gives
// its storage back when it has more than a quarter to spare, and the slots take at most a
// quarter of the budget. A budget too small for one row still keeps the newest row.
//
// TODO: replace with logical lines in packed, append-only 64 KiB pages with
// page-local style/link lists, sealed immutable pages shared with readers (ScrollbackView) and
// LZ4 for cold pages (step 13). Until then the cells here hold global StyleTable/LinkTable/
// GraphemePool indices, so the table collection pass has to visit them too
// (Terminal::collect_tables).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vt/cell.h"

namespace kt::vt {

class Scrollback {
public:
    static constexpr std::size_t kNoByteLimit = SIZE_MAX;
    explicit Scrollback(uint32_t limit, std::size_t max_bytes = kNoByteLimit) : limit_(limit), max_bytes_(max_bytes) {}

    struct Line {
        std::vector<Cell> cells;
        uint16_t flags = 0;
    };

    // Appends a row. Evicts the oldest row when the limit is reached.
    void push(std::span<const Cell> cells, uint16_t flags);
    // Removes and returns the newest row (resize pulls rows back onto the screen).
    bool pop_newest(Line& out);
    void clear();
    void set_limit(uint32_t limit);
    void set_max_bytes(std::size_t max_bytes);

    std::size_t size() const { return count_; }
    uint32_t limit() const { return limit_; }
    std::size_t max_bytes() const { return max_bytes_; }
    // The rows' content (cells plus one Line each), the figure the budget limits.
    std::size_t bytes() const { return cell_bytes_ + count_ * sizeof(Line); }
    // Heap held by the rows: cell storage capacity plus the ring slots.
    std::size_t heap_bytes() const { return cap_bytes_ + ring_.capacity() * sizeof(Line); }
    // Cells held, what a pass over every cell visits.
    std::size_t cells() const { return cell_bytes_ / sizeof(Cell); }
    // i = 0 is the oldest row.
    const Line& line(std::size_t i) const { return ring_[(head_ + i) % ring_.size()]; }
    Line& line(std::size_t i) { return ring_[(head_ + i) % ring_.size()]; }

    // Absolute line number of the oldest row; never decreases.
    uint64_t first_abs_line() const { return pushed_ - count_; }
    // Absolute line number the next pushed row gets.
    uint64_t next_abs_line() const { return pushed_; }
    // Restore (KTSN): renumbers so that the next pushed row gets `n`. Ignored if n < size().
    void set_next_abs_line(uint64_t n) {
        if (n >= count_) pushed_ = n;
    }

private:
    static std::size_t cells_bytes(const Line& l) { return l.cells.size() * sizeof(Cell); }
    static std::size_t cap_bytes(const Line& l) { return l.cells.capacity() * sizeof(Cell); }
    void forget(const Line& l) {
        cell_bytes_ -= cells_bytes(l);
        cap_bytes_ -= cap_bytes(l);
    }
    void count(const Line& l) {
        cell_bytes_ += cells_bytes(l);
        cap_bytes_ += cap_bytes(l);
    }
    // Drops the oldest row and frees its storage.
    void evict_oldest();
    // Evicts while over the byte budget, keeping at least one row.
    void enforce_bytes();
    // Most slots the ring may have: the line limit, and a quarter of the byte budget.
    std::size_t max_slots() const;
    // After a limit change: evicts and shrinks the ring to the limits.
    void shrink_to_limits();
    // The ring in order (oldest first) with room for `cap` rows; head_ becomes 0.
    void relayout(std::size_t cap);

    // Saturates: a restored snapshot may carry any line number, and wrapping past 2^64 would put
    // the counter below size(). No real stream gets there.
    void count_pushed() {
        if (pushed_ != UINT64_MAX) ++pushed_;
    }

    uint32_t limit_;
    std::size_t max_bytes_;
    std::size_t cell_bytes_ = 0;   // sum of cells_bytes() over all slots
    std::size_t cap_bytes_ = 0;    // sum of cap_bytes() over all slots
    std::vector<Line> ring_;
    std::size_t head_ = 0;   // index of the oldest row in ring_
    std::size_t count_ = 0;
    uint64_t pushed_ = 0;    // rows ever pushed minus rows popped back
};

}  // namespace kt::vt
