// NEON kernels (arm64 baseline): 8 pixels per iteration, channels deinterleaved.
#include "render/software/kernels.h"

#if defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>

namespace kt::render::sw {

namespace {

const Kernels& scalar() { return scalar_kernels(); }

inline uint8x8_t div255_8(uint16x8_t x) {
    x = vaddq_u16(x, vdupq_n_u16(128));
    return vmovn_u16(vshrq_n_u16(vaddq_u16(x, vshrq_n_u16(x, 8)), 8));
}

// src-over of 8 premultiplied pixels, channel-planar.
inline uint8x8x4_t over8(uint8x8x4_t s, uint8x8x4_t d) {
    const uint8x8_t inv = vsub_u8(vdup_n_u8(255), s.val[3]);
    uint8x8x4_t r;
    for (int c = 0; c < 4; ++c) r.val[c] = vqadd_u8(s.val[c], div255_8(vmull_u8(d.val[c], inv)));
    return r;
}

void fill_solid_neon(uint32_t* d, int n, uint32_t color) {
    int i = 0;
    if ((color >> 24) == 255) {
        const uint32x4_t c = vdupq_n_u32(color);
        for (; i + 8 <= n; i += 8) {
            vst1q_u32(d + i, c);
            vst1q_u32(d + i + 4, c);
        }
    } else if (color != 0) {
        uint8x8x4_t s;
        for (int c = 0; c < 4; ++c) s.val[c] = vdup_n_u8(static_cast<uint8_t>(color >> (8 * c)));
        for (; i + 8 <= n; i += 8) {
            uint8_t* p = reinterpret_cast<uint8_t*>(d + i);
            vst4_u8(p, over8(s, vld4_u8(p)));
        }
    } else {
        return;
    }
    if (i < n) scalar().fill_solid(d + i, n - i, color);
}

void blend_mask_neon(uint32_t* d, const uint8_t* m, int n, uint32_t color) {
    uint8x8_t cc[4];
    for (int c = 0; c < 4; ++c) cc[c] = vdup_n_u8(static_cast<uint8_t>(color >> (8 * c)));
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint8x8_t mm = vld1_u8(m + i);
        if (vget_lane_u64(vreinterpret_u64_u8(mm), 0) == 0) continue;
        uint8x8x4_t s;
        for (int c = 0; c < 4; ++c) s.val[c] = div255_8(vmull_u8(cc[c], mm));
        uint8_t* p = reinterpret_cast<uint8_t*>(d + i);
        vst4_u8(p, over8(s, vld4_u8(p)));
    }
    if (i < n) scalar().blend_mask(d + i, m + i, n - i, color);
}

void blend_image_neon(uint32_t* d, const uint32_t* s, int n, uint32_t opacity) {
    const uint8x8_t op = vdup_n_u8(static_cast<uint8_t>(opacity));
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        uint8x8x4_t src = vld4_u8(reinterpret_cast<const uint8_t*>(s + i));
        if (opacity != 255) {
            for (int c = 0; c < 4; ++c) src.val[c] = div255_8(vmull_u8(src.val[c], op));
        }
        uint8_t* p = reinterpret_cast<uint8_t*>(d + i);
        vst4_u8(p, over8(src, vld4_u8(p)));
    }
    if (i < n) scalar().blend_image(d + i, s + i, n - i, opacity);
}

constexpr Kernels kNeon{"neon", fill_solid_neon, blend_mask_neon, blend_image_neon};

}  // namespace

const Kernels* detail::neon_kernels() { return &kNeon; }

}  // namespace kt::render::sw

#else

namespace kt::render::sw {
const Kernels* detail::neon_kernels() { return nullptr; }
}  // namespace kt::render::sw

#endif
