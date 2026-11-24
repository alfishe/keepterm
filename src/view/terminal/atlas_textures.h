// Mirrors the glyph atlas pages (CPU memory) into renderer textures.
//
// One per renderer, shared by every terminal view drawn with it. The owner of a window frame
// calls begin_frame() / end_frame() around building all views; a view that is built outside
// such a bracket opens one itself (single view, headless rendering, tests).
#pragma once

#include <cstdint>
#include <vector>

#include "render/renderer.h"
#include "text/atlas.h"

namespace kt::view {

class AtlasTextures {
public:
    AtlasTextures(text::GlyphAtlas&, render::IRenderer&);
    ~AtlasTextures();
    AtlasTextures(const AtlasTextures&) = delete;
    AtlasTextures& operator=(const AtlasTextures&) = delete;

    void begin_frame();        // GlyphAtlas::begin_frame with the next frame number
    void end_frame();          // GlyphAtlas::end_frame + sync()
    bool in_frame() const { return in_frame_; }
    uint64_t frame() const { return frame_; }

    // Creates and uploads textures for new or changed pages. Returns true when a page was
    // cleared by eviction or released since the last call: glyph refs into it are stale.
    bool sync();
    render::TextureId texture(uint16_t page) const;
    // Drops all textures (backend switch); the next sync() uploads every page again.
    void reset();

    text::GlyphAtlas& atlas() { return atlas_; }

private:
    struct PageTex {
        render::TextureId id = 0;
        uint32_t generation = 0;
        uint16_t size = 0;
        text::PageFormat format = text::PageFormat::a8;
    };
    text::GlyphAtlas& atlas_;
    render::IRenderer& r_;
    std::vector<PageTex> pages_;
    uint64_t frame_ = 0;
    bool in_frame_ = false;
};

}  // namespace kt::view
