// macOS window presenter of the software backend: three IOSurfaces shown as the contents of the
// surface CALayer that the platform layer created.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <array>
#include <cstring>
#include <vector>

#include "platform_gui/native_surface.h"
#include "render/software/presenter.h"

namespace kt::render::sw {

namespace {

constexpr int kBuffers = 3;
constexpr size_t kMaxStaleRects = 32;
constexpr int32_t kMaxSide = 16384;   // clamp huge windows

class IOSurfacePresenter final : public ISoftwarePresenter {
public:
    explicit IOSurfacePresenter(CALayer* layer) : layer_(layer) {}
    ~IOSurfacePresenter() override { release(); }

    PixelOrder order() const override { return PixelOrder::bgra; }

    Result<void> resize(PxSize size) override {
        if (size.w < 0 || size.h < 0) return make_error(Errc::invalid_argument, "negative surface size");
        size.w = std::min(size.w, kMaxSide);
        size.h = std::min(size.h, kMaxSide);
        release();
        size_ = size;
        if (size.w == 0 || size.h == 0) return {};
        const size_t bpr = (static_cast<size_t>(size.w) * 4 + 63) & ~size_t{63};   // 64-byte rows
        NSDictionary* props = @{
            (__bridge NSString*)kIOSurfaceWidth : @(size.w),
            (__bridge NSString*)kIOSurfaceHeight : @(size.h),
            (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
            (__bridge NSString*)kIOSurfaceBytesPerRow : @(bpr),
            (__bridge NSString*)kIOSurfacePixelFormat : @(static_cast<uint32_t>('BGRA')),
        };
        for (auto& b : bufs_) {
            b.surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            if (!b.surface) {
                release();
                return make_error(Errc::unavailable, "IOSurfaceCreate failed");
            }
            b.full_stale = true;
            b.stale.clear();
        }
        return {};
    }

    Result<FrameBuffer> acquire() override {
        if (size_.w == 0 || size_.h == 0) return FrameBuffer{};
        if (back_ >= 0) return fb_;
        // Never write the surface on screen: take the next one that the window server released.
        int pick = -1;
        for (int i = 1; i <= kBuffers; ++i) {
            const int c = ((front_ < 0 ? 0 : front_) + i) % kBuffers;
            if (c == front_) continue;
            if (!IOSurfaceIsInUse(bufs_[static_cast<size_t>(c)].surface)) {
                pick = c;
                break;
            }
        }
        if (pick < 0) pick = (front_ + 1) % kBuffers;   // all busy: reuse the oldest
        Buf& b = bufs_[static_cast<size_t>(pick)];
        if (IOSurfaceLock(b.surface, 0, nullptr) != kIOReturnSuccess) {
            return make_error(Errc::os_error, "IOSurfaceLock failed");
        }
        catch_up(b);
        back_ = pick;
        fb_.pixels = static_cast<uint32_t*>(IOSurfaceGetBaseAddress(b.surface));
        fb_.stride = static_cast<int32_t>(IOSurfaceGetBytesPerRow(b.surface) / 4);
        fb_.size = size_;
        return fb_;
    }

    Result<void> present(std::span<const IRect> damage) override {
        if (back_ < 0) return {};
        Buf& b = bufs_[static_cast<size_t>(back_)];
        IOSurfaceUnlock(b.surface, 0, nullptr);
        for (int i = 0; i < kBuffers; ++i) {
            if (i == back_) continue;
            Buf& o = bufs_[static_cast<size_t>(i)];
            if (o.full_stale) continue;
            for (const IRect& r : damage) o.stale.push_back(r);
            if (o.stale.size() > kMaxStaleRects) {
                o.full_stale = true;
                o.stale.clear();
            }
        }
        front_ = back_;
        back_ = -1;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer_.contents = (__bridge id)b.surface;
        [CATransaction commit];
        return {};
    }

private:
    struct Buf {
        IOSurfaceRef surface = nullptr;
        std::vector<IRect> stale;   // regions that differ from the front buffer
        bool full_stale = true;
    };

    // Brings `b` up to the last presented frame: the backend draws only the new damage.
    void catch_up(Buf& b) {
        if (front_ < 0) {
            b.full_stale = false;
            b.stale.clear();
            return;
        }
        Buf& f = bufs_[static_cast<size_t>(front_)];
        if (&f == &b || (!b.full_stale && b.stale.empty())) return;
        if (IOSurfaceLock(f.surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess) return;
        const auto* src = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(f.surface));
        auto* dst = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(b.surface));
        const size_t sbpr = IOSurfaceGetBytesPerRow(f.surface), dbpr = IOSurfaceGetBytesPerRow(b.surface);
        auto copy = [&](IRect r) {
            r = intersect(r, IRect{0, 0, size_.w, size_.h});
            if (r.empty()) return;
            for (int32_t y = r.y; y < r.bottom(); ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dbpr + static_cast<size_t>(r.x) * 4,
                            src + static_cast<size_t>(y) * sbpr + static_cast<size_t>(r.x) * 4,
                            static_cast<size_t>(r.w) * 4);
            }
        };
        if (b.full_stale) {
            copy(IRect{0, 0, size_.w, size_.h});
        } else {
            for (const IRect& r : b.stale) copy(r);
        }
        IOSurfaceUnlock(f.surface, kIOSurfaceLockReadOnly, nullptr);
        b.full_stale = false;
        b.stale.clear();
    }

    void release() {
        if (back_ >= 0) IOSurfaceUnlock(bufs_[static_cast<size_t>(back_)].surface, 0, nullptr);
        back_ = -1;
        front_ = -1;
        fb_ = FrameBuffer{};
        if (layer_) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer_.contents = nil;
            [CATransaction commit];
        }
        for (auto& b : bufs_) {
            if (b.surface) CFRelease(b.surface);
            b.surface = nullptr;
            b.stale.clear();
            b.full_stale = true;
        }
    }

    CALayer* layer_ = nil;
    std::array<Buf, kBuffers> bufs_;
    int front_ = -1, back_ = -1;
    PxSize size_;
    FrameBuffer fb_;
};

}  // namespace

Result<std::unique_ptr<ISoftwarePresenter>> create_window_presenter(const platform::NativeSurface& s) {
    if (s.ws != platform::WindowSystem::cocoa || !s.layer) {
        return make_error(Errc::invalid_argument, "not a Cocoa surface with a layer");
    }
    CALayer* layer = (__bridge CALayer*)s.layer;
    return std::unique_ptr<ISoftwarePresenter>(std::make_unique<IOSurfacePresenter>(layer));
}

}  // namespace kt::render::sw
