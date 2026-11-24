// The known pattern of the renderer probe and of the GPU golden tests.
//
// Two frames that exercise every quad kind the backends implement, without fonts: synthetic
// A8 and BGRA textures (glyph_a8, glyph_lcd, image with and without opacity), solid fills at
// fractional positions, gradients in both directions, rounded fills, borders and both together,
// a shadow, a rectangular clip, translucent overlays, then a second frame with partial damage
// and a copy_rect. A backend whose read-back matches the software backend within the golden
// tolerance renders correctly.
#pragma once

#include "render/renderer.h"

namespace kt::render {

inline constexpr PxSize kProbeSceneSize{256, 160};

// Draws both frames. The renderer must be attached (window or offscreen) with kProbeSceneSize.
// Textures are created and destroyed inside. Errors of the backend are passed through.
Result<void> draw_probe_scene(IRenderer&);

}  // namespace kt::render
