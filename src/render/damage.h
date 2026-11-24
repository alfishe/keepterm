// Damage and copy helpers shared by every backend.
#pragma once

#include <cstdint>
#include <vector>

#include "render/renderer.h"

namespace kt::render {

inline constexpr size_t kMaxDamageRects = 16;   // more are merged into their bounding box

// Disjoint cover of the union of the rectangles, so that no pixel is blended twice.
std::vector<IRect> make_disjoint(const std::vector<IRect>& rects);

// The damage of a frame as disjoint rectangles inside the surface: full damage is the surface;
// more than kMaxDamageRects rectangles become their bounding box.
std::vector<IRect> normalize_damage(const Damage&, PxSize surface);

// Clips a copy_rect to the surface, keeping source and destination the same size.
// Returns false when nothing is copied.
bool clip_copy(IRect src, int32_t dst_x, int32_t dst_y, PxSize surface, IRect& src_out, IRect& dst_out);

}  // namespace kt::render
