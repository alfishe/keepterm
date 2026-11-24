#include "view/terminal/atlas_textures.h"

namespace kt::view {

namespace {

render::TexFormat tex_format(text::PageFormat f) {
    switch (f) {
        case text::PageFormat::a8: return render::TexFormat::a8;
        case text::PageFormat::rgba8_lcd: return render::TexFormat::rgba8_lcd;
        case text::PageFormat::bgra8_premul: return render::TexFormat::bgra8_premul;
    }
    return render::TexFormat::a8;
}

}  // namespace

AtlasTextures::AtlasTextures(text::GlyphAtlas& atlas, render::IRenderer& r) : atlas_(atlas), r_(r) {}

AtlasTextures::~AtlasTextures() { reset(); }

void AtlasTextures::reset() {
    for (PageTex& p : pages_) {
        if (p.id) r_.destroy_texture(p.id);
    }
    pages_.clear();
}

void AtlasTextures::begin_frame() {
    atlas_.begin_frame(++frame_);
    in_frame_ = true;
}

void AtlasTextures::end_frame() {
    atlas_.end_frame();
    sync();
    in_frame_ = false;
}

bool AtlasTextures::sync() {
    bool stale = false;
    auto pages = atlas_.pages();
    if (pages_.size() < pages.size()) pages_.resize(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        text::AtlasPage& page = *pages[i];
        PageTex& pt = pages_[i];
        if (page.size == 0) {   // released slot
            if (pt.id) {
                r_.destroy_texture(pt.id);
                stale = true;
            }
            pt = PageTex{};
            continue;
        }
        const bool recreate = pt.id == 0 || pt.size != page.size || pt.format != page.format;
        if (!recreate && pt.generation != page.generation) stale = true;   // cleared by eviction
        if (recreate || pt.generation != page.generation) {
            if (recreate) {
                if (pt.id) r_.destroy_texture(pt.id);
                auto id = r_.create_texture(render::TextureDesc{page.size, page.size, tex_format(page.format), false});
                pt.id = id ? *id : 0;
                pt.size = page.size;
                pt.format = page.format;
            }
            pt.generation = page.generation;
            page.dirty.assign(1, text::IRect{0, 0, page.size, page.size});
        }
        if (pt.id == 0) continue;
        const uint32_t bpp = text::bytes_per_pixel(page.format);
        for (const text::IRect& d : page.dirty) {
            const uint8_t* src = page.pixels.data() + (static_cast<size_t>(d.y) * page.size + static_cast<size_t>(d.x)) * bpp;
            r_.update_texture(pt.id, render::IRect{d.x, d.y, d.w, d.h}, src, page.size * bpp);
        }
        page.dirty.clear();
    }
    return stale;
}

render::TextureId AtlasTextures::texture(uint16_t page) const { return page < pages_.size() ? pages_[page].id : 0; }

}  // namespace kt::view
