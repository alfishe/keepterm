#include "vt/screen.h"

#include <algorithm>
#include <cstring>

namespace kt::vt {

Screen::Screen(uint16_t cols, uint16_t rows)
    : cols_(cols), rows_(rows), cells_(std::size_t{cols} * rows), map_(rows), flags_(rows, 0), dirty_(rows, 1) {
    for (uint16_t i = 0; i < rows; ++i) map_[i] = i;
}

void Screen::set_flag(uint16_t y, uint16_t f, bool on) {
    uint16_t& v = flags_[map_[y]];
    const uint32_t cur = v, bit = f;
    v = static_cast<uint16_t>(on ? (cur | bit) : (cur & ~bit));
}

void Screen::mark_all_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), uint8_t{1});
    any_dirty_ = true;
}

void Screen::clear_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), uint8_t{0});
    any_dirty_ = false;
}

void Screen::split_pairs(uint16_t y, uint16_t x0, uint16_t x1) {
    auto r = row(y);
    if (x0 < cols_ && r[x0].width() == CellWidth::WideTail) {
        r[x0] = Cell::blank(r[x0].style);
        if (x0 > 0) r[x0 - 1u] = Cell::blank(r[x0 - 1u].style);
    }
    if (x1 > 0 && x1 <= cols_ && r[x1 - 1u].width() == CellWidth::WideLead) {
        r[x1 - 1u] = Cell::blank(r[x1 - 1u].style);
        if (x1 < cols_) r[x1] = Cell::blank(r[x1].style);
    }
}

void Screen::fix_region_edges(uint16_t y, uint16_t x0, uint16_t x1) {
    if (x0 >= x1) return;
    auto r = row(y);
    if (r[x0].width() == CellWidth::WideTail) r[x0] = Cell::blank(r[x0].style);
    if (r[x1 - 1u].width() == CellWidth::WideLead) r[x1 - 1u] = Cell::blank(r[x1 - 1u].style);
}

void Screen::fill(uint16_t y, uint16_t x0, uint16_t x1, Cell c) {
    x1 = std::min(x1, cols_);
    if (x0 >= x1) return;
    split_pairs(y, x0, x1);
    auto r = row(y);
    std::fill(r.begin() + x0, r.begin() + x1, c);
    mark_dirty(y);
}

void Screen::erase(uint16_t y, uint16_t x0, uint16_t x1, Cell blank, bool respect_prot) {
    x1 = std::min(x1, cols_);
    if (x0 >= x1) return;
    if (!respect_prot) {
        fill(y, x0, x1, blank);
        return;
    }
    split_pairs(y, x0, x1);
    auto r = row(y);
    for (uint16_t x = x0; x < x1; ++x) {
        if (!r[x].prot()) r[x] = blank;
    }
    fix_region_edges(y, x0, x1);
    mark_dirty(y);
}

void Screen::clear_row(uint16_t y, Cell blank) {
    auto r = row(y);
    std::fill(r.begin(), r.end(), blank);
    flags_[map_[y]] = 0;
    mark_dirty(y);
}

void Screen::clear(Cell blank) {
    std::fill(cells_.begin(), cells_.end(), blank);
    std::fill(flags_.begin(), flags_.end(), uint16_t{0});
    mark_all_dirty();
}

void Screen::scroll_up(uint16_t top, uint16_t bottom, uint16_t left, uint16_t right, uint16_t n, Cell blank) {
    if (top > bottom || bottom >= rows_ || left > right || right >= cols_) return;
    const auto height = static_cast<uint16_t>(bottom - top + 1u);
    n = std::min(n, height);
    if (n == 0) return;
    if (left == 0 && right == cols_ - 1u) {
        std::rotate(map_.begin() + top, map_.begin() + top + n, map_.begin() + bottom + 1);
        for (uint16_t y = static_cast<uint16_t>(bottom + 1u - n); y <= bottom; ++y) clear_row(y, blank);
        for (uint16_t y = top; y <= bottom; ++y) mark_dirty(y);
        return;
    }
    const auto x1 = static_cast<uint16_t>(right + 1u);
    for (uint16_t y = top; y <= bottom; ++y) {
        split_pairs(y, left, x1);
        set_flag(y, kRowWrapped, false);
    }
    for (uint16_t y = top; y + n <= bottom; ++y) {
        auto dst = row(y);
        auto src = row(static_cast<uint16_t>(y + n));
        std::copy(src.begin() + left, src.begin() + x1, dst.begin() + left);
    }
    for (uint16_t y = static_cast<uint16_t>(bottom + 1u - n); y <= bottom; ++y) {
        auto r = row(y);
        std::fill(r.begin() + left, r.begin() + x1, blank);
    }
    for (uint16_t y = top; y <= bottom; ++y) {
        fix_region_edges(y, left, x1);
        mark_dirty(y);
    }
}

void Screen::scroll_down(uint16_t top, uint16_t bottom, uint16_t left, uint16_t right, uint16_t n, Cell blank) {
    if (top > bottom || bottom >= rows_ || left > right || right >= cols_) return;
    const auto height = static_cast<uint16_t>(bottom - top + 1u);
    n = std::min(n, height);
    if (n == 0) return;
    if (left == 0 && right == cols_ - 1u) {
        std::rotate(map_.begin() + top, map_.begin() + bottom + 1 - n, map_.begin() + bottom + 1);
        for (uint16_t y = top; y < top + n; ++y) clear_row(y, blank);
        for (uint16_t y = top; y <= bottom; ++y) mark_dirty(y);
        return;
    }
    const auto x1 = static_cast<uint16_t>(right + 1u);
    for (uint16_t y = top; y <= bottom; ++y) {
        split_pairs(y, left, x1);
        set_flag(y, kRowWrapped, false);
    }
    for (uint16_t y = bottom; y >= top + n; --y) {
        auto dst = row(y);
        auto src = row(static_cast<uint16_t>(y - n));
        std::copy(src.begin() + left, src.begin() + x1, dst.begin() + left);
    }
    for (uint16_t y = top; y < top + n; ++y) {
        auto r = row(y);
        std::fill(r.begin() + left, r.begin() + x1, blank);
    }
    for (uint16_t y = top; y <= bottom; ++y) {
        fix_region_edges(y, left, x1);
        mark_dirty(y);
    }
}

void Screen::insert_cells(uint16_t y, uint16_t x, uint16_t right, uint16_t n, Cell blank) {
    if (x > right || right >= cols_) return;
    const auto x1 = static_cast<uint16_t>(right + 1u);
    n = std::min(n, static_cast<uint16_t>(x1 - x));
    if (n == 0) return;
    split_pairs(y, x, x1);
    auto r = row(y);
    std::copy_backward(r.begin() + x, r.begin() + (x1 - n), r.begin() + x1);
    std::fill(r.begin() + x, r.begin() + x + n, blank);
    fix_region_edges(y, x, x1);
    set_flag(y, kRowWrapped, false);
    mark_dirty(y);
}

void Screen::delete_cells(uint16_t y, uint16_t x, uint16_t right, uint16_t n, Cell blank) {
    if (x > right || right >= cols_) return;
    const auto x1 = static_cast<uint16_t>(right + 1u);
    n = std::min(n, static_cast<uint16_t>(x1 - x));
    if (n == 0) return;
    split_pairs(y, x, x1);
    auto r = row(y);
    std::copy(r.begin() + x + n, r.begin() + x1, r.begin() + x);
    std::fill(r.begin() + (x1 - n), r.begin() + x1, blank);
    fix_region_edges(y, x, x1);
    set_flag(y, kRowWrapped, false);
    mark_dirty(y);
}

void Screen::resize(uint16_t cols, uint16_t rows) {
    std::vector<Cell> cells(std::size_t{cols} * rows);
    std::vector<uint16_t> flags(rows, 0);
    const uint16_t cr = std::min(rows, rows_);
    const uint16_t cc = std::min(cols, cols_);
    for (uint16_t y = 0; y < cr; ++y) {
        auto src = row(y);
        Cell* dst = cells.data() + std::size_t{y} * cols;
        std::copy(src.begin(), src.begin() + cc, dst);
        if (cc > 0 && dst[cc - 1u].width() == CellWidth::WideLead) dst[cc - 1u] = Cell::blank(dst[cc - 1u].style);
        flags[y] = flags_[map_[y]];
        if (cols != cols_) flags[y] = static_cast<uint16_t>(flags[y] & ~static_cast<uint32_t>(kRowWrapped));
    }
    cols_ = cols;
    rows_ = rows;
    cells_ = std::move(cells);
    flags_ = std::move(flags);
    map_.resize(rows);
    for (uint16_t i = 0; i < rows; ++i) map_[i] = i;
    dirty_.assign(rows, 1);
    any_dirty_ = true;
}

void Screen::drop_top(uint16_t n) {
    n = std::min(n, rows_);
    if (n == 0) return;
    std::rotate(map_.begin(), map_.begin() + n, map_.end());
    for (uint16_t y = static_cast<uint16_t>(rows_ - n); y < rows_; ++y) clear_row(y, Cell{});
    mark_all_dirty();
}

void Screen::push_top(uint16_t n) {
    n = std::min(n, rows_);
    if (n == 0) return;
    std::rotate(map_.begin(), map_.end() - n, map_.end());
    for (uint16_t y = 0; y < n; ++y) clear_row(y, Cell{});
    mark_all_dirty();
}

}  // namespace kt::vt
