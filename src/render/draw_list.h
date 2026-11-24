// The backend-independent draw list.
#pragma once

#include <cstdint>
#include <vector>

#include "render/types.h"

namespace kt::render {

using TextureId = uint32_t;          // 0 = none

enum class QuadKind : uint8_t {
    solid,          // color
    gradient,       // color -> color2, vertical or horizontal (flag)
    glyph_a8,       // coverage * color, gamma/contrast corrected
    glyph_lcd,      // per-channel coverage, dual-source or 3-pass fallback
    image,          // premultiplied BGRA texel * opacity (color glyphs, icons, emoji)
    rounded,        // SDF rounded rect: fill + optional border
    shadow,         // analytic gaussian of a rounded rect
};

// Quad::flags
inline constexpr uint8_t kQuadGradientHorizontal = 1;   // gradient: left -> right (else top -> bottom)
inline constexpr uint8_t kQuadBorderOnly = 2;           // rounded: no fill, border only

struct Quad {                 // 48 bytes, instance data
    float     x, y, w, h;     // physical pixels, may be fractional (UI animation)
    uint16_t  u0, v0, u1, v1; // texel coordinates in the bound texture
    uint32_t  color;          // premultiplied RGBA8, sRGB-encoded
    uint32_t  color2;         // gradient end / border color
    uint8_t   kind;           // QuadKind
    uint8_t   flags;          // gradient direction, border-only, ...
    uint16_t  clip;           // index into DrawList::clips, 0 = none
    float     radius;         // corner radius (rounded, shadow) in px
    float     param;          // border width, or shadow sigma; image: opacity (0 = 1)
    uint32_t  reserved;       // padding to 48 bytes (the fields above add up to 44)
};
static_assert(sizeof(Quad) == 48);

struct Clip { IRect rect; float radius; };   // radius > 0 => rounded clip (SDF in shader)

struct Batch {
    TextureId texture;        // 0 for untextured kinds
    uint32_t  first, count;   // range in quads
    IRect     scissor;        // axis-aligned clip from the clip stack
};

// Opacity group. Backends draw no offscreen target for it: DrawList::quad multiplies the colors
// of the quads inside by the product of the active layer opacities (per-quad opacity, exact for
// groups whose quads do not overlap, which is how the UI uses it).
struct Layer {
    IRect bounds; float opacity; uint32_t first_batch, batch_count;
};

// Scissor of batches outside any clip.
inline constexpr IRect kNoScissor{-(1 << 28), -(1 << 28), 1 << 29, 1 << 29};

// Helpers that fill a Quad for the common kinds.
Quad solid_quad(float x, float y, float w, float h, uint32_t color);
Quad glyph_quad(float x, float y, uint16_t u0, uint16_t v0, uint16_t w, uint16_t h, uint32_t color);

class DrawList {
public:
    DrawList();
    void push_clip(IRect, float radius = 0);
    void pop_clip();
    void push_layer(IRect bounds, float opacity);
    void pop_layer();
    void quad(const Quad&, TextureId tex = 0);   // merges into the current batch when possible
    void clear();
    // Built by the UI thread; read by the backend inside submit(). No locking.
    std::vector<Quad>  quads;
    std::vector<Batch> batches;
    std::vector<Clip>  clips;
    std::vector<Layer> layers;

    uint16_t current_clip() const { return clip_stack_.empty() ? 0 : clip_stack_.back(); }
    size_t clip_depth() const { return clip_stack_.size(); }
    size_t layer_depth() const { return layer_stack_.size(); }
    float layer_alpha() const { return alpha_stack_.empty() ? 1.0f : alpha_stack_.back(); }

private:
    IRect current_scissor() const;
    std::vector<uint16_t> clip_stack_;
    std::vector<uint32_t> layer_stack_;   // indices into layers
    std::vector<float> alpha_stack_;      // product of the layer opacities, per depth
    bool break_batch_ = true;
};

}  // namespace kt::render
