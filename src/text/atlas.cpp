#include "text/atlas.h"

#include <algorithm>
#include <cstring>

namespace kt::text {

namespace {

inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

inline uint16_t shelf_bucket(uint32_t h) {
    // 1 px border above and below, rounded up to a multiple of 4.
    return static_cast<uint16_t>((h + 2 + 3) & ~3u);
}

constexpr size_t kMaxDirtyRects = 64;

}  // namespace

size_t GlyphKeyHash::operator()(const GlyphKey& k) const noexcept {
    uint64_t h = k.face;
    h = mix(h, k.glyph);
    h = mix(h, k.size_26_6);
    h = mix(h, k.variant);
    h = mix(h, (uint64_t{k.scale_q} << 24) | (uint64_t{k.mode} << 16) | (uint64_t{k.synth} << 8) | k.subpixel);
    return static_cast<size_t>(h);
}

GlyphAtlas::GlyphAtlas(uint16_t page_size, size_t memory_budget)
    : page_size_(page_size), budget_(memory_budget) {}

GlyphAtlas::~GlyphAtlas() = default;

std::span<AtlasPage* const> GlyphAtlas::pages() { return page_ptrs_; }

void GlyphAtlas::sync_page_list() {
    page_ptrs_.clear();
    for (auto& p : pages_) page_ptrs_.push_back(&p->page);
}

size_t GlyphAtlas::page_bytes(PageFormat f) const {
    return size_t{page_size_} * page_size_ * bytes_per_pixel(f);
}

void GlyphAtlas::begin_frame(uint64_t frame_no) {
    const uint64_t prev = frame_;
    frame_ = std::max(frame_no, prev);
    raster_spent_ = std::chrono::nanoseconds(0);
    // Temporary pages above the budget go away once no glyph on them was used last frame.
    for (size_t i = 0; i < pages_.size() && bytes_ > budget_; ++i) {
        PageState& ps = *pages_[i];
        if (ps.page.size != 0 && ps.over_budget && ps.last_used < prev) release(static_cast<uint16_t>(i));
    }
}

void GlyphAtlas::end_frame() {}

std::optional<AtlasRef> GlyphAtlas::find(const GlyphKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    it->second.last_used = frame_;
    if (it->second.ref.page != kNoPage) pages_[it->second.ref.page]->last_used = frame_;
    return it->second.ref;
}

std::optional<AtlasRef> GlyphAtlas::place(PageState& ps, uint16_t page_index, const GlyphBitmap& bm) {
    const uint16_t size = ps.page.size;
    const uint16_t bucket = shelf_bucket(bm.h);
    const uint32_t need_w = uint32_t{bm.w} + 2;
    Shelf* shelf = nullptr;
    for (Shelf& s : ps.shelves) {
        if (s.h == bucket && s.x + need_w <= size) {
            shelf = &s;
            break;
        }
    }
    if (!shelf) {
        if (uint32_t{ps.next_shelf_y} + bucket > size || need_w > size) return std::nullopt;
        ps.shelves.push_back(Shelf{ps.next_shelf_y, bucket, 0});
        ps.next_shelf_y = static_cast<uint16_t>(ps.next_shelf_y + bucket);
        shelf = &ps.shelves.back();
    }
    const uint16_t gx = static_cast<uint16_t>(shelf->x + 1);
    const uint16_t gy = static_cast<uint16_t>(shelf->y + 1);
    shelf->x = static_cast<uint16_t>(shelf->x + need_w);

    const uint32_t bpp = bytes_per_pixel(ps.page.format);
    const size_t row_bytes = size_t{bm.w} * bpp;
    for (uint32_t row = 0; row < bm.h; ++row) {
        std::memcpy(&ps.page.pixels[(size_t{gy} + row) * size * bpp + size_t{gx} * bpp],
                    &bm.pixels[row * row_bytes], row_bytes);
    }
    auto& dirty = ps.page.dirty;
    if (dirty.size() >= kMaxDirtyRects) {
        // Collapse into the bounding box: render uploads the union anyway.
        int32_t x0 = gx, y0 = gy, x1 = gx + bm.w, y1 = gy + bm.h;
        for (const IRect& r : dirty) {
            x0 = std::min(x0, r.x);
            y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.w);
            y1 = std::max(y1, r.y + r.h);
        }
        dirty.assign(1, IRect{x0, y0, x1 - x0, y1 - y0});
    } else {
        dirty.push_back(IRect{gx, gy, bm.w, bm.h});
    }
    ps.last_used = frame_;

    AtlasRef ref;
    ref.page = page_index;
    ref.x = gx;
    ref.y = gy;
    ref.w = bm.w;
    ref.h = bm.h;
    ref.bearing_x = bm.bearing_x;
    ref.bearing_y = bm.bearing_y;
    ref.kind = static_cast<uint8_t>(bm.format == PageFormat::a8          ? GlyphKind::a8
                                    : bm.format == PageFormat::rgba8_lcd ? GlyphKind::lcd
                                                                         : GlyphKind::color);
    return ref;
}

GlyphAtlas::PageState* GlyphAtlas::new_page(PageFormat format, uint16_t& index, bool over_budget) {
    size_t slot = pages_.size();
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i]->page.size == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= kNoPage) return nullptr;
    if (slot == pages_.size()) {
        pages_.push_back(std::make_unique<PageState>());
        sync_page_list();
    }
    PageState& ps = *pages_[slot];
    const uint32_t generation = ps.page.generation;
    ps = PageState{};
    ps.page.generation = generation + 1;
    ps.page.format = format;
    ps.page.size = page_size_;
    ps.page.pixels.assign(page_bytes(format), 0);
    ps.page.dirty.push_back(IRect{0, 0, page_size_, page_size_});   // first upload is the whole page
    ps.last_used = frame_;
    ps.over_budget = over_budget;
    bytes_ += ps.page.pixels.size();
    if (over_budget) ++over_budget_pages_;
    index = static_cast<uint16_t>(slot);
    return &ps;
}

void GlyphAtlas::evict(uint16_t page_index) {
    PageState& ps = *pages_[page_index];
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second.ref.page == page_index) it = map_.erase(it);
        else ++it;
    }
    std::fill(ps.page.pixels.begin(), ps.page.pixels.end(), uint8_t{0});
    ps.shelves.clear();
    ps.next_shelf_y = 0;
    ps.page.generation++;
    ps.page.dirty.assign(1, IRect{0, 0, ps.page.size, ps.page.size});
    ++evictions_;
}

void GlyphAtlas::release(uint16_t page_index) {
    evict(page_index);
    PageState& ps = *pages_[page_index];
    bytes_ -= ps.page.pixels.size();
    ps.page.pixels = {};
    ps.page.dirty.clear();
    ps.page.size = 0;
    ps.over_budget = false;
}

Result<AtlasRef> GlyphAtlas::insert(const GlyphKey& key, const GlyphBitmap& bm) {
    if (bm.w == 0 || bm.h == 0) {
        AtlasRef empty;
        empty.bearing_x = bm.bearing_x;
        empty.bearing_y = bm.bearing_y;
        map_[key] = Entry{empty, frame_};
        return empty;
    }
    if (uint32_t{bm.w} + 2 > page_size_ || uint32_t{bm.h} + 2 > page_size_)
        return make_error(Errc::invalid_argument, "glyph larger than an atlas page");
    if (bm.pixels.size() < size_t{bm.w} * bm.h * bytes_per_pixel(bm.format))
        return make_error(Errc::invalid_argument, "glyph bitmap too small");

    auto commit = [&](AtlasRef ref) -> AtlasRef {
        map_[key] = Entry{ref, frame_};
        return ref;
    };

    // 1. Another page of the same format, most recent first (older pages are fuller).
    for (size_t i = pages_.size(); i-- > 0;) {
        PageState& ps = *pages_[i];
        if (ps.page.size == 0 || ps.page.format != bm.format) continue;
        if (auto ref = place(ps, static_cast<uint16_t>(i), bm)) return commit(*ref);
    }
    // 2. A new page under the budget.
    uint16_t index = 0;
    if (bytes_ + page_bytes(bm.format) <= budget_) {
        if (PageState* ps = new_page(bm.format, index, false)) {
            if (auto ref = place(*ps, index, bm)) return commit(*ref);
        }
    }
    // 3. Evict the page of this format with the oldest use that was not used in this frame.
    PageState* victim = nullptr;
    uint16_t victim_index = 0;
    for (size_t i = 0; i < pages_.size(); ++i) {
        PageState& ps = *pages_[i];
        if (ps.page.size == 0 || ps.page.format != bm.format || ps.last_used >= frame_) continue;
        if (!victim || ps.last_used < victim->last_used) {
            victim = &ps;
            victim_index = static_cast<uint16_t>(i);
        }
    }
    if (victim) {
        evict(victim_index);
        if (auto ref = place(*victim, victim_index, bm)) return commit(*ref);
    }
    // 4. Every page is in use in this frame: a temporary page above the budget.
    if (PageState* ps = new_page(bm.format, index, true)) {
        if (auto ref = place(*ps, index, bm)) return commit(*ref);
    }
    return make_error(Errc::busy, "atlas full");
}

void GlyphAtlas::set_memory_budget(size_t bytes) { budget_ = bytes; }

void GlyphAtlas::set_page_size(uint16_t size) { page_size_ = size; }

void GlyphAtlas::clear() {
    map_.clear();
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i]->page.size != 0) release(static_cast<uint16_t>(i));
    }
}

void GlyphAtlas::set_raster_budget(std::chrono::microseconds budget) { raster_budget_ = budget; }

bool GlyphAtlas::raster_budget_left() const {
    return raster_budget_.count() == 0 || raster_spent_ < raster_budget_;
}

void GlyphAtlas::charge_raster(std::chrono::nanoseconds spent) { raster_spent_ += spent; }

AtlasStats GlyphAtlas::stats() const {
    AtlasStats s;
    s.glyphs = map_.size();
    for (const auto& p : pages_) s.live_pages += p->page.size != 0 ? 1u : 0u;
    s.memory_bytes = bytes_;
    s.evictions = evictions_;
    s.over_budget_pages = over_budget_pages_;
    return s;
}

}  // namespace kt::text
