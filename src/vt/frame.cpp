#include "vt/frame.h"

#include <algorithm>
#include <utility>

#include "vt/screen.h"

namespace kt::vt {

std::size_t DamageSet::count() const {
    if (full) return rows.size();
    return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](uint8_t v) { return v != 0; }));
}

FramePublisher::FramePublisher() = default;

void FramePublisher::set_waker(std::function<void()> wake) { wake_ = std::move(wake); }

const Frame* FramePublisher::acquire() {
    uint32_t s = state_.load(std::memory_order_acquire);
    for (;;) {
        if ((s & kFresh) == 0 || (s & kBusy) != 0) return nullptr;
        const uint32_t front = s & kFront;
        const uint32_t ns = (s & ~(kFresh | kBusyIndex)) | kBusy | (front != 0 ? kBusyIndex : 0u);
        if (state_.compare_exchange_weak(s, ns, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return &buf_[front != 0 ? 1 : 0].frame;
        }
    }
}

void FramePublisher::release(const Frame* f) {
    if (f == nullptr) return;
    consumed_gen_.store(f->gen, std::memory_order_release);
    state_.fetch_and(~kBusy, std::memory_order_acq_rel);
}

bool FramePublisher::publish(const FrameInput& in) {
    const Screen& s = *in.screen;
    ++gen_;
    if (s.cols() != cols_ || s.rows() != rows_) {
        cols_ = s.cols();
        rows_ = s.rows();
        row_gen_.assign(rows_, gen_);
        size_gen_ = gen_;
    } else {
        for (uint16_t r = 0; r < rows_; ++r) {
            if (s.dirty(r)) row_gen_[r] = gen_;
        }
    }

    uint32_t st = state_.load(std::memory_order_acquire);
    const std::size_t back = (st & kFront) != 0 ? 0 : 1;
    if ((st & kBusy) != 0 && ((st & kBusyIndex) != 0 ? 1u : 0u) == back) {
        pending_ = true;
        return false;
    }

    Buffer& b = buf_[back];
    const bool full = b.cols != cols_ || b.rows_n != rows_;
    if (full) {
        b.cols = cols_;
        b.rows_n = rows_;
        b.cells.assign(std::size_t{cols_} * rows_, Cell{});
        b.rows.resize(rows_);
        for (uint16_t r = 0; r < rows_; ++r) b.rows[r].cells = {b.cells.data() + std::size_t{r} * cols_, cols_};
    }
    for (uint16_t r = 0; r < rows_; ++r) {
        if (full || row_gen_[r] > b.written_gen) {
            auto src = s.row(r);
            std::copy(src.begin(), src.end(), b.cells.begin() + static_cast<std::ptrdiff_t>(std::size_t{r} * cols_));
        }
        b.rows[r].flags = s.flags(r);
    }
    b.written_gen = gen_;

    Frame& f = b.frame;
    f.seq = ++seq_;
    f.size = in.size;
    f.size.cols = cols_;
    f.size.rows = rows_;
    f.rows = b.rows;
    f.cursor = in.cursor;
    f.modes = in.modes;
    f.styles = in.styles;
    f.links = in.links;
    f.graphemes = in.graphemes;
    f.first_abs_line = in.first_abs_line;
    f.scrollback_lines = in.scrollback_lines;
    f.gen = gen_;
    const uint64_t consumed = consumed_gen_.load(std::memory_order_acquire);
    f.damage.full = size_gen_ > consumed;
    f.damage.rows.assign(rows_, 0);
    for (uint16_t r = 0; r < rows_; ++r) f.damage.rows[r] = row_gen_[r] > consumed ? 1 : 0;

    for (;;) {
        const uint32_t ns = (st & ~(kFront | kFresh)) | (back != 0 ? kFront : 0u) | kFresh;
        if (state_.compare_exchange_weak(st, ns, std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    pending_ = false;
    if (wake_) wake_();
    return true;
}

}  // namespace kt::vt
