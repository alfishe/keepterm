// The terminal view: turns VT frames into draw lists
// and pointer / key input into PTY bytes.
//
// UI thread only. The owner acquires a frame from the FramePublisher, calls build(), releases the
// frame and submits the draw list with the returned damage. Without a new frame, redraw() builds
// from the view's copy of the last one.
//
// Per viewport row the view caches its quads; build() rebuilds the rows that the frame marks as
// damaged, plus the rows the cursor left or entered, and re-emits the rest from the cache.
// Everything else (scroll offset, focus, theme, highlights, size, atlas eviction) invalidates all
// rows. The draw list is always complete; `damage` says which pixels changed.
//
// Input structs mirror the platform input events (physical Key values, the platform modifier
// bits, pointer positions), with positions already in physical pixels relative to the view: the
// adapter from platform::KeyEvent / MouseEvent / ScrollEvent multiplies DIPs by the scale.
//
// Not here yet: selection, search and link logic (only the highlight hook below),
// DECSCNM reverse video (not in the frame yet), the palette of the frame (OSC 4/10/11: the owner
// merges Terminal::effective_colors() into the theme, src/theme/apply.h), SGR blink (drawn
// static; the cursor blinks through set_cursor_blink_hidden), scroll_delta reuse, IME preedit.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "render/draw_list.h"
#include "text/font.h"
#include "text/terminal_shaper.h"
#include "view/terminal/atlas_textures.h"
#include "view/terminal/theme.h"
#include "vt/frame.h"
#include "vt/input.h"

namespace kt::vt {
class Terminal;
}

namespace kt::view {

// ---------------------------------------------------------------- input (mirrors the platform layer)

// platform::Mods bits.
enum PlatformMods : uint16_t {
    kPmShift = 1u << 0,
    kPmCtrl = 1u << 1,
    kPmAlt = 1u << 2,
    kPmSuper = 1u << 3,
    kPmAltGr = 1u << 4,
    kPmCaps = 1u << 5,
    kPmNum = 1u << 6,
};
uint8_t to_vt_mods(uint16_t platform_mods);   // xterm order for the encoder

struct KeyInput {                  // platform::KeyEvent
    vt::Key key = vt::Key::unknown;   // same values as platform::Key
    char32_t logical = 0;
    uint32_t native_code = 0;
    uint16_t mods = 0;                // PlatformMods
    vt::KeyAction action = vt::KeyAction::Press;
    std::u8string text;
    bool composing = false;
};

enum class PointerButton : uint8_t { none, left, middle, right, back, forward };   // platform::MouseButton

struct PointerInput {              // platform::MouseEvent, position in physical px from the view origin
    enum class Type : uint8_t { move, press, release, enter, leave } type = Type::move;
    PointerButton button = PointerButton::none;
    uint8_t click_count = 0;
    uint16_t mods = 0;
    float x = 0, y = 0;
};

struct WheelInput {                // platform::ScrollEvent (dy > 0 = away from the user = up)
    float dx = 0, dy = 0;          // lines when !precise, physical px when precise
    bool precise = false;
    uint16_t mods = 0;
    float x = 0, y = 0;
};

enum class InputResult : uint8_t {
    sent,        // bytes were appended for the PTY
    selection,   // not reported: the event belongs to selection (Shift bypass, no tracking)
    scrolled,    // the wheel moved the scrollback viewport
    ignored,
};

// ---------------------------------------------------------------- geometry and options

struct CellHit {
    int32_t col = 0, row = 0;   // clamped to the grid
    bool inside = false;        // the point was inside the grid (not in the padding)
    uint16_t px = 0, py = 0;    // pixels inside the grid, clamped (mode 1016)
};

struct ViewOptions {
    Theme theme = Theme::keepterm_dark();
    int32_t pad_x = 0, pad_y = 0;          // physical px between the bounds and the grid
    vt::EncoderOptions encoder;
    float dim_factor = 0.6f;               // SGR 2: fg mixed toward bg by 1 - dim_factor
    int32_t wheel_lines = 3;               // scrollback rows per wheel notch
    // Smooth scrolling (animations.smooth_scrolling): the viewport moves by pixels, wheel notches
    // and pages animate. The owner turns it on only where frames are cheap (a GPU backend, not a
    // remote session, full motion); off, the viewport moves by whole rows at once.
    bool smooth_scroll = false;
    bool snap_scroll = true;               // animations.snap_scroll_to_row: settle on a whole row
};

// Selection drawing hook: cells [col0, col1) of an absolute line get these colors. Selection
// and search logic produce the list later.
struct Highlight {
    uint64_t abs_line = 0;
    uint16_t col0 = 0, col1 = 0;
    uint32_t background = 0;                 // 0xRRGGBB
    std::optional<uint32_t> foreground;      // empty: keep the cell's foreground
};

// Rows above the screen for a scrolled-back viewport.
class ScrollbackSource {
public:
    virtual ~ScrollbackSource() = default;
    virtual uint64_t first_abs_line() const = 0;   // oldest row available
    // Cells of an absolute line (may be shorter than the grid: trailing blanks trimmed).
    virtual std::span<const vt::Cell> line(uint64_t abs_line, uint16_t& flags) const = 0;
};

// Reads a Terminal's scrollback directly: only valid on the thread that feeds the terminal
// (headless rendering and tests). The GUI moves to a scrollback view later.
class TerminalScrollback final : public ScrollbackSource {
public:
    explicit TerminalScrollback(const vt::Terminal& t) : t_(t) {}
    uint64_t first_abs_line() const override;
    std::span<const vt::Cell> line(uint64_t abs_line, uint16_t& flags) const override;

private:
    const vt::Terminal& t_;
};

struct BuildResult {
    std::vector<render::IRect> damage;   // changed pixels, disjoint rectangles in target coordinates
    bool full = false;                   // everything changed (damage holds the bounds)
    bool incomplete = false;             // the rasterization budget ran out: build again next frame
    uint32_t rows_rebuilt = 0;
};

// ---------------------------------------------------------------- the view

class TerminalView {
public:
    TerminalView(text::FontSet&, AtlasTextures&, ViewOptions = {});
    ~TerminalView();
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    // Placement in the render target, physical px. The grid starts at bounds + padding.
    void set_bounds(render::IRect bounds);
    render::IRect bounds() const { return bounds_; }
    // The grid that fits the bounds (for Terminal::resize and the PTY size).
    vt::GridSize grid_for_bounds() const;
    const text::CellMetrics& metrics() const;
    // Switches to another font set (font size or DPI change) and rebuilds everything on the
    // next build() or redraw(). The kept frame stays, so redraw() works right away.
    void set_font_set(text::FontSet&);
    void set_padding(int32_t pad_x, int32_t pad_y);   // physical px

    void set_theme(const Theme&);
    const ViewOptions& options() const { return opts_; }
    void set_focused(bool);
    bool focused() const { return focused_; }
    // Cursor blink phase, driven by the owner's timer: true hides a cursor whose frame state
    // blinks (DECSCUSR odd styles, mode 12, terminal.cursor_blink). Steady cursors stay.
    void set_cursor_blink_hidden(bool hidden);
    void set_highlights(std::vector<Highlight>);

    // Scrollback viewport: rows above the live screen (0 = live). The viewport stays anchored to
    // its absolute line while output arrives; clamped to what the scrollback holds at build().
    void scroll_to(uint32_t rows_up);
    void scroll_by(int32_t rows);   // positive = toward older output
    uint32_t scroll_offset() const { return offset_; }

    // Smooth scrolling (ViewOptions::smooth_scroll). The viewport can stand between rows: it is
    // scroll_offset() rows plus scroll_pixel() px up, the row above the top drawn in the gap.
    // scroll_to() and scroll_by() jump to a whole row and end any animation.
    void set_smooth_scroll(bool on, bool snap);
    bool smooth_scroll() const { return opts_.smooth_scroll && !degraded_; }
    int32_t scroll_pixel() const { return pixel_; }
    bool scrolled_back() const { return offset_ > 0 || pixel_ > 0; }
    void scroll_by_px(int32_t px);              // positive = toward older output; no animation
    void scroll_by_animated(int32_t rows);      // a page or a notch: animates when smooth, else jumps
    // Advances the scroll animation and the snap to a row to `now`. True while frames are still
    // needed. Frames that come too slowly during an animation (the machine cannot keep up) turn
    // smooth scrolling off for this view: it falls back to whole rows.
    bool tick(std::chrono::steady_clock::time_point now);
    bool scroll_animating() const { return anim_total_ != 0 || snap_wait_; }

    // Builds the draw list for a frame. `scrollback` is needed only when scrolled back. The view
    // keeps a copy of the frame's cells, so the owner may release the frame right after.
    void build(const vt::Frame&, render::DrawList& out, BuildResult& result, const ScrollbackSource* scrollback = nullptr);
    // Builds again from the kept copy when there is no new frame but the view changed (focus,
    // theme, highlights, scroll offset, bounds). False before the first build().
    bool redraw(render::DrawList& out, BuildResult& result, const ScrollbackSource* scrollback = nullptr);
    // Forces a full rebuild on the next build() (font or DPI change, backend switch).
    void invalidate();

    // Geometry.
    CellHit hit(float x, float y) const;
    render::IRect cell_rect(int32_t col, int32_t row) const;   // target coordinates
    render::IRect row_rect(int32_t row) const;                 // the full row incl. padding

    // Input. `modes` are the frame's InputModes. Bytes are appended to `out`.
    InputResult key(const KeyInput&, const vt::InputModes&, std::string& out);
    InputResult pointer(const PointerInput&, const vt::InputModes&, std::string& out);
    InputResult wheel(const WheelInput&, const vt::InputModes&, std::string& out);
    void paste(std::u8string_view text, const vt::InputModes&, std::string& out);
    // Sets the focus state and reports it (mode 1004).
    void focus(bool focused, const vt::InputModes&, std::string& out);

private:
    struct GlyphQuad {
        render::Quad q;
        uint16_t page;
    };
    struct RowCache {
        std::vector<render::Quad> bg;
        std::vector<GlyphQuad> glyphs;
        std::vector<render::Quad> deco;
        bool valid = false;
    };
    struct CellLook;

    struct Retained {
        vt::Frame frame;
        std::vector<vt::Cell> cells;
        std::vector<vt::FrameRow> rows;
        bool valid = false;
    };
    void retain(const vt::Frame&);
    void build_frame(const vt::Frame&, render::DrawList& out, BuildResult&, const ScrollbackSource*);
    void build_row(RowCache& rc, int32_t r, std::span<const vt::Cell> cells, uint16_t flags, uint64_t abs_line, const vt::Frame&,
                   bool cursor_row, BuildResult&);
    void emit(render::DrawList& out);
    std::optional<text::AtlasRef> curly_strip();
    void add_damage_rows(const std::vector<uint8_t>& rows, BuildResult&) const;
    int32_t grid_x() const { return bounds_.x + opts_.pad_x; }
    int32_t grid_y() const { return bounds_.y + opts_.pad_y; }

    text::FontSet* fonts_;
    AtlasTextures& tex_;
    std::unique_ptr<text::TerminalShaper> shaper_;
    ViewOptions opts_;
    render::IRect bounds_{};
    bool focused_ = true;
    bool blink_hidden_ = false;
    std::vector<Highlight> highlights_;

    // Viewport.
    uint32_t offset_ = 0;
    int32_t pixel_ = 0;            // 0..cell_h-1 px further up than offset_ (smooth scrolling)
    int32_t last_pixel_ = 0;
    uint32_t last_offset_ = 0;     // offset_ at the last build
    RowCache above_;               // the row above the top, shown while pixel_ > 0
    uint64_t above_abs_ = UINT64_MAX;
    // Scroll animation: anim_total_ px over kScrollAnim, anim_done_ px of it applied so far.
    float anim_total_ = 0, anim_done_ = 0, px_carry_ = 0;
    std::optional<std::chrono::steady_clock::time_point> anim_t0_, last_tick_;
    bool snap_wait_ = false;       // precise scrolling stopped between rows: snap after a pause
    std::chrono::steady_clock::time_point last_precise_{};
    bool precise_seen_ = false;
    float wheel_px_carry_ = 0;
    int slow_frames_ = 0;
    bool degraded_ = false;
    uint64_t anchor_ = 0;          // abs line of the top row while scrolled back
    bool anchored_ = false;

    // State of the last build.
    Retained kept_;
    std::vector<RowCache> rows_;
    vt::GridSize last_size_{0, 0};
    vt::CursorState last_cursor_{};
    bool last_cursor_drawn_ = false;
    uint16_t last_cursor_row_ = 0;
    bool last_focused_ = true;
    uint64_t last_top_abs_ = 0;
    bool invalid_ = true;
    std::vector<render::Quad> cursor_quads_;

    // Pointer state for drags (1002) and releases.
    PointerButton held_ = PointerButton::none;
    float wheel_accum_ = 0;

    // Scratch.
    std::vector<text::CellIn> cells_in_;
    std::u32string cluster_buf_;
    std::vector<text::GlyphOut> glyphs_out_;
};

}  // namespace kt::view
