#include "vt/scrollback.h"

#include <algorithm>
#include <utility>

namespace kt::vt {

void Scrollback::relayout(std::size_t cap) {
    std::vector<Line> lines;
    lines.reserve(cap);
    for (std::size_t i = 0; i < count_; ++i) lines.push_back(std::move(line(i)));
    // Storage of the slots that held no row (freed by evict_oldest) is gone with the old ring.
    lines.resize(std::max(cap, count_));
    ring_ = std::move(lines);
    head_ = 0;
    cell_bytes_ = 0;
    cap_bytes_ = 0;
    for (const Line& l : ring_) count(l);
}

void Scrollback::evict_oldest() {
    Line& l = ring_[head_];
    forget(l);
    std::vector<Cell>().swap(l.cells);
    l.flags = 0;
    head_ = (head_ + 1) % ring_.size();
    --count_;
}

void Scrollback::enforce_bytes() {
    while (count_ > 1 && bytes() > max_bytes_) evict_oldest();
}

void Scrollback::push(std::span<const Cell> cells, uint16_t flags) {
    if (limit_ == 0) {
        count_pushed();
        return;
    }
    // Trim trailing blanks in the default style.
    std::size_t n = cells.size();
    while (n > 0 && cells[n - 1].is_blank() && cells[n - 1].style == 0 && cells[n - 1].link == 0) --n;

    if (count_ == ring_.size()) {
        // Full ring: double it (O(1) amortized) up to max_slots(), else the oldest row goes and
        // its slot is reused.
        const std::size_t cap = std::min(max_slots(), std::max<std::size_t>(16, ring_.size() * 2));
        if (cap > ring_.size()) {
            relayout(cap);
        } else {
            // The oldest row goes; its slot (and storage) takes the new row.
            head_ = (head_ + 1) % ring_.size();
            --count_;
        }
    }
    Line& slot = ring_[(head_ + count_) % ring_.size()];
    forget(slot);
    // A slot that held a longer row gives its storage back when more than a quarter would be
    // spare, so the heap stays near the content the budget counts.
    if (slot.cells.capacity() > n + n / 4) std::vector<Cell>().swap(slot.cells);
    slot.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(n));
    slot.flags = flags;
    count(slot);
    ++count_;
    count_pushed();
    if (max_bytes_ != kNoByteLimit) enforce_bytes();
}

bool Scrollback::pop_newest(Line& out) {
    if (count_ == 0) return false;
    Line& l = ring_[(head_ + count_ - 1) % ring_.size()];
    forget(l);
    out.cells.swap(l.cells);
    out.flags = l.flags;
    std::vector<Cell>().swap(l.cells);
    --count_;
    --pushed_;
    return true;
}

void Scrollback::clear() {
    // Absolute line numbers keep counting: the cleared lines are gone, not renumbered.
    ring_.clear();
    ring_.shrink_to_fit();
    head_ = 0;
    count_ = 0;
    cell_bytes_ = 0;
    cap_bytes_ = 0;
}

std::size_t Scrollback::max_slots() const {
    // The slots take at most a quarter of the byte budget; rows of blanks cost a slot each.
    const std::size_t by_bytes = max_bytes_ == kNoByteLimit ? SIZE_MAX : std::max<std::size_t>(1, max_bytes_ / (4 * sizeof(Line)));
    return std::min<std::size_t>(limit_, by_bytes);
}

void Scrollback::shrink_to_limits() {
    const std::size_t slots = max_slots();
    while (count_ > slots) evict_oldest();
    relayout(std::min(ring_.size(), slots));
    if (max_bytes_ != kNoByteLimit) enforce_bytes();
}

void Scrollback::set_limit(uint32_t limit) {
    limit_ = limit;
    shrink_to_limits();
}

void Scrollback::set_max_bytes(std::size_t max_bytes) {
    max_bytes_ = max_bytes;
    shrink_to_limits();
}

}  // namespace kt::vt
