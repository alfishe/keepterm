// The render surface handed from the platform layer to a render backend.
// Header-only and free of OS types, so that kt_render can include it without linking
// kt_platform_gui.
#pragma once

#include <cstdint>

namespace kt::platform {

enum class WindowSystem : uint8_t { win32, cocoa, x11, wayland };

// Opaque handles for the render backend. No OS or GPU types leak out.
struct NativeSurface {
    WindowSystem ws = WindowSystem::win32;
    void*    display = nullptr;   // X11 Display*, wl_display*; null on Win32 and Cocoa
    void*    handle  = nullptr;   // HWND (child), NSView*, wl_surface*
    uint64_t xid     = 0;         // X11 Window (the render child)
    void*    layer   = nullptr;   // CAMetalLayer* / CALayer* on macOS (owned by the view)
};

struct SurfaceRequest {
    enum class Kind : uint8_t { gpu_swapchain, opengl, software_blit };
    Kind     kind = Kind::software_blit;
    uint64_t x11_visual_id = 0;   // GLX needs a specific visual; 0 = default
    bool     metal_layer = false; // macOS: CAMetalLayer instead of plain CALayer
};

}  // namespace kt::platform
