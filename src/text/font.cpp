#include "text/font.h"

#include "text/face.h"
#include "text/font_source.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace kt::text {

PointScale native_point_scale() {
#if defined(__APPLE__)
    return PointScale::dpi72;
#else
    return PointScale::dpi96;
#endif
}

float points_to_pixels(float pt, float scale, PointScale ps) {
    return ps == PointScale::dpi96 ? pt * 96.0f / 72.0f * scale : pt * scale;
}

// ---------------------------------------------------------------- FontSet

FontSet::~FontSet() = default;

FaceId FontSet::primary(Style s) const { return styles_[static_cast<size_t>(s) & 3]; }

detail::Face* FontSet::face(FaceId id) const { return system_->face(id); }

FaceId FontSet::face_for(std::u32string_view cluster, Style style, bool emoji_presentation) {
    const size_t si = static_cast<size_t>(style) & 3;
    if (cluster.empty()) return styles_[si];
    const char32_t cp = cluster.front();
    const bool emoji_first = emoji_presentation && emoji_ != kNoFace;
    if (!emoji_first && cp < 4096 && !primary_covered_[si].empty() &&
        (primary_covered_[si][cp >> 6] >> (cp & 63)) & 1u)
        return styles_[si];

    const uint64_t key = (uint64_t{si} << 32) | (uint64_t{emoji_first} << 40) | cp;
    if (auto it = cp_cache_.find(key); it != cp_cache_.end()) return it->second;

    auto has = [&](FaceId id) {
        const detail::Face* f = id == kNoFace ? nullptr : face(id);
        return f && f->glyph_index(cp) != 0;
    };
    FaceId found = kTofuFace;
    if (emoji_first && has(emoji_)) found = emoji_;
    else if (has(styles_[si])) found = styles_[si];
    else {
        for (FaceId id : fallback_) {
            if (has(id)) {
                found = id;
                break;
            }
        }
        if (found == kTofuFace && !emoji_first && has(emoji_)) found = emoji_;
        if (found == kTofuFace) found = system_->system_fallback(*this, cp, style);
    }
    if (cp_cache_.size() >= 65536) cp_cache_.clear();   // bounded
    cp_cache_.emplace(key, found);
    return found;
}

// ---------------------------------------------------------------- FontSystem

struct FontSystem::Impl {
    FT_Library lib = nullptr;
    std::vector<std::pair<std::string, std::weak_ptr<const std::vector<uint8_t>>>> blobs;
    std::vector<std::unique_ptr<detail::Face>> faces;   // index = FaceId; null after release
    std::vector<std::unique_ptr<FontSet>> sets;
    GlyphAtlas atlas;
    uint32_t next_set_id = 1;
    IFontSource* source = nullptr;
    std::string lang;

    ~Impl() {
        sets.clear();
        faces.clear();
        if (lib) FT_Done_FreeType(lib);
    }
};

FontSystem::FontSystem() : impl_(std::make_unique<Impl>()) {}

FontSystem::~FontSystem() = default;

Result<std::unique_ptr<FontSystem>> FontSystem::create() {
    std::unique_ptr<FontSystem> fs(new FontSystem());
    // One FT_Library per rasterizing thread; this one belongs to the UI thread.
    // FT_Init_FreeType = FT_New_Library + FT_Add_Default_Modules with the default allocator.
    FT_Error err = FT_Init_FreeType(&fs->impl_->lib);
    if (err) return make_error(Errc::unavailable, "FT_Init_FreeType failed", err);
    return fs;
}

FT_LibraryRec_* FontSystem::ft_library() const { return impl_->lib; }

GlyphAtlas& FontSystem::atlas() { return impl_->atlas; }

void FontSystem::set_memory_budget(size_t bytes) { impl_->atlas.set_memory_budget(bytes); }

void FontSystem::set_font_source(IFontSource* source, std::string lang) {
    impl_->source = source;
    impl_->lang = std::move(lang);
}

namespace {

// font.thicken 1.0 grows every stem by half a DIP; FreeType's strength is that growth in 26.6.
int32_t thicken_26_6(const FontSpec& spec, float scale) {
    const float t = std::clamp(spec.thicken, 0.0f, 2.0f);
    return static_cast<int32_t>(std::lround(t * scale * 32.0f));
}

}  // namespace

FaceId FontSystem::system_fallback(FontSet& set, char32_t cp, Style style) {
    // Bounded per set: each query may cost milliseconds (fontconfig sort, CoreText, DirectWrite).
    constexpr uint32_t kMaxQueries = 4096;
    constexpr size_t kMaxFaces = 64;
    if (!impl_->source || set.system_queries_ >= kMaxQueries) return kTofuFace;
    ++set.system_queries_;
    const auto si = static_cast<uint8_t>(style) & 3u;
    const uint8_t synth = static_cast<uint8_t>(((si & 1u) ? detail::kSynthBold : 0) | ((si & 2u) ? detail::kSynthItalic : 0));
    for (const FontFileRef& ref : impl_->source->fallback_for(cp, FontQuery{set.family_, style, 0}, impl_->lang)) {
        if (ref.path.empty() && !ref.memory) continue;
        std::string key = ref.path.empty() ? "mem:" + std::to_string(reinterpret_cast<uintptr_t>(ref.memory.get())) : ref.path;
        key += '#' + std::to_string(ref.face_index) + '#' + std::to_string(synth);
        FaceId id = kNoFace;
        if (auto it = set.system_faces_.find(key); it != set.system_faces_.end()) {
            id = it->second;
        } else {
            if (set.system_faces_.size() >= kMaxFaces) continue;
            // Faces that fail to open (bitmap-only color fonts, corrupt files) are remembered.
            if (auto opened = open_face(ref, set.size_px_, synth, set.spec_.hinting)) {
                id = *opened;
                set.owned_.push_back(id);
                impl_->faces[id]->thicken_26_6 = thicken_26_6(set.spec_, set.scale_);
            }
            set.system_faces_.emplace(std::move(key), id);
        }
        const detail::Face* f = id == kNoFace ? nullptr : face(id);
        if (f && f->glyph_index(cp) != 0) return id;
    }
    return kTofuFace;
}

detail::Face* FontSystem::face(FaceId id) const {
    return id < impl_->faces.size() ? impl_->faces[id].get() : nullptr;
}

namespace {

Result<std::shared_ptr<const std::vector<uint8_t>>> read_file(const std::string& path) {
    // Font files are read whole for now; memory mapping goes through platform_core later.
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return make_error(Errc::unavailable, "cannot open font file");
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > (std::streamoff{256} << 20)) return make_error(Errc::corrupt, "bad font file size");
    auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(data->data()), size)) return make_error(Errc::os_error, "cannot read font file");
    return std::shared_ptr<const std::vector<uint8_t>>(std::move(data));
}

}  // namespace

Result<FaceId> FontSystem::open_face(const FontFileRef& ref, float size_px, uint8_t synth, Hinting hinting) {
    std::shared_ptr<const std::vector<uint8_t>> blob = ref.memory;
    if (!blob) {
        if (ref.path.empty()) return make_error(Errc::invalid_argument, "font file has no path and no data");
        for (auto& [path, weak] : impl_->blobs) {
            if (path == ref.path) {
                blob = weak.lock();
                break;
            }
        }
        if (!blob) {
            auto data = read_file(ref.path);
            if (!data) return Unexpected<Error>(data.error());
            blob = *data;
            std::erase_if(impl_->blobs, [](const auto& e) { return e.second.expired(); });
            impl_->blobs.emplace_back(ref.path, blob);
        }
    }
    if (impl_->faces.size() >= kTofuFace) return make_error(Errc::busy, "face table full");
    auto face = detail::Face::open(impl_->lib, blob, ref.face_index, size_px, synth, hinting);
    if (!face) return Unexpected<Error>(face.error());
    const auto id = static_cast<FaceId>(impl_->faces.size());
    (*face)->id = id;
    impl_->faces.push_back(std::move(*face));
    return id;
}

Result<FontSet*> FontSystem::font_set(const FontSpec& spec, const FontFiles& files, float scale, RenderMode mode,
                                      PointScale ps) {
    const float size_px = points_to_pixels(spec.size_pt, scale, ps);
    std::unique_ptr<FontSet> set(new FontSet());
    set->system_ = this;
    set->spec_ = spec;
    set->scale_ = scale;
    set->size_px_ = size_px;
    set->mode_ = mode;
    set->family_ = files.family;

    auto open = [&](const FontFileRef& ref, uint8_t synth) -> Result<FaceId> {
        auto id = open_face(ref, size_px, synth, spec.hinting);
        if (id) {
            set->owned_.push_back(*id);
            impl_->faces[*id]->thicken_26_6 = thicken_26_6(spec, scale);
        }
        return id;
    };
    auto fail = [&](Error e) -> Result<FontSet*> {
        for (FaceId id : set->owned_) impl_->faces[id].reset();
        return Unexpected<Error>(std::move(e));
    };

    auto regular = open(files.regular, 0);
    if (!regular) return fail(regular.error());
    set->styles_[0] = *regular;

    // Real style faces when given, else synthesized from the closest one.
    auto style_face = [&](const std::optional<FontFileRef>& real, const FontFileRef& base, uint8_t synth) -> Result<FaceId> {
        if (real) {
            if (auto id = open(*real, 0)) return id;
        }
        return open(base, synth);
    };
    auto bold = style_face(files.bold, files.regular, detail::kSynthBold);
    if (!bold) return fail(bold.error());
    set->styles_[1] = *bold;
    auto italic = style_face(files.italic, files.regular, detail::kSynthItalic);
    if (!italic) return fail(italic.error());
    set->styles_[2] = *italic;
    Result<FaceId> bold_italic = make_error(Errc::unavailable, "");
    if (files.bold_italic) bold_italic = open(*files.bold_italic, 0);
    if (!bold_italic && files.italic) bold_italic = open(*files.italic, detail::kSynthBold);
    if (!bold_italic && files.bold) bold_italic = open(*files.bold, detail::kSynthItalic);
    if (!bold_italic) bold_italic = open(files.regular, detail::kSynthBold | detail::kSynthItalic);
    if (!bold_italic) return fail(bold_italic.error());
    set->styles_[3] = *bold_italic;

    // Fallback faces that fail to open are skipped (corrupt font file).
    for (const FontFileRef& ref : files.fallback) {
        if (auto id = open(ref, 0)) set->fallback_.push_back(*id);
    }
    if (files.emoji) {
        if (auto id = open(*files.emoji, 0)) set->emoji_ = *id;
    }

    set->metrics_ = detail::compute_cell_metrics(*impl_->faces[set->styles_[0]], spec.line_height, spec.cell_width);

    for (size_t s = 0; s < 4; ++s) {
        const detail::Face* f = impl_->faces[set->styles_[s]].get();
        auto& bits = set->primary_covered_[s];
        bits.assign(4096 / 64, 0);
        for (char32_t cp = 0; cp < 4096; ++cp) {
            if (f->glyph_index(cp) != 0) bits[cp >> 6] |= uint64_t{1} << (cp & 63);
        }
    }

    set->id_ = impl_->next_set_id++;
    FontSet* raw = set.get();
    impl_->sets.push_back(std::move(set));
    return raw;
}

void FontSystem::release(FontSet* set) {
    auto it = std::find_if(impl_->sets.begin(), impl_->sets.end(), [&](const auto& p) { return p.get() == set; });
    if (it == impl_->sets.end()) return;
    for (FaceId id : (*it)->owned_) impl_->faces[id].reset();
    impl_->sets.erase(it);
}

}  // namespace kt::text
