#include "render/software/software_backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "render/damage.h"
#include "render/software/kernels.h"

namespace kt::render::sw {

namespace {

// ---------------------------------------------------------------- worker pool

class WorkerPool {
public:
    explicit WorkerPool(unsigned workers) {
        for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { loop(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    // Runs fn(0..count-1); the calling thread takes part. Items are independent.
    void run(uint32_t count, const std::function<void(uint32_t)>& fn) {
        if (count == 0) return;
        if (threads_.empty() || count == 1) {
            for (uint32_t i = 0; i < count; ++i) fn(i);
            return;
        }
        {
            std::lock_guard lk(m_);
            fn_ = &fn;
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            active_ = static_cast<unsigned>(threads_.size());
            ++gen_;
        }
        cv_.notify_all();
        work(fn, count);
        std::unique_lock lk(m_);
        done_.wait(lk, [&] { return active_ == 0; });
        fn_ = nullptr;
    }

private:
    void work(const std::function<void(uint32_t)>& fn, uint32_t count) {
        for (;;) {
            const uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) break;
            fn(i);
        }
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(uint32_t)>* fn;
            uint32_t count;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                fn = fn_;
                count = count_;
            }
            work(*fn, count);
            {
                std::lock_guard lk(m_);
                if (--active_ == 0) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(uint32_t)>* fn_ = nullptr;
    uint32_t count_ = 0;
    std::atomic<uint32_t> next_{0};
    unsigned active_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

// ---------------------------------------------------------------- geometry helpers

int32_t pix_lo(float v) { return static_cast<int32_t>(std::ceil(v - 0.5f)); }   // first pixel center >= v

uint32_t lerp_color(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (uint32_t s = 0; s < 32; s += 8) {
        const float ca = static_cast<float>((a >> s) & 0xFF), cb = static_cast<float>((b >> s) & 0xFF);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << s;
    }
    return out;
}

uint8_t coverage8(float c) { return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

struct Texture {
    TextureDesc desc;
    std::vector<uint8_t> a8;       // a8
    std::vector<uint32_t> rgba;    // rgba8_lcd (as given) and bgra8_premul (in back-buffer order)
};

struct Cmd {
    Quad q;
    const Texture* tex;
    IRect rect;          // pixels to touch: quad ∩ scissor ∩ clip ∩ surface
    uint32_t color;      // q.color in back-buffer order
    uint32_t color2;
    uint8_t lut_level;
};

}  // namespace

// ---------------------------------------------------------------- Impl

struct SoftwareRenderer::Impl {
    SoftwareOptions opts;
    Caps caps;
    const Kernels* k = nullptr;
    CoverageLut lut;
    std::unique_ptr<WorkerPool> pool;
    std::unique_ptr<ISoftwarePresenter> presenter;
    PxSize size;
    float scale = 1.0f;
    FrameBuffer fb;
    bool in_frame = false;

    std::unordered_map<TextureId, std::unique_ptr<Texture>> textures;
    TextureId next_texture = 1;

    // Frame state.
    std::vector<IRect> damage;          // disjoint
    std::vector<IRect> present_damage;  // damage + copy_rect destinations
    IRect damage_bounds;
    int tiles_x = 0, tiles_y = 0;
    std::vector<std::vector<IRect>> tile_pieces;
    std::vector<std::vector<uint32_t>> tile_cmds;
    std::vector<uint32_t> active_tiles;
    std::vector<uint8_t> tile_active;
    std::vector<Cmd> cmds;
    SoftwareStats stats;

    void setup_tiles() {
        tiles_x = (size.w + kTile - 1) / kTile;
        tiles_y = (size.h + kTile - 1) / kTile;
        const size_t n = static_cast<size_t>(tiles_x) * static_cast<size_t>(tiles_y);
        tile_pieces.assign(n, {});
        tile_cmds.assign(n, {});
        tile_active.assign(n, 0);
        active_tiles.clear();
    }

    uint32_t* row(int32_t y) { return fb.pixels + static_cast<size_t>(y) * static_cast<size_t>(fb.stride); }

    void bin(const Quad& q, const Texture* tex, IRect scissor, const DrawList& dl);
    void rasterize_pending();
    void draw(const Cmd& c, IRect r);
};

void SoftwareRenderer::Impl::bin(const Quad& q, const Texture* tex, IRect scissor, const DrawList& dl) {
    const auto kind = static_cast<QuadKind>(q.kind);
    IRect r;
    if (kind == QuadKind::rounded) {
        const int32_t x0 = static_cast<int32_t>(std::floor(q.x)), y0 = static_cast<int32_t>(std::floor(q.y));
        r = IRect{x0, y0, static_cast<int32_t>(std::ceil(q.x + q.w)) - x0, static_cast<int32_t>(std::ceil(q.y + q.h)) - y0};
    } else if (kind == QuadKind::shadow) {
        const float e = 3.0f * std::max(q.param, 0.0f);
        const int32_t x0 = static_cast<int32_t>(std::floor(q.x - e)), y0 = static_cast<int32_t>(std::floor(q.y - e));
        r = IRect{x0, y0, static_cast<int32_t>(std::ceil(q.x + q.w + e)) - x0,
                  static_cast<int32_t>(std::ceil(q.y + q.h + e)) - y0};
    } else {
        const int32_t x0 = pix_lo(q.x), y0 = pix_lo(q.y);
        r = IRect{x0, y0, pix_lo(q.x + q.w) - x0, pix_lo(q.y + q.h) - y0};
    }
    r = intersect(r, scissor);
    if (q.clip != 0 && q.clip < dl.clips.size()) r = intersect(r, dl.clips[q.clip].rect);   // rounded clips: rect only for now
    r = intersect(r, damage_bounds);
    if (r.empty()) return;
    if ((kind == QuadKind::glyph_a8 || kind == QuadKind::glyph_lcd || kind == QuadKind::image) && !tex) return;
    if (q.w <= 0 || q.h <= 0) return;

    const PixelOrder order = presenter->order();
    const auto idx = static_cast<uint32_t>(cmds.size());
    cmds.push_back(Cmd{q, tex, r, to_pixel(q.color, order), to_pixel(q.color2, order),
                       static_cast<uint8_t>(CoverageLut::level_for(q.color))});
    const int tx0 = r.x / kTile, tx1 = (r.right() - 1) / kTile;
    const int ty0 = r.y / kTile, ty1 = (r.bottom() - 1) / kTile;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const size_t t = static_cast<size_t>(ty) * static_cast<size_t>(tiles_x) + static_cast<size_t>(tx);
            if (tile_active[t]) tile_cmds[t].push_back(idx);
        }
    }
}

void SoftwareRenderer::Impl::draw(const Cmd& c, IRect r) {
    const Quad& q = c.q;
    const Kernels& kk = *k;
    alignas(16) uint8_t mask[kTile];
    alignas(16) uint32_t px[kTile];
    const int n = r.w;
    switch (static_cast<QuadKind>(q.kind)) {
        case QuadKind::solid:
            for (int32_t y = r.y; y < r.bottom(); ++y) kk.fill_solid(row(y) + r.x, n, c.color);
            break;
        case QuadKind::gradient: {
            if (q.flags & kQuadGradientHorizontal) {
                for (int i = 0; i < n; ++i) {
                    const float t = std::clamp((static_cast<float>(r.x + i) + 0.5f - q.x) / q.w, 0.0f, 1.0f);
                    px[i] = lerp_color(c.color, c.color2, t);
                }
                for (int32_t y = r.y; y < r.bottom(); ++y) kk.blend_image(row(y) + r.x, px, n, 255);
            } else {
                for (int32_t y = r.y; y < r.bottom(); ++y) {
                    const float t = std::clamp((static_cast<float>(y) + 0.5f - q.y) / q.h, 0.0f, 1.0f);
                    kk.fill_solid(row(y) + r.x, n, lerp_color(c.color, c.color2, t));
                }
            }
            break;
        }
        case QuadKind::glyph_a8:
        case QuadKind::glyph_lcd:
        case QuadKind::image: {
            const Texture& tex = *c.tex;
            const int32_t tw = tex.desc.w, th = tex.desc.h;
            const int32_t du = int32_t{q.u1} - q.u0, dv = int32_t{q.v1} - q.v0;
            if (du <= 0 || dv <= 0) break;
            // Nearest sampling; cell-aligned glyphs map 1:1.
            const bool exact = static_cast<float>(du) == q.w && static_cast<float>(dv) == q.h &&
                               std::floor(q.x) == q.x && std::floor(q.y) == q.y;
            const int32_t qx = static_cast<int32_t>(q.x), qy = static_cast<int32_t>(q.y);
            int32_t us[kTile];
            for (int i = 0; i < n; ++i) {
                const int32_t x = r.x + i;
                int32_t u = exact ? q.u0 + (x - qx)
                                  : q.u0 + static_cast<int32_t>(std::floor((static_cast<float>(x) + 0.5f - q.x) * static_cast<float>(du) / q.w));
                us[i] = std::clamp(u, 0, tw - 1);
            }
            const uint8_t* lut_row = lut.row(c.lut_level);
            const bool is_image = static_cast<QuadKind>(q.kind) == QuadKind::image;
            const uint32_t opacity = q.param > 0.0f && q.param < 1.0f ? coverage8(q.param) : 255u;
            for (int32_t y = r.y; y < r.bottom(); ++y) {
                int32_t v = exact ? q.v0 + (y - qy)
                                  : q.v0 + static_cast<int32_t>(std::floor((static_cast<float>(y) + 0.5f - q.y) * static_cast<float>(dv) / q.h));
                v = std::clamp(v, 0, th - 1);
                const size_t base = static_cast<size_t>(v) * static_cast<size_t>(tw);
                if (is_image) {
                    if (tex.desc.format == TexFormat::a8) break;
                    for (int i = 0; i < n; ++i) px[i] = tex.rgba[base + static_cast<size_t>(us[i])];
                    kk.blend_image(row(y) + r.x, px, n, opacity);
                } else {
                    if (tex.desc.format == TexFormat::a8) {
                        for (int i = 0; i < n; ++i) mask[i] = lut_row[tex.a8[base + static_cast<size_t>(us[i])]];
                    } else {
                        // LCD coverage drawn as grayscale from its max channel until per-channel
                        // blending lands (grayscale fallback).
                        for (int i = 0; i < n; ++i) mask[i] = lut_row[tex.rgba[base + static_cast<size_t>(us[i])] >> 24];
                    }
                    kk.blend_mask(row(y) + r.x, mask, n, c.color);
                }
            }
            break;
        }
        case QuadKind::rounded: {
            const float hw = q.w * 0.5f, hh = q.h * 0.5f;
            const float cx = q.x + hw, cy = q.y + hh;
            const float rad = std::clamp(q.radius, 0.0f, std::min(hw, hh));
            const float bw = std::max(q.param, 0.0f);
            const bool border_only = (q.flags & kQuadBorderOnly) != 0;
            alignas(16) uint8_t bmask[kTile];
            for (int32_t y = r.y; y < r.bottom(); ++y) {
                const float py = std::abs(static_cast<float>(y) + 0.5f - cy) - (hh - rad);
                bool any_fill = false, any_border = false;
                for (int i = 0; i < n; ++i) {
                    const float pxx = std::abs(static_cast<float>(r.x + i) + 0.5f - cx) - (hw - rad);
                    const float ox = std::max(pxx, 0.0f), oy = std::max(py, 0.0f);
                    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(pxx, py), 0.0f) - rad;
                    const float outer = std::clamp(0.5f - d, 0.0f, 1.0f);
                    const float inner = bw > 0.0f ? std::clamp(0.5f - (d + bw), 0.0f, 1.0f) : outer;
                    mask[i] = border_only ? 0 : coverage8(inner);
                    bmask[i] = bw > 0.0f ? coverage8(outer - inner) : 0;
                    any_fill |= mask[i] != 0;
                    any_border |= bmask[i] != 0;
                }
                if (any_fill) kk.blend_mask(row(y) + r.x, mask, n, c.color);
                if (any_border) kk.blend_mask(row(y) + r.x, bmask, n, c.color2);
            }
            break;
        }
        case QuadKind::shadow: {
            // Analytic gaussian of the box (product of erf differences). Corner rounding of the
            // blurred shape is not applied yet.
            const float sigma = std::max(q.param, 0.01f);
            const float k2 = 1.0f / (sigma * 1.41421356f);
            float ix[kTile];
            for (int i = 0; i < n; ++i) {
                const float p = static_cast<float>(r.x + i) + 0.5f;
                ix[i] = 0.5f * (std::erf((q.x + q.w - p) * k2) - std::erf((q.x - p) * k2));
            }
            for (int32_t y = r.y; y < r.bottom(); ++y) {
                const float p = static_cast<float>(y) + 0.5f;
                const float iy = 0.5f * (std::erf((q.y + q.h - p) * k2) - std::erf((q.y - p) * k2));
                for (int i = 0; i < n; ++i) mask[i] = coverage8(ix[i] * iy);
                kk.blend_mask(row(y) + r.x, mask, n, c.color);
            }
            break;
        }
    }
}

void SoftwareRenderer::Impl::rasterize_pending() {
    if (cmds.empty()) return;
    uint32_t commands = 0;
    for (uint32_t t : active_tiles) commands += static_cast<uint32_t>(tile_cmds[t].size());
    pool->run(static_cast<uint32_t>(active_tiles.size()), [&](uint32_t i) {
        const uint32_t t = active_tiles[i];
        for (uint32_t ci : tile_cmds[t]) {
            const Cmd& c = cmds[ci];
            for (const IRect& piece : tile_pieces[t]) {
                const IRect r = intersect(piece, c.rect);
                if (!r.empty()) draw(c, r);
            }
        }
    });
    stats.tiles = static_cast<uint32_t>(active_tiles.size());
    stats.commands += commands;
    for (uint32_t t : active_tiles) tile_cmds[t].clear();
    cmds.clear();
}

// ---------------------------------------------------------------- SoftwareRenderer

SoftwareRenderer::SoftwareRenderer(const SoftwareOptions& opts) : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    s.opts = opts;
    s.k = opts.kernels ? opts.kernels : &best_kernels();
    s.lut.build(opts.text_gamma);
    unsigned workers = opts.threads;
    if (workers == 0) {
        const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        workers = std::min(hc > 2 ? hc - 1 : 1u, 8u);
    }
    // The calling thread rasterizes too, so the pool has one thread less.
    s.pool = std::make_unique<WorkerPool>(workers - 1);
    s.caps.kind = BackendKind::software;
    s.caps.adapter_name = std::string("software (") + s.k->name + ", " + std::to_string(workers) +
                          (workers == 1 ? " thread)" : " threads)");
    s.caps.max_texture_size = 16384;
    s.caps.dual_source_blend = false;
    s.caps.partial_present = true;
    s.caps.scroll_blit = true;
    s.caps.premultiplied_window_alpha = true;
    s.caps.vsync_control = false;
    s.caps.frames_in_flight = 1;
}

SoftwareRenderer::~SoftwareRenderer() = default;

const Caps& SoftwareRenderer::caps() const { return impl_->caps; }

const SoftwareStats& SoftwareRenderer::stats() const { return impl_->stats; }

ISoftwarePresenter* SoftwareRenderer::presenter() const { return impl_->presenter.get(); }

void SoftwareRenderer::set_text_gamma(const TextGammaParams& p) { impl_->lut.build(p); }

Result<void> SoftwareRenderer::attach(const platform::NativeSurface& surface, PxSize size, float scale) {
    auto p = create_window_presenter(surface);
    if (!p) return Unexpected<Error>(p.error());
    return attach_presenter(std::move(*p), size, scale);
}

Result<void> SoftwareRenderer::attach_presenter(std::unique_ptr<ISoftwarePresenter> p, PxSize size, float scale) {
    if (!p) return make_error(Errc::invalid_argument, "null presenter");
    impl_->presenter = std::move(p);
    impl_->size = PxSize{};
    impl_->scale = scale;
    if (auto r = impl_->presenter->resize(size); !r) return r;
    impl_->size = PxSize{std::min(size.w, 16384), std::min(size.h, 16384)};
    impl_->setup_tiles();
    return {};
}

Result<void> SoftwareRenderer::attach_offscreen(PxSize size, float scale) {
    return attach_presenter(std::make_unique<MemoryPresenter>(), size, scale);
}

void SoftwareRenderer::resize(PxSize size, float scale) {
    Impl& s = *impl_;
    s.scale = scale;
    if (!s.presenter) return;
    if (!s.presenter->resize(size)) {
        s.size = PxSize{};   // allocation failed: frames are skipped until the next resize
    } else {
        s.size = PxSize{std::min(size.w, 16384), std::min(size.h, 16384)};
    }
    s.setup_tiles();
}

void SoftwareRenderer::detach() {
    impl_->presenter.reset();
    impl_->size = PxSize{};
    impl_->setup_tiles();
}

Result<TextureId> SoftwareRenderer::create_texture(const TextureDesc& desc) {
    if (desc.w == 0 || desc.h == 0) return make_error(Errc::invalid_argument, "empty texture");
    auto t = std::make_unique<Texture>();
    t->desc = desc;
    const size_t n = size_t{desc.w} * desc.h;
    if (desc.format == TexFormat::a8) t->a8.assign(n, 0);
    else t->rgba.assign(n, 0);
    const TextureId id = impl_->next_texture++;
    impl_->textures.emplace(id, std::move(t));
    return id;
}

void SoftwareRenderer::update_texture(TextureId id, IRect region, const void* pixels, uint32_t stride) {
    auto it = impl_->textures.find(id);
    if (it == impl_->textures.end() || !pixels) return;
    Texture& t = *it->second;
    const IRect r = intersect(region, IRect{0, 0, t.desc.w, t.desc.h});
    if (r.empty()) return;
    const auto* src = static_cast<const uint8_t*>(pixels);
    // `pixels` points at the region's top-left texel; clipping moves the start.
    const size_t bpp = t.desc.format == TexFormat::a8 ? 1 : 4;
    src += static_cast<size_t>(r.y - region.y) * stride + static_cast<size_t>(r.x - region.x) * bpp;
    const PixelOrder order = impl_->presenter ? impl_->presenter->order() : PixelOrder::bgra;
    for (int32_t y = 0; y < r.h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * stride;
        const size_t dst = static_cast<size_t>(r.y + y) * t.desc.w + static_cast<size_t>(r.x);
        if (t.desc.format == TexFormat::a8) {
            std::memcpy(&t.a8[dst], s, static_cast<size_t>(r.w));
        } else {
            std::memcpy(&t.rgba[dst], s, static_cast<size_t>(r.w) * 4);
            if (t.desc.format == TexFormat::bgra8_premul && order == PixelOrder::rgba) {
                for (int32_t x = 0; x < r.w; ++x) {
                    uint32_t& p = t.rgba[dst + static_cast<size_t>(x)];
                    p = to_pixel(p, PixelOrder::bgra);   // swap R and B
                }
            }
        }
    }
}

void SoftwareRenderer::destroy_texture(TextureId id) { impl_->textures.erase(id); }

Result<bool> SoftwareRenderer::begin_frame(const Damage& damage) {
    Impl& s = *impl_;
    if (!s.presenter || s.size.w <= 0 || s.size.h <= 0) return false;
    auto fb = s.presenter->acquire();
    if (!fb) return Unexpected<Error>(fb.error());
    if (!fb->pixels) return false;
    s.fb = *fb;
    s.damage = normalize_damage(damage, s.size);
    s.present_damage = s.damage;
    s.damage_bounds = IRect{};
    for (const IRect& r : s.damage) s.damage_bounds = unite(s.damage_bounds, r);

    for (uint32_t t : s.active_tiles) {
        s.tile_active[t] = 0;
        s.tile_pieces[t].clear();
        s.tile_cmds[t].clear();
    }
    s.active_tiles.clear();
    for (const IRect& r : s.damage) {
        for (int ty = r.y / kTile; ty <= (r.bottom() - 1) / kTile; ++ty) {
            for (int tx = r.x / kTile; tx <= (r.right() - 1) / kTile; ++tx) {
                const uint32_t t = static_cast<uint32_t>(ty * s.tiles_x + tx);
                const IRect piece = intersect(r, IRect{tx * kTile, ty * kTile, kTile, kTile});
                if (piece.empty()) continue;
                if (!s.tile_active[t]) {
                    s.tile_active[t] = 1;
                    s.active_tiles.push_back(t);
                }
                s.tile_pieces[t].push_back(piece);
            }
        }
    }
    s.cmds.clear();
    s.stats.commands = 0;
    s.stats.quads = 0;
    s.stats.tiles = 0;
    s.in_frame = true;
    return true;
}

void SoftwareRenderer::submit(const DrawList& dl) {
    Impl& s = *impl_;
    if (!s.in_frame || s.active_tiles.empty()) return;
    // Layers (opacity groups) have no offscreen pass: DrawList::quad already scaled the colors
    // of their quads by the layer opacity.
    for (const Batch& b : dl.batches) {
        const Texture* tex = nullptr;
        if (b.texture != 0) {
            auto it = s.textures.find(b.texture);
            if (it != s.textures.end()) tex = it->second.get();
        }
        const uint32_t end = std::min<uint32_t>(b.first + b.count, static_cast<uint32_t>(dl.quads.size()));
        for (uint32_t i = b.first; i < end; ++i) s.bin(dl.quads[i], tex, b.scissor, dl);
        s.stats.quads += end > b.first ? end - b.first : 0;
    }
}

Result<void> SoftwareRenderer::end_frame() {
    Impl& s = *impl_;
    if (!s.in_frame) return make_error(Errc::invalid_argument, "end_frame without begin_frame");
    s.rasterize_pending();
    s.in_frame = false;
    ++s.stats.frames;
    return {};
}

Result<void> SoftwareRenderer::present() {
    Impl& s = *impl_;
    if (!s.presenter) return make_error(Errc::unavailable, "no surface");
    return s.presenter->present(s.present_damage);
}

void SoftwareRenderer::copy_rect(IRect src, int32_t dst_x, int32_t dst_y) {
    Impl& s = *impl_;
    if (!s.fb.pixels) return;
    s.rasterize_pending();   // copies see everything submitted before them
    IRect sr, dr;
    if (!clip_copy(src, dst_x, dst_y, s.size, sr, dr)) return;
    const size_t bytes = static_cast<size_t>(dr.w) * 4;
    if (dr.y > sr.y) {   // moving down: copy bottom-up
        for (int32_t y = dr.h - 1; y >= 0; --y) std::memmove(s.row(dr.y + y) + dr.x, s.row(sr.y + y) + sr.x, bytes);
    } else {
        for (int32_t y = 0; y < dr.h; ++y) std::memmove(s.row(dr.y + y) + dr.x, s.row(sr.y + y) + sr.x, bytes);
    }
    s.present_damage.push_back(dr);
}

Result<Image> SoftwareRenderer::read_back(IRect rect) {
    Impl& s = *impl_;
    if (!s.presenter || !s.fb.pixels) return make_error(Errc::unavailable, "no frame");
    const IRect r = intersect(rect, IRect{0, 0, s.size.w, s.size.h});
    Image img;
    img.order = s.presenter->order();
    if (r.empty()) return img;
    img.w = r.w;
    img.h = r.h;
    img.pixels.resize(static_cast<size_t>(r.w) * static_cast<size_t>(r.h));
    for (int32_t y = 0; y < r.h; ++y)
        std::memcpy(&img.pixels[static_cast<size_t>(y) * static_cast<size_t>(r.w)], s.row(r.y + y) + r.x,
                    static_cast<size_t>(r.w) * 4);
    return img;
}

}  // namespace kt::render::sw
