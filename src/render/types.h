// Basic render types: rectangles, sizes, colors, images.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace kt::platform {
struct NativeSurface;   // defined by the platform layer
}

namespace kt::render {

struct IRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int32_t right() const { return x + w; }
    int32_t bottom() const { return y + h; }
    bool operator==(const IRect&) const = default;
};

inline IRect intersect(IRect a, IRect b) {
    const int32_t x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int32_t x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    if (x1 <= x0 || y1 <= y0) return IRect{x0, y0, 0, 0};
    return IRect{x0, y0, x1 - x0, y1 - y0};
}

inline IRect unite(IRect a, IRect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int32_t x1 = std::max(a.right(), b.right()), y1 = std::max(a.bottom(), b.bottom());
    return IRect{x0, y0, x1 - x0, y1 - y0};
}

struct PxSize {
    int32_t w = 0, h = 0;
    bool operator==(const PxSize&) const = default;
};

// Byte order of 32-bit pixels in memory. The software back buffer is BGRA8 on every OS
// (DIB section, IOSurface, X11 ZPixmap, wl_shm ARGB8888 are all B, G, R, A in memory);
// RGBA exists for presenters and tests that want it. Alpha is always the top byte.
enum class PixelOrder : uint8_t { bgra, rgba };

// Colors in draw lists: premultiplied, sRGB-encoded, packed as r | g << 8 | b << 16 | a << 24
// (RGBA in memory on little-endian hosts).
constexpr uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | ((a & 0xFF) << 24);
}
// Straight (non-premultiplied) components to a premultiplied color.
constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return pack_rgba((r * a + 127) / 255, (g * a + 127) / 255, (b * a + 127) / 255, a);
}
constexpr uint32_t ch_r(uint32_t c) { return c & 0xFF; }
constexpr uint32_t ch_g(uint32_t c) { return (c >> 8) & 0xFF; }
constexpr uint32_t ch_b(uint32_t c) { return (c >> 16) & 0xFF; }
constexpr uint32_t ch_a(uint32_t c) { return c >> 24; }

// An RGBA color as a pixel in the given order.
constexpr uint32_t to_pixel(uint32_t rgba_color, PixelOrder order) {
    if (order == PixelOrder::rgba) return rgba_color;
    return pack_rgba(ch_b(rgba_color), ch_g(rgba_color), ch_r(rgba_color), ch_a(rgba_color));
}
constexpr uint32_t from_pixel(uint32_t pixel, PixelOrder order) { return to_pixel(pixel, order); }

// Premultiplied pixels, tightly packed, top-down.
struct Image {
    int32_t w = 0, h = 0;
    PixelOrder order = PixelOrder::bgra;
    std::vector<uint32_t> pixels;
    uint32_t at(int32_t x, int32_t y) const {
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
    // Pixel as an RGBA color (r | g << 8 | b << 16 | a << 24).
    uint32_t rgba_at(int32_t x, int32_t y) const { return from_pixel(at(x, y), order); }
};

}  // namespace kt::render
