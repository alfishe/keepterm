// Shaping of terminal rows into cell-aligned glyphs.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "text/atlas.h"
#include "text/font.h"

namespace kt::text {

// Input: one row of the terminal view, already split into cells by the VT module.
struct CellIn {
    std::u32string_view cluster;     // empty = blank cell; wide chars: first cell has it,
    uint8_t  width = 1;              // second cell has width 0 (spacer)
    Style    style = Style::regular;
    uint16_t attr_break = 0;         // changes that must break shaping (font-affecting only)
    bool     emoji_presentation = false;   // from the VT width rules (VS16, default emoji)
};

struct GlyphOut {                    // one textured quad, cell-relative
    uint16_t col;                    // first cell covered
    uint8_t  span;                   // number of cells (1, 2, or N for a ligature)
    AtlasRef ref;                    // page, uv rect, kind (a8, lcd, color), bearing
    int16_t  dx, dy;                 // top-left of the bitmap from the origin of cell `col`, px
};

struct ShaperStats {
    uint64_t runs = 0, cache_hits = 0, fast_path = 0, harfbuzz = 0, rasterized = 0, cluster_fallbacks = 0;
};

class TerminalShaper {
public:
    explicit TerminalShaper(FontSet&);
    ~TerminalShaper();
    TerminalShaper(const TerminalShaper&) = delete;
    TerminalShaper& operator=(const TerminalShaper&) = delete;

    // UI thread. Output is appended; returns false if the frame rasterization budget
    // ran out: the caller draws what it has and requests another frame.
    bool shape_row(std::span<const CellIn> row, std::vector<GlyphOut>& out);
    void set_cursor_col(std::optional<uint16_t>);   // ligature break under the cursor

    const ShaperStats& stats() const { return stats_; }
    void clear_cache();

    // A glyph of the run cache: cell-relative, before atlas lookup.
    struct Shaped {
        uint32_t glyph;      // glyph id, or code point for procedural / tofu
        uint16_t col;        // cell offset in the run
        uint8_t  span;
        uint8_t  kind;       // 0 font glyph, 1 procedural, 2 tofu
        uint16_t scale_q;    // horizontal scale-down in 1/4096 when wider than the span (0 = none)
        int16_t  x_off, y_off;   // px from the cell origin on the baseline (y up): padding + HarfBuzz offsets
    };

private:
    struct RunKey {
        FaceId face;
        Style style;
        std::u32string text;   // clusters joined by U+0000 with widths encoded after each
        bool operator==(const RunKey&) const = default;
    };
    struct RunKeyHash {
        size_t operator()(const RunKey&) const noexcept;
    };
    using Lru = std::list<std::pair<RunKey, std::vector<Shaped>>>;

    void shape_run(FaceId face, Style style, std::span<const CellIn> cells, std::vector<Shaped>& out);
    bool emit(FaceId face, Style style, uint16_t base_col, std::span<const CellIn> cells,
              const std::vector<Shaped>& shaped, std::vector<GlyphOut>& out);
    void shape_cmap(FaceId face, std::span<const CellIn> cells, std::vector<Shaped>& out);

    FontSet& set_;
    std::optional<uint16_t> cursor_col_;
    Lru lru_;
    std::unordered_map<RunKey, Lru::iterator, RunKeyHash> cache_;
    ShaperStats stats_;
    struct Hb;
    std::unique_ptr<Hb> hb_;            // HarfBuzz buffer and features
    bool user_gsub_ = false;
    struct CellClass {
        FaceId face;
        uint8_t kind;   // 0 blank, 1 spacer, 2 text, 3 procedural, 4 tofu
    };
    std::vector<CellClass> cls_;        // scratch
};

}  // namespace kt::text
