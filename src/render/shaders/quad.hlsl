// The quad shader of every GPU backend: one source in a portable HLSL
// subset. fxc compiles it to DXBC vs_4_0 / ps_4_0 (D3D11); glslang's HLSL front end compiles it
// to SPIR-V 1.0 (Vulkan), and SPIRV-Cross turns the SPIR-V into MSL (Metal) and GLSL (OpenGL).
// tools/gen_shaders.py runs the tools and writes src/render/shaders/gen/.
//
// The fragment math mirrors the `software` backend step by step (src/render/software): 8-bit
// rounding of coverage, of color * coverage and of gradient colors happens at the same points,
// so that the hardware blend (ONE, INV_SRC_ALPHA on an 8-bit UNORM target) produces the same
// pixels within +-1. Change both together; the golden tests compare them.
//
// Each quad is one instance (gpu/gpu_common.h, struct Instance); the vertex shader builds the
// 4 corners of a triangle strip from the vertex id. No vertex buffer for geometry.

cbuffer Frame : register(b0)
{
    float4 g_view;    // 2 / width, 2 / height, NDC y sign (-1 D3D/Metal, +1 Vulkan), text contrast k
    float4 g_gamma;   // gamma_ratios(gamma)
};

Texture2D<float4> g_tex : register(t0);   // fetched with Load: nearest, no sampler

struct VsIn
{
    float4 rect   : RECT;     // x, y, w, h in physical pixels
    float4 uv     : UV;       // u0, v0, u1, v1 in texels
    float4 clip   : CLIP;     // x0, y0, x1, y1: pixel centers outside are discarded
    uint4  colors : COLORS;   // color, color2 (premultiplied RGBA8), kind | flags << 8, tex w | h << 16
    float4 params : PARAMS;   // radius, param, image opacity (0..255), text luminance (level / 15)
    uint   vid    : SV_VertexID;
};

struct VsOut
{
    float4 pos : SV_Position;
    nointerpolation float4 rect   : RECT;
    nointerpolation float4 uv     : UV;
    nointerpolation float4 clip   : CLIP;
    nointerpolation uint4  colors : COLORS;
    nointerpolation float4 params : PARAMS;
};

#define KIND_SOLID     0u
#define KIND_GRADIENT  1u
#define KIND_GLYPH_A8  2u
#define KIND_GLYPH_LCD 3u
#define KIND_IMAGE     4u
#define KIND_ROUNDED   5u
#define KIND_SHADOW    6u

#define FLAG_GRADIENT_HORIZONTAL 1u
#define FLAG_BORDER_ONLY         2u

VsOut vs_main(VsIn i)
{
    const uint kind = i.colors.z & 0xFFu;
    float2 p0 = i.rect.xy;
    float2 p1 = i.rect.xy + i.rect.zw;
    // Pixels touched, as in the software binning: SDF shapes cover every pixel they reach
    // (antialiased edges), shadows extend by 3 sigma; other kinds cover pixel centers inside.
    if (kind == KIND_ROUNDED)
    {
        p0 = floor(p0);
        p1 = ceil(p1);
    }
    else if (kind == KIND_SHADOW)
    {
        const float e = 3.0 * max(i.params.y, 0.0);
        p0 = floor(p0 - e);
        p1 = ceil(p1 + e);
    }
    const float2 corner = float2(float(i.vid & 1u), float(i.vid >> 1));
    const float2 p = lerp(p0, p1, corner);

    VsOut o;
    o.pos = float4(p.x * g_view.x - 1.0, (p.y * g_view.y - 1.0) * g_view.z, 0.0, 1.0);
    o.rect = i.rect;
    o.uv = i.uv;
    o.clip = i.clip;
    o.colors = i.colors;
    o.params = i.params;
    return o;
}

// --- 8-bit arithmetic of the software kernels, in floats holding integers 0..255

float4 unpack8(uint c)
{
    return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float(c >> 24));
}

float q8(float v) { return floor(saturate(v) * 255.0 + 0.5); }          // coverage8()

float4 scale8(float4 c, float m) { return floor(c * m / 255.0 + 0.5); }  // div255(c * m)

// a over b, both premultiplied 8-bit: b' = a + div255(b * (255 - a.a)).
float4 over8(float4 a, float4 b) { return min(a + floor(b * (255.0 - a.a) / 255.0 + 0.5), 255.0); }

// Text coverage correction (render/text_gamma.cpp).
float text_alpha(float c8, float lum)
{
    if (c8 <= 0.0) return 0.0;
    if (c8 >= 255.0) return 255.0;
    const float k = g_view.w;
    const float c = c8 / 255.0;
    const float a = c * (k + 1.0) / (c * k + 1.0);
    const float r = a + a * (1.0 - a) * ((g_gamma.x * lum + g_gamma.y) * a + (g_gamma.z * lum + g_gamma.w));
    return q8(r);
}

// erf, Abramowitz and Stegun 7.1.26 (|error| < 1.5e-7).
float erf_approx(float x)
{
    const float s = x < 0.0 ? -1.0 : 1.0;
    const float a = abs(x);
    const float t = 1.0 / (1.0 + 0.3275911 * a);
    const float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

int2 texel(VsOut i, float2 px)
{
    const float2 duv = i.uv.zw - i.uv.xy;
    const float2 size = float2(float(i.colors.w & 0xFFFFu), float(i.colors.w >> 16));
    const float2 t = i.uv.xy + floor((px - i.rect.xy) * duv / i.rect.zw);
    return int2(clamp(t, float2(0.0, 0.0), size - 1.0));
}

float4 ps_main(VsOut i) : SV_Target0
{
    const float2 px = i.pos.xy;   // pixel center
    if (px.x < i.clip.x || px.y < i.clip.y || px.x >= i.clip.z || px.y >= i.clip.w) discard;

    const uint kind = i.colors.z & 0xFFu;
    const uint flags = (i.colors.z >> 8) & 0xFFu;
    const float4 color = unpack8(i.colors.x);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);

    if (kind == KIND_SOLID)
    {
        o = color;
    }
    else if (kind == KIND_GRADIENT)
    {
        const float t = (flags & FLAG_GRADIENT_HORIZONTAL) != 0u
            ? saturate((px.x - i.rect.x) / i.rect.z)
            : saturate((px.y - i.rect.y) / i.rect.w);
        const float4 color2 = unpack8(i.colors.y);
        o = floor(color + (color2 - color) * t + 0.5);
    }
    else if (kind == KIND_GLYPH_A8 || kind == KIND_GLYPH_LCD || kind == KIND_IMAGE)
    {
        if (i.uv.z <= i.uv.x || i.uv.w <= i.uv.y) discard;
        const float4 s = floor(g_tex.Load(int3(texel(i, px), 0)) * 255.0 + 0.5);
        if (kind == KIND_IMAGE)
        {
            o = i.params.z >= 255.0 ? s : scale8(s, i.params.z);
        }
        else
        {
            // LCD coverage is drawn as grayscale from its alpha channel, like the software
            // backend, until per-channel blending lands (grayscale fallback).
            const float c8 = kind == KIND_GLYPH_A8 ? s.r : s.a;
            const float m = text_alpha(c8, i.params.w);
            if (m <= 0.0) discard;
            o = m >= 255.0 ? color : scale8(color, m);
        }
    }
    else if (kind == KIND_ROUNDED)
    {
        const float2 half_size = i.rect.zw * 0.5;
        const float2 center = i.rect.xy + half_size;
        const float rad = clamp(i.params.x, 0.0, min(half_size.x, half_size.y));
        const float bw = max(i.params.y, 0.0);
        const float2 q = abs(px - center) - (half_size - rad);
        const float d = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - rad;
        const float outer = saturate(0.5 - d);
        const float inner = bw > 0.0 ? saturate(0.5 - (d + bw)) : outer;
        const float mf = (flags & FLAG_BORDER_ONLY) != 0u ? 0.0 : q8(inner);
        const float mb = bw > 0.0 ? q8(outer - inner) : 0.0;
        if (mf <= 0.0 && mb <= 0.0) discard;
        // Fill, then border over it (two blend_mask passes in the software backend).
        const float4 f = scale8(color, mf);
        const float4 b = scale8(unpack8(i.colors.y), mb);
        o = over8(b, f);
    }
    else if (kind == KIND_SHADOW)
    {
        const float sigma = max(i.params.y, 0.01);
        const float k2 = 1.0 / (sigma * 1.41421356);
        const float2 lo = i.rect.xy, hi = i.rect.xy + i.rect.zw;
        const float ix = 0.5 * (erf_approx((hi.x - px.x) * k2) - erf_approx((lo.x - px.x) * k2));
        const float iy = 0.5 * (erf_approx((hi.y - px.y) * k2) - erf_approx((lo.y - px.y) * k2));
        const float m = q8(ix * iy);
        if (m <= 0.0) discard;
        o = scale8(color, m);
    }
    else
    {
        discard;
    }
    return o / 255.0;
}
