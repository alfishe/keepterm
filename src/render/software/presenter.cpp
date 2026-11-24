#include "render/software/presenter.h"

#include <cstring>
#include <new>

namespace kt::render::sw {

namespace {
constexpr size_t kAlign = 64;
constexpr int32_t kMaxSide = 16384;   // clamp huge windows
}  // namespace

void MemoryPresenter::AlignedFree::operator()(uint32_t* p) const {
    ::operator delete[](p, std::align_val_t(kAlign));
}

Result<void> MemoryPresenter::resize(PxSize size) {
    if (size.w < 0 || size.h < 0) return make_error(Errc::invalid_argument, "negative surface size");
    size.w = std::min(size.w, kMaxSide);
    size.h = std::min(size.h, kMaxSide);
    const int32_t stride = (size.w + 15) & ~15;   // 64-byte rows
    buf_.reset();
    fb_ = FrameBuffer{};
    const size_t count = static_cast<size_t>(stride) * static_cast<size_t>(size.h);
    if (count > 0) {
        auto* p = static_cast<uint32_t*>(::operator new[](count * sizeof(uint32_t), std::align_val_t(kAlign), std::nothrow));
        if (!p) return make_error(Errc::unavailable, "back buffer allocation failed");
        std::memset(p, 0, count * sizeof(uint32_t));
        buf_.reset(p);
    }
    fb_ = FrameBuffer{buf_.get(), stride, size};
    return {};
}

Result<FrameBuffer> MemoryPresenter::acquire() { return fb_; }

Result<void> MemoryPresenter::present(std::span<const IRect> damage) {
    ++presents_;
    last_damage_.assign(damage.begin(), damage.end());
    return {};
}

}  // namespace kt::render::sw
