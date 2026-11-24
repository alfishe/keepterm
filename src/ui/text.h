// UI text and icons.
//
// ITextService shapes UTF-8 strings into runs of positioned glyphs and draws runs and icons into
// a render::DrawList. The real service (ui/text_service.h) uses the text module's faces, HarfBuzz
// and the shared glyph atlas; tests use FixedTextService, which measures with fixed advances
// and draws nothing, so layout tests need no font.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/draw_list.h"
#include "ui/geometry.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace kt::ui {

struct ShapedGlyph {
    uint16_t face = 0;      // text::FaceId
    uint32_t gid = 0;       // glyph id in the face
    float x = 0, y = 0;     // px from the run origin on the baseline (y down)
    uint32_t cluster = 0;   // byte offset of the cluster in the string
};

struct TextRun {
    FontRole role;
    float scale = 1;
    float width_px = 0;
    float ascent_px = 0, descent_px = 0;   // descent positive
    std::vector<ShapedGlyph> glyphs;
    float width() const { return width_px / scale; }
    float height() const { return (ascent_px + descent_px) / scale; }
    float ascent() const { return ascent_px / scale; }
};
using TextRunPtr = std::shared_ptr<const TextRun>;

struct FontMetrics {
    float ascent = 0, descent = 0, line = 0;   // DIP
};

class ITextService {
public:
    virtual ~ITextService() = default;
    virtual TextRunPtr shape(std::string_view utf8, FontRole, float scale) = 0;
    virtual FontMetrics metrics(FontRole, float scale) = 0;
    // The longest prefix (or head and tail with `middle`) plus an ellipsis that fits.
    virtual TextRunPtr ellipsize(std::string_view utf8, FontRole, float scale, float max_w, bool middle = false);

    // Drawing (px coordinates of the target). Frames bracket every draw of a window frame.
    virtual void begin_frame() {}
    virtual void end_frame() {}
    virtual void draw_text(const TextRun&, float /*x_px*/, float /*baseline_px*/, uint32_t /*color*/, render::DrawList&) {}
    virtual void draw_icon(Icon, int /*size_px*/, float /*x_px*/, float /*y_px*/, uint32_t /*color*/, render::DrawList&) {}
};

// Fixed advances: `advance` DIP per code point (ASCII and others alike), ascent 0.8 and
// descent 0.2 of the size. For layout tests and benchmarks without fonts.
class FixedTextService final : public ITextService {
public:
    explicit FixedTextService(float advance_per_size = 0.5f) : k_(advance_per_size) {}
    TextRunPtr shape(std::string_view utf8, FontRole, float scale) override;
    FontMetrics metrics(FontRole, float scale) override;
    uint64_t shapes = 0;

private:
    float k_;
};

// UTF-8 helpers.
size_t utf8_next(std::string_view s, size_t i);      // index of the next code point
size_t utf8_prev(std::string_view s, size_t i);      // index of the previous code point
size_t utf8_length(std::string_view s);              // code points
char32_t utf8_decode(std::string_view s, size_t i);
void utf8_append(std::string& out, char32_t cp);

}  // namespace kt::ui
