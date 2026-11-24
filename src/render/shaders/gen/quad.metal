// GENERATED FROM src/render/shaders/quad.hlsl - keep in sync (tools/gen_shaders.py).
//
// MSL 2.3 for the Metal backend. tools/gen_shaders.py produces this file with SPIRV-Cross from
// the SPIR-V of quad.hlsl when SPIRV-Cross is installed; this copy is a line-by-line
// translation of quad.hlsl made while SPIRV-Cross was not available on the build hosts. The
// golden tests compare its output with the software backend, like every generated shader.
//
// Bindings: buffer(0) = Frame constants, buffer(1) = instances (stage_in, per instance),
// texture(0) = the batch texture.
#include <metal_stdlib>
using namespace metal;

struct Frame
{
    float4 g_view;
    float4 g_gamma;
};

struct VsIn
{
    float4 rect   [[attribute(0)]];
    float4 uv     [[attribute(1)]];
    float4 clip   [[attribute(2)]];
    uint4  colors [[attribute(3)]];
    float4 params [[attribute(4)]];
};

struct VsOut
{
    float4 pos [[position]];
    float4 rect   [[flat]];
    float4 uv     [[flat]];
    float4 clip   [[flat]];
    uint4  colors [[flat]];
    float4 params [[flat]];
};

constant uint KIND_SOLID     = 0u;
constant uint KIND_GRADIENT  = 1u;
constant uint KIND_GLYPH_A8  = 2u;
constant uint KIND_GLYPH_LCD = 3u;
constant uint KIND_IMAGE     = 4u;
constant uint KIND_ROUNDED   = 5u;
constant uint KIND_SHADOW    = 6u;

constant uint FLAG_GRADIENT_HORIZONTAL = 1u;
constant uint FLAG_BORDER_ONLY         = 2u;

vertex VsOut vs_main(VsIn i [[stage_in]], uint vid [[vertex_id]], constant Frame& f [[buffer(0)]])
{
    const uint kind = i.colors.z & 0xFFu;
    float2 p0 = i.rect.xy;
    float2 p1 = i.rect.xy + i.rect.zw;
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
    const float2 corner = float2(float(vid & 1u), float(vid >> 1));
    const float2 p = mix(p0, p1, corner);

    VsOut o;
    o.pos = float4(p.x * f.g_view.x - 1.0, (p.y * f.g_view.y - 1.0) * f.g_view.z, 0.0, 1.0);
    o.rect = i.rect;
    o.uv = i.uv;
    o.clip = i.clip;
    o.colors = i.colors;
    o.params = i.params;
    return o;
}

static float4 unpack8(uint c)
{
    return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float(c >> 24));
}

static float q8(float v) { return floor(saturate(v) * 255.0 + 0.5); }

static float4 scale8(float4 c, float m) { return floor(c * m / 255.0 + 0.5); }

static float4 over8(float4 a, float4 b) { return min(a + floor(b * (255.0 - a.a) / 255.0 + 0.5), 255.0); }

static float text_alpha(float c8, float lum, constant Frame& f)
{
    if (c8 <= 0.0) return 0.0;
    if (c8 >= 255.0) return 255.0;
    const float k = f.g_view.w;
    const float c = c8 / 255.0;
    const float a = c * (k + 1.0) / (c * k + 1.0);
    const float r = a + a * (1.0 - a) * ((f.g_gamma.x * lum + f.g_gamma.y) * a + (f.g_gamma.z * lum + f.g_gamma.w));
    return q8(r);
}

static float erf_approx(float x)
{
    const float s = x < 0.0 ? -1.0 : 1.0;
    const float a = abs(x);
    const float t = 1.0 / (1.0 + 0.3275911 * a);
    const float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

static int2 texel(VsOut i, float2 px)
{
    const float2 duv = i.uv.zw - i.uv.xy;
    const float2 size = float2(float(i.colors.w & 0xFFFFu), float(i.colors.w >> 16));
    const float2 t = i.uv.xy + floor((px - i.rect.xy) * duv / i.rect.zw);
    return int2(clamp(t, float2(0.0, 0.0), size - 1.0));
}

fragment float4 ps_main(VsOut i [[stage_in]], constant Frame& f [[buffer(0)]], texture2d<float> g_tex [[texture(0)]])
{
    const float2 px = i.pos.xy;
    if (px.x < i.clip.x || px.y < i.clip.y || px.x >= i.clip.z || px.y >= i.clip.w) discard_fragment();

    const uint kind = i.colors.z & 0xFFu;
    const uint flags = (i.colors.z >> 8) & 0xFFu;
    const float4 color = unpack8(i.colors.x);
    float4 o = float4(0.0);

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
        if (i.uv.z <= i.uv.x || i.uv.w <= i.uv.y) discard_fragment();
        const int2 t = texel(i, px);
        const float4 s = floor(g_tex.read(uint2(t)) * 255.0 + 0.5);
        if (kind == KIND_IMAGE)
        {
            o = i.params.z >= 255.0 ? s : scale8(s, i.params.z);
        }
        else
        {
            const float c8 = kind == KIND_GLYPH_A8 ? s.r : s.a;
            const float m = text_alpha(c8, i.params.w, f);
            if (m <= 0.0) discard_fragment();
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
        const float d = length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - rad;
        const float outer = saturate(0.5 - d);
        const float inner = bw > 0.0 ? saturate(0.5 - (d + bw)) : outer;
        const float mf = (flags & FLAG_BORDER_ONLY) != 0u ? 0.0 : q8(inner);
        const float mb = bw > 0.0 ? q8(outer - inner) : 0.0;
        if (mf <= 0.0 && mb <= 0.0) discard_fragment();
        const float4 fc = scale8(color, mf);
        const float4 bc = scale8(unpack8(i.colors.y), mb);
        o = over8(bc, fc);
    }
    else if (kind == KIND_SHADOW)
    {
        const float sigma = max(i.params.y, 0.01);
        const float k2 = 1.0 / (sigma * 1.41421356);
        const float2 lo = i.rect.xy, hi = i.rect.xy + i.rect.zw;
        const float ix = 0.5 * (erf_approx((hi.x - px.x) * k2) - erf_approx((lo.x - px.x) * k2));
        const float iy = 0.5 * (erf_approx((hi.y - px.y) * k2) - erf_approx((lo.y - px.y) * k2));
        const float m = q8(ix * iy);
        if (m <= 0.0) discard_fragment();
        o = scale8(color, m);
    }
    else
    {
        discard_fragment();
    }
    return o / 255.0;
}
