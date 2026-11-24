// The renderer interface shared by all backends.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "base/error.h"
#include "render/draw_list.h"
#include "render/types.h"

namespace kt::render {

enum class BackendKind : uint8_t { d3d11, metal, vulkan, opengl, software };

struct Caps {
    BackendKind kind = BackendKind::software;
    std::string adapter_name;        // "NVIDIA RTX A2000", "software (AVX2, 8 threads)"
    uint32_t    vendor_id = 0, device_id = 0;
    uint32_t    max_texture_size = 0;    // >= 4096 on every supported backend
    bool        dual_source_blend = false;   // subpixel (LCD) text in one pass
    bool        partial_present = false;     // dirty rects reach the compositor
    bool        scroll_blit = false;         // copy_rect inside the back buffer is cheap
    bool        premultiplied_window_alpha = false;
    bool        vsync_control = false;
    uint8_t     frames_in_flight = 1;    // 1..3
};

enum class TexFormat : uint8_t { a8, rgba8_lcd, bgra8_premul };

struct TextureDesc {
    uint16_t w = 0, h = 0;
    TexFormat format = TexFormat::a8;
    bool linear_filter = false;      // images and scaled emoji; glyphs use nearest
};

struct Damage { std::span<const IRect> rects; bool full = false; };   // physical pixels

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual const Caps& caps() const = 0;

    // Surface lifetime follows the window. Resize keeps all textures.
    virtual Result<void> attach(const platform::NativeSurface&, PxSize size, float scale) = 0;
    virtual void         resize(PxSize size, float scale) = 0;
    virtual void         detach() = 0;
    // An offscreen target instead of a window surface (tests, the probe, benchmarks):
    // present() finishes the frame without showing it; read_back() reads the target.
    virtual Result<void> attach_offscreen(PxSize size, float scale) {
        (void)size;
        (void)scale;
        return make_error(Errc::not_supported, "no offscreen target");
    }

    // Textures. Contents always also live on the CPU side (owned by the caller),
    // so that a backend switch can re-create them.
    virtual Result<TextureId> create_texture(const TextureDesc&) = 0;
    virtual void update_texture(TextureId, IRect region, const void* pixels, uint32_t stride) = 0;
    virtual void destroy_texture(TextureId) = 0;

    // One frame. begin_frame returns false when the surface is not ready
    // (minimized, swapchain out of date): skip the frame, keep the damage.
    virtual Result<bool> begin_frame(const Damage&) = 0;
    virtual void         submit(const DrawList&) = 0;      // may be called per layer
    virtual Result<void> end_frame() = 0;
    virtual Result<void> present() = 0;                    // error kind 'lost' => fallback

    // Optional fast path for terminal scrolling (Caps::scroll_blit).
    virtual void copy_rect(IRect src, int32_t dst_x, int32_t dst_y) = 0;

    // Tests and the probe.
    virtual Result<Image> read_back(IRect) = 0;
};

}  // namespace kt::render
