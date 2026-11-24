#include "text/face.h"

#include <algorithm>
#include <cmath>

#include FT_ADVANCES_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>

namespace kt::text::detail {

namespace {

// font units -> 26.6 pixels, rounded, integer only (deterministic on every host).
int32_t units_to_26_6(int64_t units, uint32_t size_26_6, uint32_t upem) {
    const int64_t num = units * int64_t{size_26_6};
    const int64_t half = upem / 2;
    return static_cast<int32_t>(num >= 0 ? (num + half) / upem : -((-num + half) / upem));
}

int32_t round_26_6(int32_t v) { return v >= 0 ? (v + 32) >> 6 : -((-v + 32) >> 6); }

int32_t ceil_26_6(int32_t v) { return v >= 0 ? (v + 63) >> 6 : -((-v) >> 6); }

}  // namespace

Result<std::unique_ptr<Face>> Face::open(FT_Library lib, Blob data, uint32_t index, float size_px,
                                         uint8_t synth, Hinting hinting) {
    if (!data || data->empty()) return make_error(Errc::invalid_argument, "empty font data");
    if (!(size_px >= 1.0f && size_px <= 1024.0f)) return make_error(Errc::invalid_argument, "font size out of range");

    std::unique_ptr<Face> f(new Face());
    f->data_ = std::move(data);
    FT_Error err = FT_New_Memory_Face(lib, f->data_->data(), static_cast<FT_Long>(f->data_->size()),
                                      static_cast<FT_Long>(index), &f->ft_);
    if (err) return make_error(Errc::corrupt, "unsupported or corrupt font file", err);
    if (!FT_IS_SCALABLE(f->ft_)) return make_error(Errc::not_supported, "bitmap-only font");
    if (FT_Select_Charmap(f->ft_, FT_ENCODING_UNICODE))
        return make_error(Errc::not_supported, "font has no Unicode cmap");

    f->size_px_ = size_px;
    f->size_26_6_ = static_cast<uint32_t>(std::lround(size_px * 64.0f));
    err = FT_Set_Char_Size(f->ft_, 0, static_cast<FT_F26Dot6>(f->size_26_6_), 72, 72);
    if (err) return make_error(Errc::not_supported, "cannot set font size", err);
    f->synth_ = synth;
    f->hinting_ = hinting;

    // HarfBuzz font over the same FT_Face (shares the memory blob and the size).
    f->hb_ = hb_ft_font_create_referenced(f->ft_);
    if (!f->hb_) return make_error(Errc::unavailable, "hb_ft_font_create failed");
    hb_ft_font_set_load_flags(f->hb_, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);

    for (char32_t c = 0x20; c <= 0x7E; ++c) f->ascii_[c - 0x20] = FT_Get_Char_Index(f->ft_, c);

    // Vertical metrics: OS/2 typo metrics when USE_TYPO_METRICS (fsSelection bit 7), else hhea.
    const uint32_t upem = f->ft_->units_per_EM ? f->ft_->units_per_EM : 1000;
    const auto s26 = f->size_26_6_;
    auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(f->ft_, FT_SFNT_OS2));
    auto* hhea = static_cast<TT_HoriHeader*>(FT_Get_Sfnt_Table(f->ft_, FT_SFNT_HHEA));
    auto* post = static_cast<TT_Postscript*>(FT_Get_Sfnt_Table(f->ft_, FT_SFNT_POST));
    const bool os2_ok = os2 && os2->version != 0xFFFFu;
    int64_t asc = f->ft_->ascender, desc = f->ft_->descender, gap = 0;
    if (os2_ok && (os2->fsSelection & (1u << 7))) {
        asc = os2->sTypoAscender;
        desc = os2->sTypoDescender;
        gap = os2->sTypoLineGap;
    } else if (hhea && (hhea->Ascender != 0 || hhea->Descender != 0)) {
        asc = hhea->Ascender;
        desc = hhea->Descender;
        gap = hhea->Line_Gap;
    } else if (os2_ok) {
        asc = os2->sTypoAscender;
        desc = os2->sTypoDescender;
        gap = os2->sTypoLineGap;
    }
    VMetrics& vm = f->vm_;
    vm.ascender = units_to_26_6(asc, s26, upem);
    vm.descender = units_to_26_6(desc, s26, upem);
    vm.line_gap = units_to_26_6(std::max<int64_t>(gap, 0), s26, upem);
    const int64_t ul_thick = post && post->underlineThickness > 0 ? int64_t{post->underlineThickness} : static_cast<int64_t>(upem) / 20;
    const int64_t ul_pos = post && post->underlinePosition != 0 ? post->underlinePosition : -int64_t{upem} / 10;
    vm.underline_pos = units_to_26_6(ul_pos, s26, upem);
    vm.underline_thickness = units_to_26_6(ul_thick, s26, upem);
    int64_t x_height = os2_ok && os2->version >= 2 && os2->sxHeight > 0 ? os2->sxHeight : (asc * 1) / 2;
    vm.x_height = units_to_26_6(x_height, s26, upem);
    if (os2_ok && os2->yStrikeoutSize > 0) {
        vm.strikeout_pos = units_to_26_6(os2->yStrikeoutPosition, s26, upem);
        vm.strikeout_thickness = units_to_26_6(os2->yStrikeoutSize, s26, upem);
    } else {
        vm.strikeout_thickness = vm.underline_thickness;
        vm.strikeout_pos = vm.x_height / 2 + vm.strikeout_thickness / 2;
    }
    return f;
}

Face::~Face() {
    if (hb_) hb_font_destroy(hb_);   // drops its reference on ft_
    if (ft_) FT_Done_Face(ft_);
}

uint32_t Face::glyph_index(char32_t cp) const {
    if (cp >= 0x20 && cp <= 0x7E) return ascii_[cp - 0x20];
    return FT_Get_Char_Index(ft_, cp);
}

int32_t Face::advance_26_6(uint32_t gid) const {
    FT_Fixed adv = 0;
    if (FT_Get_Advance(ft_, gid, FT_LOAD_NO_SCALE, &adv)) return 0;
    const uint32_t upem = ft_->units_per_EM ? ft_->units_per_EM : 1000;
    return units_to_26_6(adv, size_26_6_, upem);
}

int32_t Face::max_ascii_advance_26_6() const {
    int32_t m = 0;
    for (uint32_t gid : ascii_) {
        if (gid) m = std::max(m, advance_26_6(gid));
    }
    return m;
}

CellMetrics compute_cell_metrics(const Face& f, float line_height, float cell_width) {
    const VMetrics& vm = f.vmetrics();
    CellMetrics m;
    // Cell width: max ASCII advance, rounded up, times font.cell_width.
    const double cw = std::clamp(static_cast<double>(cell_width), 0.5, 2.0);
    const int32_t adv = f.max_ascii_advance_26_6();
    m.cell_w = std::max(1, cw == 1.0 ? ceil_26_6(adv) : static_cast<int32_t>(std::ceil(adv * cw / 64.0 - 1e-3)));

    // Cell height: ceil((asc - desc + gap) * line_height); extra height split above and below.
    const int32_t natural = vm.ascender - vm.descender + vm.line_gap;
    const double lh = std::clamp(static_cast<double>(line_height), 0.5, 4.0);
    m.cell_h = std::max(1, static_cast<int32_t>(std::ceil(natural * lh / 64.0 - 1e-3)));
    const int32_t top_padding = (m.cell_h * 64 - (vm.ascender - vm.descender)) / 2;
    m.baseline = std::clamp(round_26_6(top_padding + vm.ascender), 1, m.cell_h);

    m.underline_thickness = std::max(1, round_26_6(vm.underline_thickness));
    m.underline_pos = std::clamp(m.baseline - round_26_6(vm.underline_pos), 0, m.cell_h - m.underline_thickness);
    m.strikeout_thickness = std::max(1, round_26_6(vm.strikeout_thickness));
    m.strikeout_pos = std::clamp(m.baseline - round_26_6(vm.strikeout_pos), 0, m.cell_h - m.strikeout_thickness);

    // Box line widths: light = max(1, round(cell_w / 8)) with 3 * light <= cell_w; heavy = 2 * light,
    // or light + 1 when 2 * light exceeds the double-line budget of cell_w / 3.
    m.box_light = std::max(1, static_cast<int32_t>(std::lround(m.cell_w / 8.0)));
    while (m.box_light > 1 && 3 * m.box_light > m.cell_w) --m.box_light;
    m.box_heavy = 2 * m.box_light;
    if (3 * m.box_heavy > m.cell_w + m.box_light) m.box_heavy = m.box_light + 1;
    return m;
}

}  // namespace kt::text::detail
