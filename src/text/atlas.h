// Glyph atlas: CPU pages, shelf packing, LRU page eviction, dirty rectangles.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "base/error.h"

namespace kt::text {

// Same layout as render::IRect. Text does not include render headers
// (kt_text does not depend on kt_render); the consumer copies the values.
struct IRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

enum class PageFormat : uint8_t { a8, rgba8_lcd, bgra8_premul };   // = render::TexFormat
inline constexpr uint32_t bytes_per_pixel(PageFormat f) { return f == PageFormat::a8 ? 1u : 4u; }

enum class GlyphKind : uint8_t { a8, lcd, color };

inline constexpr uint16_t kNoPage = 0xFFFF;

// Where a glyph lives. An empty glyph (space) has page == kNoPage and w == h == 0.
// bearing_x: left edge of the bitmap from the pen position; bearing_y: top edge above
// the baseline (FreeType bitmap_left / bitmap_top), in pixels.
struct AtlasRef {
    uint16_t page = kNoPage;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int16_t bearing_x = 0, bearing_y = 0;
    uint8_t kind = 0;   // GlyphKind
    bool empty() const { return w == 0 || h == 0; }
};

struct AtlasPage {
    PageFormat format = PageFormat::a8;
    uint16_t size = 0;                     // 1024 or 2048; 0 = slot released
    std::vector<uint8_t> pixels;           // CPU copy, always present while the slot is live
    std::vector<IRect> dirty;              // since the last upload
    uint32_t generation = 0;               // bumped when cleared by eviction or released
};

// Atlas key. Procedural glyphs use face = kProceduralFace and glyph = code point;
// their cell geometry goes into `variant`.
struct GlyphKey {
    uint32_t face = 0;
    uint32_t glyph = 0;
    uint32_t size_26_6 = 0;
    uint32_t variant = 0;      // procedural: cell_w | cell_h << 10 | box_light << 20 | span << 26
    uint16_t scale_q = 0;      // horizontal scale-down in 1/4096 (0 = none)
    uint8_t  mode = 0;         // RenderMode
    uint8_t  synth = 0;        // bit 0 bold, bit 1 italic
    uint8_t  subpixel = 0;     // x bucket 0..3; terminal glyphs use 0
    bool operator==(const GlyphKey&) const = default;
};
inline constexpr uint32_t kProceduralFace = 0xFFFFFFF0u;

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept;
};

// A rasterized glyph before packing. Rows are tightly packed (w * bytes_per_pixel).
struct GlyphBitmap {
    uint16_t w = 0, h = 0;
    int16_t bearing_x = 0, bearing_y = 0;
    PageFormat format = PageFormat::a8;
    std::vector<uint8_t> pixels;
};

struct AtlasStats {
    size_t glyphs = 0;
    size_t live_pages = 0;
    size_t memory_bytes = 0;
    uint64_t evictions = 0;
    uint64_t over_budget_pages = 0;
};

class GlyphAtlas {
public:
    static constexpr size_t kDefaultBudget = 24u << 20;

    explicit GlyphAtlas(uint16_t page_size = 2048, size_t memory_budget = kDefaultBudget);
    ~GlyphAtlas();
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // Index = AtlasRef::page. Released slots have size 0 and no pixels.
    std::span<AtlasPage* const> pages();

    void begin_frame(uint64_t frame_no);   // for LRU stamps and the rasterization budget
    void end_frame();                      // render takes dirty rects after this

    // Lookup stamps the entry and its page with the current frame.
    std::optional<AtlasRef> find(const GlyphKey&);
    // Packs a bitmap (may evict a page not used in this frame). Empty bitmaps are cached as empty refs.
    Result<AtlasRef> insert(const GlyphKey&, const GlyphBitmap&);

    void set_memory_budget(size_t bytes);
    void set_page_size(uint16_t size);     // takes effect for new pages (backend max_texture_size)
    void clear();                          // drops every glyph and page

    // Per-frame rasterization budget (4 ms). Zero = unlimited (tests, prewarm).
    void set_raster_budget(std::chrono::microseconds);
    bool raster_budget_left() const;
    void charge_raster(std::chrono::nanoseconds);

    AtlasStats stats() const;
    uint64_t frame() const { return frame_; }

private:
    struct Shelf {
        uint16_t y, h, x;
    };
    struct PageState {
        AtlasPage page;
        std::vector<Shelf> shelves;
        uint16_t next_shelf_y = 0;
        uint64_t last_used = 0;
        bool over_budget = false;
    };
    struct Entry {
        AtlasRef ref;
        uint64_t last_used;
    };

    std::optional<AtlasRef> place(PageState&, uint16_t page_index, const GlyphBitmap&);
    PageState* new_page(PageFormat, uint16_t& index, bool over_budget);
    void evict(uint16_t page_index);
    void release(uint16_t page_index);
    size_t page_bytes(PageFormat) const;
    void sync_page_list();

    uint16_t page_size_;
    size_t budget_;
    uint64_t frame_ = 1;
    std::vector<std::unique_ptr<PageState>> pages_;
    std::vector<AtlasPage*> page_ptrs_;
    std::unordered_map<GlyphKey, Entry, GlyphKeyHash> map_;
    size_t bytes_ = 0;
    uint64_t evictions_ = 0;
    uint64_t over_budget_pages_ = 0;
    std::chrono::nanoseconds raster_budget_{std::chrono::milliseconds(4)};
    std::chrono::nanoseconds raster_spent_{0};
};

}  // namespace kt::text
