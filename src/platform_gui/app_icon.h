// The application icon as pixels, for the window systems that take it at run time (X11's
// _NET_WM_ICON). Windows reads the icon from the exe's resources (app/keepterm.rc.in), macOS
// from app_icon_png.inc (platform_macos.mm). All of it is drawn by tools/gen_icon.py.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace kt::platform {

struct AppIconImage {
    int size = 0;                       // square, size x size
    std::span<const uint8_t> rgba;      // straight alpha, rows top to bottom
};

// Every size of the icon, smallest first.
std::span<const AppIconImage> app_icon_images();

// The _NET_WM_ICON property: per image its width, its height, then one ARGB value per pixel,
// each in an `unsigned long` (Xlib's convention for format-32 properties, also on 64-bit).
std::vector<unsigned long> app_icon_net_wm_icon();

}  // namespace kt::platform
