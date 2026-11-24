// Inner loops of the software backend: 8-bit channels, 16-bit intermediates.
// Every SIMD variant is bit-exact with the scalar reference (checked by the unit tests).
// Pixels and colors here are in the back buffer's byte order with alpha in the top byte;
// the operations are symmetric in the other three channels, so BGRA and RGBA share them.
#pragma once

#include <cstdint>
#include <span>

namespace kt::render::sw {

// x / 255 rounded, exact for 16-bit products: (x + 128 + ((x + 128) >> 8)) >> 8.
constexpr uint32_t div255(uint32_t x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

struct Kernels {
    const char* name;
    // d = s + d * (255 - sa) / 255 for a premultiplied color s (plain store when sa = 255).
    void (*fill_solid)(uint32_t* d, int n, uint32_t color);
    // s = color * m / 255 per pixel, then source-over. m is the corrected coverage (LUT applied).
    void (*blend_mask)(uint32_t* d, const uint8_t* m, int n, uint32_t color);
    // s = texel * opacity / 255 per pixel, then source-over (premultiplied texels).
    void (*blend_image)(uint32_t* d, const uint32_t* s, int n, uint32_t opacity);
};

const Kernels& scalar_kernels();
// SIMD variants usable on this CPU, best first; the scalar set is always last.
std::span<const Kernels* const> available_kernels();
inline const Kernels& best_kernels() { return *available_kernels().front(); }

namespace detail {
const Kernels* sse2_kernels();   // nullptr when not compiled for this target
const Kernels* neon_kernels();
}  // namespace detail

}  // namespace kt::render::sw
