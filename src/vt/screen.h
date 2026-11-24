// The cell grid of one screen (primary or alternate).
//
// Cells live in one contiguous array of rows x cols. A row map translates logical rows (0 = top
// of the screen) to storage slots, so scrolling a full-width region rotates the map instead of
// moving cells. Row flags travel with their slot. Each logical row has a dirty bit for the
// frame publisher.
//
// All coordinates are 0-based. Ranges are half-open [x0, x1). Callers clamp to the grid; the
// screen asserts nothing and clamps again where a bad range could touch memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vt/cell.h"

namespace kt::vt {

class Screen {
public:
    Screen(uint16_t cols, uint16_t rows);

    uint16_t cols() const { return cols_; }
    uint16_t rows() const { return rows_; }

    std::span<Cell> row(uint16_t y) { return {cells_.data() + slot_offset(y), cols_}; }
    std::span<const Cell> row(uint16_t y) const { return {cells_.data() + slot_offset(y), cols_}; }
    Cell& at(uint16_t x, uint16_t y) { return cells_[slot_offset(y) + x]; }
    const Cell& at(uint16_t x, uint16_t y) const { return cells_[slot_offset(y) + x]; }

    uint16_t flags(uint16_t y) const { return flags_[map_[y]]; }
    void set_flags(uint16_t y, uint16_t f) { flags_[map_[y]] = f; }
    void set_flag(uint16_t y, uint16_t f, bool on);

    // Damage.
    void mark_dirty(uint16_t y) { dirty_[y] = 1; any_dirty_ = true; }
    void mark_all_dirty();
    bool dirty(uint16_t y) const { return dirty_[y] != 0; }
    bool any_dirty() const { return any_dirty_; }
    void clear_dirty();

    // Writes `c` into [x0, x1) of row y, clearing the halves of wide pairs cut at the edges.
    void fill(uint16_t y, uint16_t x0, uint16_t x1, Cell c);
    // Like fill() with a blank, but skips protected cells when `respect_prot` (DECSED/DECSEL).
    void erase(uint16_t y, uint16_t x0, uint16_t x1, Cell blank, bool respect_prot = false);
    // Clears a row completely and resets its flags.
    void clear_row(uint16_t y, Cell blank);
    void clear(Cell blank);

    // Before writing cells [x0, x1) of row y: clears the other half of a wide pair cut at x0 or x1.
    void split_pairs(uint16_t y, uint16_t x0, uint16_t x1);

    // Scrolls rows [top, bottom] (inclusive) up or down by n, inside columns [left, right]
    // (inclusive). New rows are filled with `blank`.
    void scroll_up(uint16_t top, uint16_t bottom, uint16_t left, uint16_t right, uint16_t n, Cell blank);
    void scroll_down(uint16_t top, uint16_t bottom, uint16_t left, uint16_t right, uint16_t n, Cell blank);

    // ICH / DCH on row y inside [x, right] (inclusive).
    void insert_cells(uint16_t y, uint16_t x, uint16_t right, uint16_t n, Cell blank);
    void delete_cells(uint16_t y, uint16_t x, uint16_t right, uint16_t n, Cell blank);

    // Resizes without reflow: keeps the top-left corner, pads with blanks. Marks all dirty.
    void resize(uint16_t cols, uint16_t rows);
    // Removes `n` rows from the top (the caller saved them) and appends blank rows at the bottom.
    void drop_top(uint16_t n);
    // Inserts `n` blank rows at the top, dropping rows at the bottom.
    void push_top(uint16_t n);

private:
    std::size_t slot_offset(uint16_t y) const { return std::size_t{map_[y]} * cols_; }
    void fix_region_edges(uint16_t y, uint16_t x0, uint16_t x1);

    uint16_t cols_, rows_;
    std::vector<Cell> cells_;
    std::vector<uint16_t> map_;    // logical row -> slot
    std::vector<uint16_t> flags_;  // per slot
    std::vector<uint8_t> dirty_;   // per logical row
    bool any_dirty_ = true;
};

}  // namespace kt::vt
