#include "render/software/kernels.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace kt::render::sw {

namespace {

inline uint32_t over(uint32_t s, uint32_t d) {
    const uint32_t inv = 255 - (s >> 24);
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        const uint32_t sc = (s >> shift) & 0xFF;
        const uint32_t dc = (d >> shift) & 0xFF;
        out |= std::min(255u, sc + div255(dc * inv)) << shift;
    }
    return out;
}

inline uint32_t scale(uint32_t c, uint32_t m) {
    return div255((c & 0xFF) * m) | (div255(((c >> 8) & 0xFF) * m) << 8) | (div255(((c >> 16) & 0xFF) * m) << 16) |
           (div255((c >> 24) * m) << 24);
}

void fill_solid_scalar(uint32_t* d, int n, uint32_t color) {
    if ((color >> 24) == 255) {
        std::fill(d, d + n, color);
        return;
    }
    if (color == 0) return;
    for (int i = 0; i < n; ++i) d[i] = over(color, d[i]);
}

void blend_mask_scalar(uint32_t* d, const uint8_t* m, int n, uint32_t color) {
    for (int i = 0; i < n; ++i) {
        const uint32_t a = m[i];
        if (a == 0) continue;
        d[i] = over(a == 255 ? color : scale(color, a), d[i]);
    }
}

void blend_image_scalar(uint32_t* d, const uint32_t* s, int n, uint32_t opacity) {
    for (int i = 0; i < n; ++i) {
        const uint32_t px = opacity == 255 ? s[i] : scale(s[i], opacity);
        if (px == 0) continue;
        d[i] = (px >> 24) == 255 ? px : over(px, d[i]);
    }
}

constexpr Kernels kScalar{"scalar", fill_solid_scalar, blend_mask_scalar, blend_image_scalar};

struct Registry {
    std::array<const Kernels*, 3> list{};
    size_t count = 0;
    Registry() {
        if (const Kernels* k = detail::neon_kernels()) list[count++] = k;
        if (const Kernels* k = detail::sse2_kernels()) list[count++] = k;
        list[count++] = &kScalar;
    }
};

}  // namespace

const Kernels& scalar_kernels() { return kScalar; }

std::span<const Kernels* const> available_kernels() {
    static const Registry reg;
    return {reg.list.data(), reg.count};
}

}  // namespace kt::render::sw
