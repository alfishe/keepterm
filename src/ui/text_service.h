// The real UI text service: the UI font through the text module (FreeType
// faces, HarfBuzz shaping, per-code-point fallback, the shared glyph atlas) and the built-in
// icons rasterized into the same atlas. Drawing needs the window's AtlasTextures (one renderer).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "text/font.h"
#include "text/font_source.h"
#include "ui/text.h"

namespace kt::view {
class AtlasTextures;
}

namespace kt::ui {

struct UiFontFiles {
    text::FontFiles ui;     // proportional UI font (ui.font, else the OS UI font)
    text::FontFiles mono;   // monospace UI font (ui.mono_font, else the terminal font)
};

// Resolves the UI fonts through a font source: `family` (empty: the OS UI font, then common
// sans families), `mono` (empty: the OS monospace). Falls back to the monospace font for the UI
// font when no sans family is installed.
Result<UiFontFiles> resolve_ui_fonts(text::IFontSource&, const std::string& family, const std::string& mono);

class FontTextService final : public ITextService {
public:
    FontTextService(text::FontSystem&, UiFontFiles files);
    ~FontTextService() override;

    // The textures of the renderer that draws this window; null = measure only.
    void set_textures(view::AtlasTextures* t) { tex_ = t; }
    void set_files(UiFontFiles files);   // ui.font changed: drops cached sets and runs

    TextRunPtr shape(std::string_view utf8, FontRole, float scale) override;
    FontMetrics metrics(FontRole, float scale) override;
    void begin_frame() override;
    void end_frame() override;
    void draw_text(const TextRun&, float x_px, float baseline_px, uint32_t color, render::DrawList&) override;
    void draw_icon(Icon, int size_px, float x_px, float y_px, uint32_t color, render::DrawList&) override;

    struct Stats {
        uint64_t shaped = 0, cache_hits = 0, rasterized = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    view::AtlasTextures* tex_ = nullptr;
    Stats stats_;
};

}  // namespace kt::ui
