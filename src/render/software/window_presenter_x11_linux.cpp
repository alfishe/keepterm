// X11 window presenter of the software backend: MIT-SHM XShmPutImage per damage rectangle,
// XPutImage when SHM is not available (remote X). libX11 and libXext are
// dlopen'ed; the platform layer has already loaded libX11 for the same Display.
#include <dlfcn.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "base/log.h"
#include "platform_gui/native_surface.h"
#include "render/software/presenter.h"

namespace kt::render::sw {

Result<std::unique_ptr<ISoftwarePresenter>> create_x11_presenter(const platform::NativeSurface&);

namespace {

constexpr int32_t kMaxSide = 16384;

struct X11Fns {
    decltype(&::XGetWindowAttributes) GetWindowAttributes = nullptr;
    decltype(&::XCreateGC) CreateGC = nullptr;
    decltype(&::XFreeGC) FreeGC = nullptr;
    decltype(&::XCreateImage) CreateImage = nullptr;
    decltype(&::XPutImage) PutImage = nullptr;
    decltype(&::XSync) Sync = nullptr;
    decltype(&::XFlush) Flush = nullptr;
    decltype(&::XSetErrorHandler) SetErrorHandler = nullptr;
    // libXext, optional
    decltype(&::XShmQueryExtension) ShmQueryExtension = nullptr;
    decltype(&::XShmAttach) ShmAttach = nullptr;
    decltype(&::XShmDetach) ShmDetach = nullptr;
    decltype(&::XShmPutImage) ShmPutImage = nullptr;
    decltype(&::XShmCreateImage) ShmCreateImage = nullptr;
    bool ok = false;
    bool shm = false;
};

template <class F>
bool load(void* lib, F& out, const char* name) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}

const X11Fns& fns() {
    static const X11Fns f = [] {
        X11Fns r;
        void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!x11) return r;
        r.ok = load(x11, r.GetWindowAttributes, "XGetWindowAttributes") && load(x11, r.CreateGC, "XCreateGC") &&
               load(x11, r.FreeGC, "XFreeGC") && load(x11, r.CreateImage, "XCreateImage") &&
               load(x11, r.PutImage, "XPutImage") && load(x11, r.Sync, "XSync") && load(x11, r.Flush, "XFlush") &&
               load(x11, r.SetErrorHandler, "XSetErrorHandler");
        if (void* xext = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL)) {
            r.shm = load(xext, r.ShmQueryExtension, "XShmQueryExtension") && load(xext, r.ShmAttach, "XShmAttach") &&
                    load(xext, r.ShmDetach, "XShmDetach") && load(xext, r.ShmPutImage, "XShmPutImage") &&
                    load(xext, r.ShmCreateImage, "XShmCreateImage");
        }
        return r;
    }();
    return f;
}

bool g_x_error = false;
int record_error(Display*, XErrorEvent*) {
    g_x_error = true;
    return 0;
}

class X11Presenter final : public ISoftwarePresenter {
public:
    X11Presenter(Display* dpy, Window win, Visual* visual, int depth, bool try_shm)
        : dpy_(dpy), win_(win), visual_(visual), depth_(depth), use_shm_(try_shm) {
        gc_ = fns().CreateGC(dpy_, win_, 0, nullptr);
    }

    ~X11Presenter() override {
        release();
        if (gc_) fns().FreeGC(dpy_, gc_);
    }

    PixelOrder order() const override { return PixelOrder::bgra; }

    Result<void> resize(PxSize size) override {
        if (size.w < 0 || size.h < 0) return make_error(Errc::invalid_argument, "negative surface size");
        size.w = std::min(size.w, kMaxSide);
        size.h = std::min(size.h, kMaxSide);
        release();
        size_ = size;
        if (size.w == 0 || size.h == 0) return {};
        // The image is `stride` pixels wide so that rows start on 64-byte boundaries; only the
        // left size.w columns are ever put on screen.
        const int32_t stride = (size.w + 15) & ~15;
        if (use_shm_ && create_shm(stride)) return {};
        use_shm_ = false;
        return create_plain(stride);
    }

    Result<FrameBuffer> acquire() override {
        return FrameBuffer{pixels_, stride_, pixels_ ? size_ : PxSize{}};
    }

    Result<void> present(std::span<const IRect> damage) override {
        if (!image_) return {};
        const IRect bounds{0, 0, size_.w, size_.h};
        for (IRect r : damage) {
            r = intersect(r, bounds);
            if (r.empty()) continue;
            if (shm_attached_) {
                fns().ShmPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, static_cast<unsigned>(r.w),
                                  static_cast<unsigned>(r.h), False);
            } else {
                fns().PutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, static_cast<unsigned>(r.w),
                               static_cast<unsigned>(r.h));
            }
        }
        if (shm_attached_) {
            // The server reads the segment asynchronously: wait before the next frame writes it.
            fns().Sync(dpy_, False);
        } else {
            fns().Flush(dpy_);
        }
        return {};
    }

private:
    bool create_shm(int32_t stride) {
        const X11Fns& f = fns();
        image_ = f.ShmCreateImage(dpy_, visual_, static_cast<unsigned>(depth_), ZPixmap, nullptr, &shm_,
                                  static_cast<unsigned>(stride), static_cast<unsigned>(size_.h));
        if (!image_) return false;
        if (image_->bits_per_pixel != 32 || image_->bytes_per_line != stride * 4) {
            destroy_image();
            return false;
        }
        const size_t bytes = static_cast<size_t>(image_->bytes_per_line) * static_cast<size_t>(size_.h);
        shm_.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (shm_.shmid < 0) {
            destroy_image();
            return false;
        }
        void* addr = shmat(shm_.shmid, nullptr, 0);
        if (addr == reinterpret_cast<void*>(-1)) {
            shmctl(shm_.shmid, IPC_RMID, nullptr);
            shm_.shmid = -1;
            destroy_image();
            return false;
        }
        shm_.shmaddr = static_cast<char*>(addr);
        shm_.readOnly = False;
        image_->data = shm_.shmaddr;
        // XShmAttach fails asynchronously on remote displays: catch the error with a round trip.
        g_x_error = false;
        auto old = f.SetErrorHandler(&record_error);
        const Bool attached = f.ShmAttach(dpy_, &shm_);
        f.Sync(dpy_, False);
        f.SetErrorHandler(old);
        shmctl(shm_.shmid, IPC_RMID, nullptr);   // freed once both sides detach
        if (!attached || g_x_error) {
            log::info("render", "MIT-SHM attach failed; using XPutImage");
            shmdt(shm_.shmaddr);
            shm_.shmaddr = nullptr;
            shm_.shmid = -1;
            image_->data = nullptr;
            destroy_image();
            return false;
        }
        shm_attached_ = true;
        pixels_ = reinterpret_cast<uint32_t*>(shm_.shmaddr);
        stride_ = stride;
        std::memset(pixels_, 0, bytes);
        return true;
    }

    Result<void> create_plain(int32_t stride) {
        const size_t bytes = static_cast<size_t>(stride) * 4 * static_cast<size_t>(size_.h);
        auto* p = static_cast<uint32_t*>(::operator new[](bytes, std::align_val_t(64), std::nothrow));
        if (!p) return make_error(Errc::unavailable, "back buffer allocation failed");
        std::memset(p, 0, bytes);
        image_ = fns().CreateImage(dpy_, visual_, static_cast<unsigned>(depth_), ZPixmap, 0, reinterpret_cast<char*>(p),
                                   static_cast<unsigned>(stride), static_cast<unsigned>(size_.h), 32, stride * 4);
        if (!image_) {
            ::operator delete[](p, std::align_val_t(64));
            return make_error(Errc::os_error, "XCreateImage failed");
        }
        image_->byte_order = LSBFirst;   // our pixels are little-endian BGRA; Xlib swaps for a big-endian server
        own_ = p;
        pixels_ = p;
        stride_ = stride;
        return {};
    }

    void destroy_image() {
        if (!image_) return;
        image_->data = nullptr;          // the pixels are ours (SHM segment or aligned heap)
        image_->f.destroy_image(image_);   // XDestroyImage is a macro for this
        image_ = nullptr;
    }

    void release() {
        if (shm_attached_) {
            fns().ShmDetach(dpy_, &shm_);
            fns().Sync(dpy_, False);
            shmdt(shm_.shmaddr);
            shm_attached_ = false;
            shm_.shmaddr = nullptr;
            shm_.shmid = -1;
        }
        destroy_image();
        if (own_) {
            ::operator delete[](own_, std::align_val_t(64));
            own_ = nullptr;
        }
        pixels_ = nullptr;
        stride_ = 0;
    }

    Display* dpy_;
    Window win_;
    Visual* visual_;
    int depth_;
    bool use_shm_;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    XShmSegmentInfo shm_{};
    bool shm_attached_ = false;
    uint32_t* own_ = nullptr;
    uint32_t* pixels_ = nullptr;
    int32_t stride_ = 0;
    PxSize size_;
};

}  // namespace

Result<std::unique_ptr<ISoftwarePresenter>> create_x11_presenter(const platform::NativeSurface& s) {
    if (s.ws != platform::WindowSystem::x11 || !s.display || !s.xid) {
        return make_error(Errc::invalid_argument, "not an X11 surface");
    }
    const X11Fns& f = fns();
    if (!f.ok) return make_error(Errc::unavailable, "libX11.so.6 is not available");
    auto* dpy = static_cast<Display*>(s.display);
    const auto win = static_cast<Window>(s.xid);
    XWindowAttributes wa{};
    if (!f.GetWindowAttributes(dpy, win, &wa)) return make_error(Errc::os_error, "XGetWindowAttributes failed");
    // BGRA in memory = a 24- or 32-bit TrueColor visual with red in bits 16..23.
    if ((wa.depth != 24 && wa.depth != 32) || !wa.visual || wa.visual->red_mask != 0xFF0000 ||
        wa.visual->green_mask != 0xFF00 || wa.visual->blue_mask != 0xFF) {
        return make_error(Errc::not_supported, "the window visual is not 24/32-bit BGRA TrueColor");
    }
    bool shm = false;
    if (f.shm) {
        if (const char* env = std::getenv("KT_X11_NO_SHM"); !env || !*env) shm = f.ShmQueryExtension(dpy) != 0;
    }
    return std::unique_ptr<ISoftwarePresenter>(std::make_unique<X11Presenter>(dpy, win, wa.visual, wa.depth, shm));
}

}  // namespace kt::render::sw
