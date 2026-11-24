#include "text/terminal_shaper.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>

#include "text/face.h"
#include "text/raster.h"

namespace kt::text {

namespace {

constexpr size_t kRunCacheSize = 8192;
constexpr uint32_t kTofuKeyFace = kProceduralFace + 1;

enum : uint8_t { kBlank = 0, kSpacer = 1, kText = 2, kProcedural = 3, kTofu = 4 };
enum : uint8_t { kGlyphFont = 0, kGlyphProcedural = 1, kGlyphTofu = 2 };

int16_t clamp16(int64_t v) {
    return static_cast<int16_t>(std::clamp<int64_t>(v, std::numeric_limits<int16_t>::min(),
                                                    std::numeric_limits<int16_t>::max()));
}

int32_t round_26_6(int32_t v) { return v >= 0 ? (v + 32) >> 6 : -((-v + 32) >> 6); }

bool is_blank(std::u32string_view c) { return c.empty() || (c.size() == 1 && c[0] == U' '); }

}  // namespace

struct TerminalShaper::Hb {
    hb_buffer_t* buf = nullptr;
    std::vector<hb_feature_t> features;
    ~Hb() {
        if (buf) hb_buffer_destroy(buf);
    }
};

size_t TerminalShaper::RunKeyHash::operator()(const RunKey& k) const noexcept {
    size_t h = std::hash<std::u32string>{}(k.text);
    return h ^ (size_t{k.face} * 0x9E3779B1u) ^ (static_cast<size_t>(k.style) << 20);
}

TerminalShaper::TerminalShaper(FontSet& set) : set_(set), hb_(std::make_unique<Hb>()) {
    hb_->buf = hb_buffer_create();
    std::vector<std::string> feats;
    if (!set.spec().ligatures) feats = {"liga=0", "calt=0", "dlig=0"};
    feats.emplace_back("kern=0");   // kerning never applies to the grid
    for (const std::string& f : set.spec().features) feats.push_back(f);
    for (const std::string& f : feats) {
        hb_feature_t hf;
        if (hb_feature_from_string(f.c_str(), static_cast<int>(f.size()), &hf)) hb_->features.push_back(hf);
    }
    user_gsub_ = !set.spec().features.empty();
}

TerminalShaper::~TerminalShaper() = default;

void TerminalShaper::set_cursor_col(std::optional<uint16_t> col) { cursor_col_ = col; }

void TerminalShaper::clear_cache() {
    cache_.clear();
    lru_.clear();
}

void TerminalShaper::shape_cmap(FaceId face_id, std::span<const CellIn> cells, std::vector<Shaped>& out) {
    const detail::Face* face = set_.face(face_id);
    const int32_t cell_w26 = set_.metrics().cell_w * 64;
    for (size_t k = 0; k < cells.size(); ++k) {
        const CellIn& c = cells[k];
        if (c.width == 0 || is_blank(c.cluster)) continue;
        const uint32_t gid = face->glyph_index(c.cluster.front());
        const int32_t avail = cell_w26 * std::max<int32_t>(1, c.width);
        int32_t adv = face->advance_26_6(gid);
        uint16_t scale_q = 0;
        if (adv > avail + 32) {
            scale_q = static_cast<uint16_t>(std::max<int64_t>(1, int64_t{avail} * 4096 / adv));
            adv = avail;
        }
        out.push_back(Shaped{gid, static_cast<uint16_t>(k), static_cast<uint8_t>(std::max<uint8_t>(1, c.width)),
                             kGlyphFont, scale_q, clamp16(round_26_6((avail - adv) / 2)), 0});
    }
}

void TerminalShaper::shape_run(FaceId face_id, Style style, std::span<const CellIn> cells,
                               std::vector<Shaped>& out) {
    (void)style;
    detail::Face* face = set_.face(face_id);
    const int32_t cell_w26 = set_.metrics().cell_w * 64;

    // Fast path: ligatures off, all printable ASCII in single cells, no user features.
    bool ascii = !set_.spec().ligatures && !user_gsub_;
    for (size_t k = 0; ascii && k < cells.size(); ++k) {
        const CellIn& c = cells[k];
        if (c.width != 1) ascii = false;
        else if (!c.cluster.empty() && (c.cluster.size() != 1 || c.cluster[0] < 0x20 || c.cluster[0] > 0x7E))
            ascii = false;
    }
    if (ascii) {
        ++stats_.fast_path;
        for (size_t k = 0; k < cells.size(); ++k) {
            const CellIn& c = cells[k];
            if (is_blank(c.cluster)) continue;
            const uint32_t gid = face->ascii_glyph(c.cluster[0]);
            const int32_t adv = std::min(face->advance_26_6(gid), cell_w26);
            out.push_back(Shaped{gid, static_cast<uint16_t>(k), 1, kGlyphFont, 0,
                                 clamp16(round_26_6((cell_w26 - adv) / 2)), 0});
        }
        return;
    }

    // HarfBuzz: cluster value = cell index in the run.
    ++stats_.harfbuzz;
    hb_buffer_t* buf = hb_->buf;
    hb_buffer_clear_contents(buf);
    for (size_t k = 0; k < cells.size(); ++k) {
        const CellIn& c = cells[k];
        if (c.width == 0) continue;
        if (c.cluster.empty()) {
            hb_buffer_add(buf, U' ', static_cast<unsigned>(k));
            continue;
        }
        for (char32_t cp : c.cluster) hb_buffer_add(buf, cp, static_cast<unsigned>(k));
    }
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);   // logical order, no bidi
    hb_buffer_guess_segment_properties(buf);
    hb_shape(face->hb(), buf, hb_->features.data(), static_cast<unsigned>(hb_->features.size()));

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);

    // Monotone cluster check (broken fonts fall back to one glyph per cell).
    for (unsigned i = 1; i < n; ++i) {
        if (info[i].cluster < info[i - 1].cluster || info[i].cluster >= cells.size()) {
            ++stats_.cluster_fallbacks;
            shape_cmap(face_id, cells, out);
            return;
        }
    }
    if (n > 0 && info[0].cluster >= cells.size()) {
        ++stats_.cluster_fallbacks;
        shape_cmap(face_id, cells, out);
        return;
    }

    // Snap to the grid: each glyph belongs to the cell of its cluster; advances are
    // ignored except between the glyphs of one cluster (marks), whose HarfBuzz offsets are kept.
    unsigned i = 0;
    while (i < n) {
        const unsigned cl = info[i].cluster;
        unsigned end = i;
        while (end < n && info[end].cluster == cl) ++end;
        const unsigned next_cl = end < n ? info[end].cluster : static_cast<unsigned>(cells.size());
        const unsigned span = std::max(1u, next_cl - cl);
        const int32_t avail = cell_w26 * static_cast<int32_t>(span);
        int32_t adv = face->advance_26_6(info[i].codepoint);
        uint16_t scale_q = 0;
        if (adv > avail + 32) {   // wider than its span: scale down
            scale_q = static_cast<uint16_t>(std::max<int64_t>(1, int64_t{avail} * 4096 / adv));
            adv = avail;
        }
        const int32_t pad = (avail - adv) / 2;
        int32_t pen = 0;
        for (unsigned g = i; g < end; ++g) {
            const int32_t x = pad + pen + pos[g].x_offset;
            pen += pos[g].x_advance;
            if (is_blank(cells[cl].cluster) && end - i == 1) continue;   // spaces draw nothing
            out.push_back(Shaped{info[g].codepoint, static_cast<uint16_t>(cl),
                                 static_cast<uint8_t>(std::min(span, 255u)), kGlyphFont, scale_q,
                                 clamp16(round_26_6(x)), clamp16(round_26_6(pos[g].y_offset))});
        }
        i = end;
    }
}

bool TerminalShaper::emit(FaceId face_id, Style, uint16_t base_col, std::span<const CellIn>,
                          const std::vector<Shaped>& shaped, std::vector<GlyphOut>& out) {
    GlyphAtlas& atlas = set_.system().atlas();
    const CellMetrics& m = set_.metrics();
    detail::Face* face = face_id < kTofuFace ? set_.face(face_id) : nullptr;
    bool complete = true;
    for (const Shaped& s : shaped) {
        GlyphKey key;
        key.mode = static_cast<uint8_t>(set_.mode());
        if (s.kind == kGlyphFont) {
            if (!face) continue;
            key.face = face_id;
            key.glyph = s.glyph;
            key.size_26_6 = face->size_26_6();
            key.synth = face->synth();
            key.scale_q = s.scale_q;
        } else {
            key.face = s.kind == kGlyphProcedural ? kProceduralFace : kTofuKeyFace;
            key.glyph = s.glyph;
            key.variant = detail::procedural_variant(m, s.span);
            key.mode = 0;
        }
        std::optional<AtlasRef> ref = atlas.find(key);
        if (!ref) {
            if (!atlas.raster_budget_left()) {
                complete = false;   // the cell stays blank this frame
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Result<GlyphBitmap> bm = make_error(Errc::unavailable, "");
            if (s.kind == kGlyphFont) bm = detail::rasterize_glyph(*face, s.glyph, set_.mode(), s.scale_q);
            else if (s.kind == kGlyphProcedural) bm = detail::rasterize_procedural(set_.system().ft_library(), s.glyph, m);
            else bm = detail::rasterize_tofu(s.glyph, s.span, m);
            if (!bm) bm = GlyphBitmap{};   // unrenderable glyph: cache as empty
            auto inserted = atlas.insert(key, *bm);
            atlas.charge_raster(std::chrono::steady_clock::now() - t0);
            ++stats_.rasterized;
            if (!inserted) continue;
            ref = *inserted;
        }
        if (ref->empty()) continue;
        GlyphOut g;
        g.col = static_cast<uint16_t>(base_col + s.col);
        g.span = s.span;
        g.ref = *ref;
        g.dx = clamp16(int64_t{s.x_off} + ref->bearing_x);
        g.dy = clamp16(int64_t{m.baseline} - s.y_off - ref->bearing_y);
        out.push_back(g);
    }
    return complete;
}

bool TerminalShaper::shape_row(std::span<const CellIn> row, std::vector<GlyphOut>& out) {
    const bool procedural_on = set_.spec().builtin_box_drawing;
    cls_.resize(row.size());
    for (size_t k = 0; k < row.size(); ++k) {
        const CellIn& c = row[k];
        CellClass& cc = cls_[k];
        cc.face = kNoFace;
        if (c.width == 0) cc.kind = kSpacer;
        else if (is_blank(c.cluster)) cc.kind = kBlank;
        else if (procedural_on && c.cluster.size() == 1 && detail::is_procedural(c.cluster[0])) cc.kind = kProcedural;
        else {
            cc.face = set_.face_for(c.cluster, c.style, c.emoji_presentation);
            cc.kind = cc.face == kTofuFace ? kTofu : kText;
        }
    }

    bool complete = true;
    const bool cursor_break = set_.spec().ligatures && cursor_col_.has_value();
    auto same_run = [&](size_t a, size_t b) {
        return cls_[b].kind == kText && cls_[b].face == cls_[a].face && row[b].style == row[a].style &&
               row[b].attr_break == row[a].attr_break;
    };
    std::vector<Shaped> shaped;
    size_t i = 0;
    while (i < row.size()) {
        const CellClass cc = cls_[i];
        if (cc.kind == kBlank || cc.kind == kSpacer) {
            ++i;
            continue;
        }
        if (cc.kind == kProcedural || cc.kind == kTofu) {
            const uint8_t span = std::max<uint8_t>(1, row[i].width);
            shaped.assign(1, Shaped{static_cast<uint32_t>(row[i].cluster[0]), 0, span,
                                    cc.kind == kProcedural ? kGlyphProcedural : kGlyphTofu, 0, 0, 0});
            complete &= emit(cc.face, row[i].style, static_cast<uint16_t>(i), row.subspan(i, 1), shaped, out);
            ++i;
            continue;
        }
        // Segment a run.
        size_t j = i + 1;
        while (j < row.size()) {
            if (cursor_break && (j == *cursor_col_ || j == size_t{*cursor_col_} + 1)) break;
            if (cls_[j].kind == kSpacer) {
                ++j;
                continue;
            }
            if (same_run(i, j)) {
                ++j;
                continue;
            }
            // A single blank between cells of the same run stays inside it (ligature context).
            if (cls_[j].kind == kBlank && j + 1 < row.size() && same_run(i, j + 1) &&
                !(cursor_break && j + 1 == *cursor_col_)) {
                j += 2;
                continue;
            }
            break;
        }
        const std::span<const CellIn> cells = row.subspan(i, j - i);
        ++stats_.runs;

        RunKey key{cc.face, row[i].style, {}};
        key.text.reserve(cells.size() * 3);
        for (const CellIn& c : cells) {
            if (c.cluster.empty()) key.text.push_back(U' ');
            else key.text.append(c.cluster);
            key.text.push_back(U'\0');
            key.text.push_back(static_cast<char32_t>(c.width));
        }
        const std::vector<Shaped>* glyphs = nullptr;
        if (auto it = cache_.find(key); it != cache_.end()) {
            ++stats_.cache_hits;
            lru_.splice(lru_.begin(), lru_, it->second);
            glyphs = &it->second->second;
        } else {
            shaped.clear();
            shape_run(cc.face, row[i].style, cells, shaped);
            if (cache_.size() >= kRunCacheSize) {
                cache_.erase(lru_.back().first);
                lru_.pop_back();
            }
            lru_.emplace_front(key, shaped);
            cache_.emplace(std::move(key), lru_.begin());
            glyphs = &lru_.front().second;
        }
        complete &= emit(cc.face, row[i].style, static_cast<uint16_t>(i), cells, *glyphs, out);
        i = j;
    }
    return complete;
}

}  // namespace kt::text
