// The GPU pipeline shared by D3D11, Metal, Vulkan and OpenGL.
//
// Every GPU backend renders into a persistent offscreen color target (B8G8R8A8_UNORM, sRGB-
// encoded values, premultiplied alpha) and copies it into the window's swapchain image or
// drawable at present. The target keeps the undamaged pixels between frames, so damage is
// rendered with scissors only, copy_rect is a GPU copy inside the target, and read_back
// reads the target. This is the persistent offscreen color target used
// on every backend.
//
// FrameBuilder turns draw lists into one instance stream and a list of draws: one draw per
// (batch, damage rectangle), scissored to both. Backends upload the instances once per flush
// and issue the draws in order.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "render/renderer.h"
#include "render/text_gamma.h"

namespace kt::render::gpu {

// One quad as the vertex shader reads it (src/render/shaders/quad.hlsl, struct VsIn).
struct Instance {
    float    rect[4];     // x, y, w, h
    float    uv[4];       // u0, v0, u1, v1
    float    clip[4];     // x0, y0, x1, y1
    uint32_t color;       // premultiplied RGBA8
    uint32_t color2;
    uint32_t kind_flags;  // kind | flags << 8
    uint32_t tex_size;    // w | h << 16, 0 without texture
    float    radius;
    float    param;
    float    opacity;     // image opacity 0..255 (coverage8 of Quad::param, or 255)
    float    luminance;   // text luminance bucket / 15 (CoverageLut::level_for)
};
static_assert(sizeof(Instance) == 80);

inline constexpr uint32_t kInstanceStride = sizeof(Instance);
inline constexpr uint32_t kInstanceAttributes = 5;   // float4, float4, float4, uint4, float4

// The constant buffer (cbuffer Frame).
struct FrameConstants {
    float view[4];    // 2 / w, 2 / h, NDC y sign, text contrast
    float gamma[4];   // gamma_ratios(gamma)
};
static_assert(sizeof(FrameConstants) == 32);

// y_sign: -1 where NDC y points up (D3D, Metal), +1 where it points down (Vulkan).
FrameConstants make_constants(PxSize size, float y_sign, const TextGammaParams&);

struct Draw {
    TextureId texture;   // 0 = none (the backend binds a 1x1 dummy)
    uint32_t  first;     // instance index
    uint32_t  count;
    IRect     scissor;   // non-empty, inside the surface
};

class FrameBuilder {
public:
    // Texture size lookup: false when the texture does not exist (its quads are skipped).
    using TextureSize = std::function<bool(TextureId, uint16_t& w, uint16_t& h)>;

    void begin(PxSize size, const Damage&);
    void add(const DrawList&, const TextureSize&);
    void add_copy_damage(IRect dst) { present_damage_.push_back(dst); }

    const std::vector<IRect>& damage() const { return damage_; }
    // damage + copy_rect destinations, for partial presents.
    const std::vector<IRect>& present_damage() const { return present_damage_; }
    PxSize size() const { return size_; }

    // Pending work since the last clear_pending() (flush points: copy_rect, end_frame).
    std::vector<Instance> instances;
    std::vector<Draw> draws;
    void clear_pending() {
        instances.clear();
        draws.clear();
    }

private:
    PxSize size_;
    std::vector<IRect> damage_;
    std::vector<IRect> present_damage_;
};

// Bytes per texel of a texture format as given to update_texture.
inline uint32_t texel_bytes(TexFormat f) { return f == TexFormat::a8 ? 1u : 4u; }

// Clips an update_texture region to the texture and moves the source pointer with it.
// Returns false when nothing is left.
bool clip_upload(IRect& region, const uint8_t*& pixels, uint32_t stride, uint16_t tw, uint16_t th, uint32_t bpp);

}  // namespace kt::render::gpu
