// Geometry types of the platform layer.
#pragma once

#include <cstdint>

namespace kt::platform {

// Device-independent units: 1/96 inch on Windows and X11, points on macOS, logical pixels on
// Wayland. Physical pixels = DIP * IWindow::scale().
struct DipPoint { float x = 0, y = 0; };
struct DipRect  { float x = 0, y = 0, w = 0, h = 0; };
struct DipSize  { float w = 0, h = 0; };
struct PxSize   {
    int32_t w = 0, h = 0;
    bool operator==(const PxSize&) const = default;
};

}  // namespace kt::platform
