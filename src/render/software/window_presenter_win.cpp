// Win32 window presenter of the software backend: a top-down BGRA DIB section in a memory DC,
// BitBlt per damage rectangle into the render child window. GDI over RDP sends
// only the blitted rectangles. Uses only gdi32/user32 functions of build 14393.
#include <windows.h>

#include "platform_gui/native_surface.h"
#include "render/software/presenter.h"

namespace kt::render::sw {

namespace {

constexpr int32_t kMaxSide = 16384;

class DibPresenter final : public ISoftwarePresenter {
public:
    explicit DibPresenter(HWND hwnd) : hwnd_(hwnd) {}
    ~DibPresenter() override { release(); }

    PixelOrder order() const override { return PixelOrder::bgra; }

    Result<void> resize(PxSize size) override {
        if (size.w < 0 || size.h < 0) return make_error(Errc::invalid_argument, "negative surface size");
        size.w = std::min(size.w, kMaxSide);
        size.h = std::min(size.h, kMaxSide);
        release();
        size_ = size;
        if (size.w == 0 || size.h == 0) return {};
        // The DIB is `stride` pixels wide so that rows start on 64-byte boundaries; only the
        // left size.w columns are blitted.
        const int32_t stride = (size.w + 15) & ~15;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = stride;
        bi.bmiHeader.biHeight = -size.h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bitmap_ || !bits) {
            bitmap_ = nullptr;
            return last_os_error("CreateDIBSection failed");
        }
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) {
            release();
            return last_os_error("CreateCompatibleDC failed");
        }
        old_ = SelectObject(dc_, bitmap_);
        pixels_ = static_cast<uint32_t*>(bits);
        stride_ = stride;
        return {};
    }

    Result<FrameBuffer> acquire() override {
        // GDI batches calls: finish pending BitBlts that read the DIB before the CPU writes it.
        GdiFlush();
        return FrameBuffer{pixels_, stride_, pixels_ ? size_ : PxSize{}};
    }

    Result<void> present(std::span<const IRect> damage) override {
        if (!dc_) return {};
        HDC wdc = GetDC(hwnd_);
        if (!wdc) return make_error(Errc::lost, "GetDC failed on the render window");
        const IRect bounds{0, 0, size_.w, size_.h};
        bool ok = true;
        for (IRect r : damage) {
            r = intersect(r, bounds);
            if (r.empty()) continue;
            if (!BitBlt(wdc, r.x, r.y, r.w, r.h, dc_, r.x, r.y, SRCCOPY)) ok = false;
        }
        ReleaseDC(hwnd_, wdc);
        if (!ok) return last_os_error("BitBlt failed");
        return {};
    }

private:
    void release() {
        if (dc_) {
            if (old_) SelectObject(dc_, old_);
            DeleteDC(dc_);
        }
        if (bitmap_) DeleteObject(bitmap_);
        dc_ = nullptr;
        old_ = nullptr;
        bitmap_ = nullptr;
        pixels_ = nullptr;
        stride_ = 0;
    }

    HWND hwnd_;
    HDC dc_ = nullptr;
    HGDIOBJ old_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    uint32_t* pixels_ = nullptr;
    int32_t stride_ = 0;
    PxSize size_;
};

}  // namespace

Result<std::unique_ptr<ISoftwarePresenter>> create_window_presenter(const platform::NativeSurface& s) {
    if (s.ws != platform::WindowSystem::win32 || !s.handle) return make_error(Errc::invalid_argument, "not a Win32 surface");
    return std::unique_ptr<ISoftwarePresenter>(std::make_unique<DibPresenter>(static_cast<HWND>(s.handle)));
}

}  // namespace kt::render::sw
