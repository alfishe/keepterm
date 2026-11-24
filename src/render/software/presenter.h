// The present seam of the software backend.
//
// The back buffer is allocated by the presenter (DIB section, IOSurface, MIT-SHM segment,
// wl_shm pool) so that present needs no copy. The backend draws into acquire()'s buffer and
// calls present() with the damage of the frame. The memory presenter is the offscreen target of
// tests, benchmarks and the probe; window presenters draw into platform surfaces.
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "base/error.h"
#include "render/types.h"

namespace kt::render::sw {

struct FrameBuffer {
    uint32_t* pixels = nullptr;   // top-down, premultiplied, sRGB-encoded
    int32_t stride = 0;           // in pixels; rows start on 64-byte boundaries
    PxSize size;
};

class ISoftwarePresenter {
public:
    virtual ~ISoftwarePresenter() = default;
    virtual PixelOrder order() const = 0;
    // (Re)allocates the back buffer(s). Contents are undefined until the next full-damage frame.
    virtual Result<void> resize(PxSize) = 0;
    // The buffer for the next frame. It holds the last presented frame: presenters with several
    // buffers (IOSurface, wl_shm) copy the undamaged part themselves.
    virtual Result<FrameBuffer> acquire() = 0;
    // Shows the frame. `damage` are disjoint rectangles in physical pixels (partial present).
    virtual Result<void> present(std::span<const IRect> damage) = 0;
};

// Offscreen presenter: one buffer in process memory.
class MemoryPresenter final : public ISoftwarePresenter {
public:
    explicit MemoryPresenter(PixelOrder order = PixelOrder::bgra) : order_(order) {}
    PixelOrder order() const override { return order_; }
    Result<void> resize(PxSize) override;
    Result<FrameBuffer> acquire() override;
    Result<void> present(std::span<const IRect> damage) override;

    uint64_t presents() const { return presents_; }
    const std::vector<IRect>& last_damage() const { return last_damage_; }

private:
    struct AlignedFree {
        void operator()(uint32_t*) const;
    };
    PixelOrder order_;
    std::unique_ptr<uint32_t, AlignedFree> buf_;
    FrameBuffer fb_;
    uint64_t presents_ = 0;
    std::vector<IRect> last_damage_;
};

// Window presenter for a platform surface: DIB section + BitBlt on Win32
// (window_presenter_win.cpp), IOSurfaces in the surface CALayer on Cocoa
// (window_presenter_macos.mm), MIT-SHM / XPutImage on X11 (window_presenter_x11_linux.cpp).
// Wayland (wl_shm) returns Errc::not_supported until the Wayland backend exists.
Result<std::unique_ptr<ISoftwarePresenter>> create_window_presenter(const platform::NativeSurface&);

}  // namespace kt::render::sw
