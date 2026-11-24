// SSE2 kernels (x86-64 baseline): 4 pixels per iteration, 16-bit lanes.
#include "render/software/kernels.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>

#include <cstring>

namespace kt::render::sw {

namespace {

const Kernels& scalar() { return scalar_kernels(); }

inline __m128i div255_16(__m128i x) {
    x = _mm_add_epi16(x, _mm_set1_epi16(128));
    return _mm_srli_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), 8);
}

// Alpha (lane 3 of each pixel) broadcast to all four lanes of that pixel.
inline __m128i alpha16(__m128i x) {
    x = _mm_shufflelo_epi16(x, _MM_SHUFFLE(3, 3, 3, 3));
    return _mm_shufflehi_epi16(x, _MM_SHUFFLE(3, 3, 3, 3));
}

// src-over of two premultiplied pixels in 16-bit lanes.
inline __m128i over16(__m128i s, __m128i d) {
    const __m128i inv = _mm_sub_epi16(_mm_set1_epi16(255), alpha16(s));
    return _mm_add_epi16(s, div255_16(_mm_mullo_epi16(d, inv)));
}

inline __m128i load4(const uint32_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
inline void store4(uint32_t* p, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }

void fill_solid_sse2(uint32_t* d, int n, uint32_t color) {
    const __m128i c = _mm_set1_epi32(static_cast<int>(color));
    int i = 0;
    if ((color >> 24) == 255) {
        for (; i + 4 <= n; i += 4) store4(d + i, c);
    } else if (color != 0) {
        const __m128i zero = _mm_setzero_si128();
        const __m128i c16 = _mm_unpacklo_epi8(c, zero);
        for (; i + 4 <= n; i += 4) {
            const __m128i px = load4(d + i);
            const __m128i lo = over16(c16, _mm_unpacklo_epi8(px, zero));
            const __m128i hi = over16(c16, _mm_unpackhi_epi8(px, zero));
            store4(d + i, _mm_packus_epi16(lo, hi));
        }
    } else {
        return;
    }
    if (i < n) scalar().fill_solid(d + i, n - i, color);
}

void blend_mask_sse2(uint32_t* d, const uint8_t* m, int n, uint32_t color) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i c16 = _mm_unpacklo_epi8(_mm_set1_epi32(static_cast<int>(color)), zero);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t m4;
        std::memcpy(&m4, m + i, 4);
        if (m4 == 0) continue;
        __m128i mm = _mm_cvtsi32_si128(static_cast<int>(m4));
        mm = _mm_unpacklo_epi8(mm, mm);    // m0 m0 m1 m1 m2 m2 m3 m3
        mm = _mm_unpacklo_epi16(mm, mm);   // m0 x4, m1 x4, m2 x4, m3 x4
        const __m128i mlo = _mm_unpacklo_epi8(mm, zero);
        const __m128i mhi = _mm_unpackhi_epi8(mm, zero);
        const __m128i slo = div255_16(_mm_mullo_epi16(c16, mlo));
        const __m128i shi = div255_16(_mm_mullo_epi16(c16, mhi));
        const __m128i px = load4(d + i);
        const __m128i lo = over16(slo, _mm_unpacklo_epi8(px, zero));
        const __m128i hi = over16(shi, _mm_unpackhi_epi8(px, zero));
        store4(d + i, _mm_packus_epi16(lo, hi));
    }
    if (i < n) scalar().blend_mask(d + i, m + i, n - i, color);
}

void blend_image_sse2(uint32_t* d, const uint32_t* s, int n, uint32_t opacity) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i op = _mm_set1_epi16(static_cast<short>(opacity));
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128i src = load4(s + i);
        __m128i slo = _mm_unpacklo_epi8(src, zero);
        __m128i shi = _mm_unpackhi_epi8(src, zero);
        if (opacity != 255) {
            slo = div255_16(_mm_mullo_epi16(slo, op));
            shi = div255_16(_mm_mullo_epi16(shi, op));
        }
        const __m128i px = load4(d + i);
        const __m128i lo = over16(slo, _mm_unpacklo_epi8(px, zero));
        const __m128i hi = over16(shi, _mm_unpackhi_epi8(px, zero));
        store4(d + i, _mm_packus_epi16(lo, hi));
    }
    if (i < n) scalar().blend_image(d + i, s + i, n - i, opacity);
}

constexpr Kernels kSse2{"sse2", fill_solid_sse2, blend_mask_sse2, blend_image_sse2};

}  // namespace

const Kernels* detail::sse2_kernels() { return &kSse2; }

}  // namespace kt::render::sw

#else

namespace kt::render::sw {
const Kernels* detail::sse2_kernels() { return nullptr; }
}  // namespace kt::render::sw

#endif
