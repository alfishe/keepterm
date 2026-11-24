#include "ui/text_service.h"

#include <chrono>
#include <cmath>

#include "text/face.h"
#include "text/raster.h"
#include "view/terminal/atlas_textures.h"

namespace kt::ui {

namespace {

constexpr uint32_t kIconFaceBase = 0xFFFFFE00u;   // atlas keys of the built-in icons
constexpr size_t kRunCacheMax = 4096;

std::vector<std::string> ui_candidates() {
#if defined(__APPLE__)
    return {"SF Pro Text", "SF Pro", ".AppleSystemUIFont", "Helvetica Neue", "Helvetica", "Arial"};
#elif defined(_WIN32)
    return {"Segoe UI", "Tahoma", "Arial"};
#else
    return {"Inter", "Cantarell", "Ubuntu", "Noto Sans", "DejaVu Sans", "Liberation Sans", "FreeSans"};
#endif
}

int32_t px_26_6(int32_t v) { return v >= 0 ? (v + 32) >> 6 : -((-v + 32) >> 6); }

}  // namespace

Result<UiFontFiles> resolve_ui_fonts(text::IFontSource& src, const std::string& family, const std::string& mono) {
    UiFontFiles out;
    text::FontSpec ui;
    if (!family.empty()) ui.families.push_back(family);
    for (auto& f : ui_candidates()) ui.families.push_back(std::move(f));
    ui.bold_weight = 600;   // semibold where the family has it
    auto u = text::resolve_font_files(src, ui);
    if (!u) return Unexpected<Error>(u.error());
    out.ui = std::move(*u);
    text::FontSpec m;
    if (!mono.empty()) m.families.push_back(mono);
    auto mf = text::resolve_font_files(src, m);
    if (!mf) return Unexpected<Error>(mf.error());
    out.mono = std::move(*mf);
    return out;
}

struct FontTextService::Impl {
    text::FontSystem& fs;
    UiFontFiles files;
    std::unordered_map<uint64_t, text::FontSet*> sets;   // (size q, mono, scale q)

    struct RunKey {
        std::string s;
        FontRole role;
        float scale;
        bool operator==(const RunKey&) const = default;
    };
    struct RunKeyHash {
        size_t operator()(const RunKey& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.s);
            h ^= std::hash<float>{}(k.role.size) * 31u + (k.role.bold ? 7u : 0u) + (k.role.mono ? 13u : 0u);
            h ^= std::hash<float>{}(k.scale) << 1;
            return h;
        }
    };
    std::unordered_map<RunKey, TextRunPtr, RunKeyHash> runs;
    hb_buffer_t* buf = nullptr;
    bool own_frame = false;

    Impl(text::FontSystem& f, UiFontFiles fl) : fs(f), files(std::move(fl)), buf(hb_buffer_create()) {}
    ~Impl() {
        drop();
        if (buf) hb_buffer_destroy(buf);
    }
    void drop() {
        for (auto& [k, s] : sets) fs.release(s);
        sets.clear();
        runs.clear();
    }

    text::FontSet* set_for(const FontRole& r, float scale) {
        const uint64_t sq = static_cast<uint64_t>(std::lround(r.size * 64));
        const uint64_t cq = static_cast<uint64_t>(std::lround(scale * 64));
        const uint64_t key = sq | (uint64_t{r.mono} << 24) | (cq << 32);
        if (auto it = sets.find(key); it != sets.end()) return it->second;
        text::FontSpec spec;
        spec.families = {r.mono ? files.mono.family : files.ui.family};
        spec.size_pt = r.size;
        spec.bold_weight = 600;
        auto s = fs.font_set(spec, r.mono ? files.mono : files.ui, scale, text::RenderMode::gray, text::PointScale::dpi72);
        text::FontSet* set = s ? *s : nullptr;
        sets[key] = set;
        return set;
    }
};

FontTextService::FontTextService(text::FontSystem& fs, UiFontFiles files)
    : impl_(std::make_unique<Impl>(fs, std::move(files))) {}

FontTextService::~FontTextService() = default;

void FontTextService::set_files(UiFontFiles files) {
    impl_->drop();
    impl_->files = std::move(files);
}

FontMetrics FontTextService::metrics(FontRole role, float scale) {
    text::FontSet* set = impl_->set_for(role, scale);
    if (!set) return {role.size * 0.8f, role.size * 0.2f, role.size * 1.25f};
    const text::detail::Face* f = set->face(set->primary(role.bold ? text::Style::bold : text::Style::regular));
    if (!f) return {role.size * 0.8f, role.size * 0.2f, role.size * 1.25f};
    const auto& vm = f->vmetrics();
    const float a = static_cast<float>(px_26_6(vm.ascender)), d = static_cast<float>(px_26_6(-vm.descender));
    const float gap = static_cast<float>(std::max(0, px_26_6(vm.line_gap)));
    return {a / scale, d / scale, (a + d + gap) / scale};
}

TextRunPtr FontTextService::shape(std::string_view s, FontRole role, float scale) {
    Impl& I = *impl_;
    Impl::RunKey key{std::string(s), role, scale};
    if (auto it = I.runs.find(key); it != I.runs.end()) {
        ++stats_.cache_hits;
        return it->second;
    }
    ++stats_.shaped;
    auto run = std::make_shared<TextRun>();
    run->role = role;
    run->scale = scale;
    text::FontSet* set = I.set_for(role, scale);
    const text::Style style = role.bold ? text::Style::bold : text::Style::regular;
    if (!set) {
        run->ascent_px = std::round(role.size * 0.8f * scale);
        run->descent_px = std::round(role.size * 0.2f * scale);
    } else {
        const text::FaceId primary = set->primary(style);
        if (const text::detail::Face* pf = set->face(primary)) {
            run->ascent_px = static_cast<float>(px_26_6(pf->vmetrics().ascender));
            run->descent_px = static_cast<float>(px_26_6(-pf->vmetrics().descender));
        }
        // Segments of consecutive code points with the same face.
        float pen = 0;
        size_t i = 0;
        while (i < s.size()) {
            auto face_of = [&](size_t at) {
                const char32_t cp = utf8_decode(s, at);
                if (cp < 0x20 || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) return primary;   // joiners stay with their base
                std::u32string_view one(&cp, 1);
                text::FaceId f = set->face_for(one, style, false);
                return (f == text::kNoFace || f == text::kTofuFace) ? primary : f;
            };
            const text::FaceId fid = face_of(i);
            size_t j = utf8_next(s, i);
            while (j < s.size() && face_of(j) == fid) j = utf8_next(s, j);
            text::detail::Face* face = set->face(fid);
            if (!face) {
                i = j;
                continue;
            }
            hb_buffer_clear_contents(I.buf);
            hb_buffer_add_utf8(I.buf, s.data(), static_cast<int>(s.size()), static_cast<unsigned>(i), static_cast<int>(j - i));
            hb_buffer_set_direction(I.buf, HB_DIRECTION_LTR);
            hb_buffer_guess_segment_properties(I.buf);
            hb_shape(face->hb(), I.buf, nullptr, 0);
            unsigned n = 0;
            const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(I.buf, &n);
            const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(I.buf, &n);
            for (unsigned g = 0; g < n; ++g) {
                ShapedGlyph sg;
                sg.face = fid;
                sg.gid = info[g].codepoint;
                sg.x = pen + static_cast<float>(pos[g].x_offset) / 64.0f;
                sg.y = static_cast<float>(pos[g].y_offset) / 64.0f;
                sg.cluster = info[g].cluster;
                run->glyphs.push_back(sg);
                pen += static_cast<float>(pos[g].x_advance) / 64.0f;
            }
            i = j;
        }
        run->width_px = std::ceil(pen);
    }
    if (I.runs.size() >= kRunCacheMax) I.runs.clear();
    I.runs.emplace(std::move(key), run);
    return run;
}

void FontTextService::begin_frame() {
    if (tex_ && !tex_->in_frame()) {
        tex_->begin_frame();
        impl_->own_frame = true;
    }
}

void FontTextService::end_frame() {
    if (tex_ && impl_->own_frame) {
        tex_->end_frame();
        impl_->own_frame = false;
    }
}

void FontTextService::draw_text(const TextRun& run, float x_px, float baseline_px, uint32_t color, render::DrawList& dl) {
    if (!tex_ || run.glyphs.empty()) return;
    text::GlyphAtlas& atlas = tex_->atlas();
    text::FontSet* set = impl_->set_for(run.role, run.scale);
    if (!set) return;
    auto key_of = [&](const ShapedGlyph& g, const text::detail::Face& f) {
        text::GlyphKey k;
        k.face = g.face;
        k.glyph = g.gid;
        k.size_26_6 = f.size_26_6();
        k.synth = f.synth();
        k.mode = static_cast<uint8_t>(text::RenderMode::gray);
        return k;
    };
    // Pass 1: make sure every glyph is in the atlas, then upload new pages once.
    bool inserted = false;
    for (const ShapedGlyph& g : run.glyphs) {
        text::detail::Face* f = set->face(g.face);
        if (!f) continue;
        const text::GlyphKey k = key_of(g, *f);
        if (atlas.find(k)) continue;
        const auto t0 = std::chrono::steady_clock::now();
        auto bm = text::detail::rasterize_glyph(*f, g.gid, text::RenderMode::gray, 0);
        (void)atlas.insert(k, bm ? *bm : text::GlyphBitmap{});
        atlas.charge_raster(std::chrono::steady_clock::now() - t0);
        ++stats_.rasterized;
        inserted = true;
    }
    if (inserted) tex_->sync();
    // Pass 2: quads.
    const float x0 = std::round(x_px), y0 = std::round(baseline_px);
    for (const ShapedGlyph& g : run.glyphs) {
        text::detail::Face* f = set->face(g.face);
        if (!f) continue;
        auto ref = atlas.find(key_of(g, *f));
        if (!ref || ref->empty()) continue;
        const render::TextureId t = tex_->texture(ref->page);
        if (!t) continue;
        const float gx = x0 + std::round(g.x) + static_cast<float>(ref->bearing_x);
        const float gy = y0 - std::round(g.y) - static_cast<float>(ref->bearing_y);
        render::Quad q = render::glyph_quad(gx, gy, ref->x, ref->y, ref->w, ref->h, color);
        if (ref->kind == static_cast<uint8_t>(text::GlyphKind::color)) {
            q.kind = static_cast<uint8_t>(render::QuadKind::image);
            q.param = static_cast<float>(color >> 24) / 255.0f;
        }
        dl.quad(q, t);
    }
}

void FontTextService::draw_icon(Icon icon, int size_px, float x_px, float y_px, uint32_t color, render::DrawList& dl) {
    if (!tex_ || icon == Icon::none || size_px <= 0) return;
    text::GlyphAtlas& atlas = tex_->atlas();
    text::GlyphKey k;
    k.face = kIconFaceBase + static_cast<uint32_t>(icon);
    k.glyph = 0;
    k.size_26_6 = static_cast<uint32_t>(size_px);
    auto ref = atlas.find(k);
    if (!ref) {
        text::GlyphBitmap bm;
        bm.w = bm.h = static_cast<uint16_t>(size_px);
        bm.format = text::PageFormat::a8;
        bm.pixels = rasterize_icon(icon, size_px);
        auto r = atlas.insert(k, bm);
        if (!r) return;
        ++stats_.rasterized;
        tex_->sync();
        ref = atlas.find(k);
    }
    if (!ref || ref->empty()) return;
    const render::TextureId t = tex_->texture(ref->page);
    if (!t) return;
    dl.quad(render::glyph_quad(std::round(x_px), std::round(y_px), ref->x, ref->y, ref->w, ref->h, color), t);
}

}  // namespace kt::ui
