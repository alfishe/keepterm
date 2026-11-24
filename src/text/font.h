// Fonts, font sets and cell metrics.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/error.h"
#include "text/atlas.h"

struct FT_LibraryRec_;   // FreeType's FT_Library, opaque outside the module

namespace kt::text {

enum class Style : uint8_t { regular = 0, bold = 1, italic = 2, bold_italic = 3 };

enum class RenderMode : uint8_t { gray, lcd_rgb, lcd_bgr };

enum class Hinting : uint8_t { light, none };

struct FontSpec {
    std::vector<std::string> families;   // user list, first wins; e.g. {"Cascadia Mono", "Consolas"}
    float  size_pt = 12.0f;              // points; pixels per point differ per OS
    float  line_height = 1.0f;           // multiplier of the natural line height
    std::vector<std::string> features;   // "ss01", "zero", "-calt"
    bool   ligatures = false;            // false => "liga=0,calt=0,dlig=0"
    std::vector<std::string> fallback;   // user fallback families before system fallback
    bool   builtin_box_drawing = true;   // font.builtin_box_drawing
    Hinting hinting = Hinting::light;    // font.hinting
    float  cell_width = 1.0f;            // font.cell_width: multiplier of the natural cell width
    uint16_t weight = 400;               // font.weight, CSS scale 100-900: face of regular text
    uint16_t bold_weight = 700;          // font.bold_weight: face of bold text
    float  thicken = 0.0f;               // font.thicken: 1.0 grows every stem by half a DIP
};

struct CellMetrics {                     // physical pixels, integers
    int32_t cell_w = 0, cell_h = 0;
    int32_t baseline = 0;                // from the cell top
    int32_t underline_pos = 0, underline_thickness = 0;   // top edge from the cell top
    int32_t strikeout_pos = 0, strikeout_thickness = 0;   // top edge from the cell top
    int32_t box_light = 0, box_heavy = 0;                 // line widths for procedural glyphs
};

using FaceId = uint16_t;                 // index into the face table, stable for the process
inline constexpr FaceId kNoFace = 0xFFFF;
inline constexpr FaceId kTofuFace = 0xFFFE;   // last resort of every chain

// A font file: a path or a memory blob, plus the face index inside a collection.
// resolve_font_files (text/font_source.h) turns a FontSpec into files through an IFontSource.
struct FontFileRef {
    std::string path;
    std::shared_ptr<const std::vector<uint8_t>> memory;   // used when set, else path
    uint32_t face_index = 0;
};

// Resolved files for one FontSpec: the primary family's styles, the fallback chain, emoji.
// Missing styles are synthesized from the regular face.
struct FontFiles {
    std::string family;                  // resolved primary family (system fallback queries use it)
    FontFileRef regular;
    std::optional<FontFileRef> bold, italic, bold_italic;
    std::vector<FontFileRef> fallback;   // user fallback, then system fallback, in order
    std::optional<FontFileRef> emoji;
};

// Points to pixels: Windows and Linux read points at 96 dpi, macOS at 72.
enum class PointScale : uint8_t { dpi96, dpi72 };
PointScale native_point_scale();
float points_to_pixels(float pt, float scale, PointScale);

class FontSystem;
class IFontSource;
namespace detail {
class Face;
}

class FontSet {
public:
    ~FontSet();
    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    const CellMetrics& metrics() const { return metrics_; }
    float scale() const { return scale_; }
    float size_px() const { return size_px_; }
    RenderMode mode() const { return mode_; }
    const FontSpec& spec() const { return spec_; }
    uint32_t id() const { return id_; }
    FontSystem& system() const { return *system_; }

    FaceId primary(Style) const;
    // Fallback: first face in the chain that has a glyph for the cluster's base code point;
    // kTofuFace when none has.
    FaceId face_for(std::u32string_view cluster, Style, bool emoji_presentation);

    detail::Face* face(FaceId) const;   // internal: shaper and rasterizer

private:
    friend class FontSystem;
    FontSet() = default;

    FontSystem* system_ = nullptr;
    uint32_t id_ = 0;
    FontSpec spec_;
    float scale_ = 1.0f;
    float size_px_ = 0.0f;
    RenderMode mode_ = RenderMode::gray;
    CellMetrics metrics_;
    FaceId styles_[4] = {kNoFace, kNoFace, kNoFace, kNoFace};
    std::vector<FaceId> fallback_;
    FaceId emoji_ = kNoFace;
    std::vector<FaceId> owned_;
    std::vector<uint64_t> primary_covered_[4];          // 4,096-bit bitmaps per style
    std::unordered_map<uint64_t, FaceId> cp_cache_;     // (style, cp) -> face
    std::string family_;                                // resolved primary family
    std::unordered_map<std::string, FaceId> system_faces_;   // "path#index#synth" -> face (kNoFace: failed)
    uint32_t system_queries_ = 0;
};

class FontSystem {
public:
    static Result<std::unique_ptr<FontSystem>> create();
    ~FontSystem();
    FontSystem(const FontSystem&) = delete;
    FontSystem& operator=(const FontSystem&) = delete;

    // System fallback: when no face of a chain has a code point, FontSet asks
    // `source` (not owned, may be null) and opens the faces it returns. `lang` is the
    // font.cjk_lang / OS UI language tag passed to fallback_for.
    void set_font_source(IFontSource* source, std::string lang = {});

    // Builds a font set for a spec at a pixel scale (one per DPI in use). `files` come from
    // resolve_font_files (text/font_source.h) or from the caller (tests, bundled fonts).
    Result<FontSet*> font_set(const FontSpec&, const FontFiles& files, float scale, RenderMode,
                              PointScale = native_point_scale());
    void release(FontSet*);

    GlyphAtlas& atlas();
    void set_memory_budget(size_t bytes);    // default 24 MB for all pages

    detail::Face* face(FaceId) const;        // internal
    FT_LibraryRec_* ft_library() const;      // internal: procedural glyphs

private:
    friend class FontSet;
    struct Impl;
    FontSystem();
    FaceId system_fallback(FontSet&, char32_t cp, Style);
    Result<FaceId> open_face(const FontFileRef&, float size_px, uint8_t synth, Hinting);
    std::unique_ptr<Impl> impl_;
};

}  // namespace kt::text
