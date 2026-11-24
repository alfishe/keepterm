#include "view/terminal/terminal_view.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "vt/terminal.h"
#include "vt/width.h"

namespace kt::view {

namespace {

// Atlas key face of view decorations (curly underline strips). The shaper uses kProceduralFace
// and kProceduralFace + 1 (tofu).
constexpr uint32_t kDecoFace = text::kProceduralFace + 0x20;

float f(int32_t v) { return static_cast<float>(v); }

render::Quad solid(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    return render::solid_quad(f(x), f(y), f(w), f(h), render_color(rgb));
}

text::Style text_style(const vt::Style& s) {
    const bool bold = s.has(vt::kBold), italic = s.has(vt::kItalic);
    if (bold && italic) return text::Style::bold_italic;
    if (bold) return text::Style::bold;
    if (italic) return text::Style::italic;
    return text::Style::regular;
}

}  // namespace

uint8_t to_vt_mods(uint16_t pm) {
    uint8_t m = 0;
    if (pm & kPmShift) m |= vt::kModShift;
    if (pm & kPmAlt) m |= vt::kModAlt;
    if (pm & kPmCtrl) m |= vt::kModCtrl;
    if (pm & kPmSuper) m |= vt::kModSuper;
    if (pm & kPmCaps) m |= vt::kModCapsLock;
    if (pm & kPmNum) m |= vt::kModNumLock;
    return m;
}

uint64_t TerminalScrollback::first_abs_line() const { return t_.scrollback().first_abs_line(); }

std::span<const vt::Cell> TerminalScrollback::line(uint64_t abs_line, uint16_t& flags) const {
    const vt::Scrollback& sb = t_.scrollback();
    flags = 0;
    if (abs_line < sb.first_abs_line() || abs_line >= sb.next_abs_line()) return {};
    const vt::Scrollback::Line& l = sb.line(static_cast<size_t>(abs_line - sb.first_abs_line()));
    flags = l.flags;
    return l.cells;
}

// The resolved look of one cell.
struct TerminalView::CellLook {
    uint32_t fg = 0, bg = 0, ul = 0;
    vt::Underline underline = vt::Underline::None;
    uint16_t attrs = 0;
    bool hidden = false;
};

TerminalView::TerminalView(text::FontSet& fonts, AtlasTextures& tex, ViewOptions opts)
    : fonts_(&fonts), tex_(tex), shaper_(std::make_unique<text::TerminalShaper>(fonts)), opts_(std::move(opts)) {}

TerminalView::~TerminalView() = default;

const text::CellMetrics& TerminalView::metrics() const { return fonts_->metrics(); }

void TerminalView::set_font_set(text::FontSet& fonts) {
    if (&fonts == fonts_) return;
    fonts_ = &fonts;
    shaper_ = std::make_unique<text::TerminalShaper>(fonts);
    invalid_ = true;
}

void TerminalView::set_padding(int32_t pad_x, int32_t pad_y) {
    if (pad_x == opts_.pad_x && pad_y == opts_.pad_y) return;
    opts_.pad_x = pad_x;
    opts_.pad_y = pad_y;
    invalid_ = true;
}

void TerminalView::set_bounds(render::IRect b) {
    if (b == bounds_) return;
    bounds_ = b;
    invalid_ = true;
}

vt::GridSize TerminalView::grid_for_bounds() const {
    const text::CellMetrics& m = metrics();
    vt::GridSize g;
    g.cols = static_cast<uint16_t>(std::clamp((bounds_.w - 2 * opts_.pad_x) / std::max(1, m.cell_w), 1, 4096));
    g.rows = static_cast<uint16_t>(std::clamp((bounds_.h - 2 * opts_.pad_y) / std::max(1, m.cell_h), 1, 4096));
    g.cell_w_px = static_cast<uint16_t>(std::clamp(m.cell_w, 0, 65535));
    g.cell_h_px = static_cast<uint16_t>(std::clamp(m.cell_h, 0, 65535));
    return g;
}

void TerminalView::set_theme(const Theme& t) {
    opts_.theme = t;
    invalid_ = true;
}

void TerminalView::set_focused(bool focused) { focused_ = focused; }

void TerminalView::set_cursor_blink_hidden(bool hidden) { blink_hidden_ = hidden; }

void TerminalView::set_highlights(std::vector<Highlight> h) {
    highlights_ = std::move(h);
    invalid_ = true;
}

void TerminalView::scroll_to(uint32_t rows_up) {
    offset_ = rows_up;
    pixel_ = 0;
    anchored_ = false;   // re-anchored at the next build
    anim_total_ = anim_done_ = px_carry_ = 0;
    anim_t0_.reset();
    snap_wait_ = false;
}

void TerminalView::set_smooth_scroll(bool on, bool snap) {
    opts_.smooth_scroll = on;
    opts_.snap_scroll = snap;
    degraded_ = false;
    slow_frames_ = 0;
    if (!on && (pixel_ > 0 || anim_total_ != 0)) scroll_to(offset_);
}

void TerminalView::scroll_by_px(int32_t px) {
    if (px == 0) return;
    const int64_t h = std::max(1, metrics().cell_h);
    const int64_t total = std::max<int64_t>(0, int64_t{offset_} * h + pixel_ + px);
    offset_ = static_cast<uint32_t>(std::min<int64_t>(total / h, UINT32_MAX));
    pixel_ = static_cast<int32_t>(total % h);
    anchored_ = false;
}

void TerminalView::scroll_by_animated(int32_t rows) {
    if (!smooth_scroll()) {
        scroll_by(rows);
        return;
    }
    const float h = f(std::max(1, metrics().cell_h));
    anim_total_ = (anim_total_ - anim_done_) + static_cast<float>(rows) * h;   // a new notch adds to what is left
    anim_done_ = 0;
    anim_t0_.reset();
    snap_wait_ = false;
}

namespace {
constexpr auto kScrollAnim = std::chrono::milliseconds(120);   // one notch
constexpr auto kSnapAnim = std::chrono::milliseconds(90);
constexpr auto kSnapPause = std::chrono::milliseconds(150);     // no precise delta for this long: snap
constexpr auto kSlowFrame = std::chrono::milliseconds(50);      // under 20 frames a second
constexpr auto kPausedFrame = std::chrono::milliseconds(1000);  // a hidden window, not a slow one
constexpr int kSlowFramesToDegrade = 3;
}  // namespace

bool TerminalView::tick(std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    if (!smooth_scroll()) {
        last_tick_.reset();
        if (pixel_ > 0 || anim_total_ != 0) {
            // Smooth scrolling went away mid-way: land on the row the animation was heading to.
            const float h = f(std::max(1, metrics().cell_h));
            const float rest = anim_total_ - anim_done_ + f(pixel_);
            const auto rows = static_cast<int32_t>(std::lround(rest / h));
            const uint32_t base = offset_;
            scroll_to(static_cast<uint32_t>(std::max<int64_t>(0, int64_t{base} + rows)));
            return true;
        }
        return false;
    }
    if (precise_seen_) {
        precise_seen_ = false;
        last_precise_ = now;
        snap_wait_ = opts_.snap_scroll && anim_total_ == 0;
    }
    const bool busy = anim_total_ != 0 || (snap_wait_ && pixel_ > 0);
    if (!busy) {
        snap_wait_ = false;
        last_tick_.reset();   // slow_frames_ stays: only a fast frame clears it
        return false;
    }
    // A machine that cannot keep up gets whole rows (the user's rule: smooth only when it is smooth).
    if (last_tick_) {
        const auto gap = now - *last_tick_;
        if (gap > kSlowFrame && gap < kPausedFrame) ++slow_frames_;
        else slow_frames_ = 0;
        if (slow_frames_ >= kSlowFramesToDegrade) {
            degraded_ = true;
            return tick(now);
        }
    }
    last_tick_ = now;

    const int32_t h = std::max(1, metrics().cell_h);
    if (anim_total_ != 0) {
        if (!anim_t0_) anim_t0_ = now;
        const auto dur = std::abs(anim_total_) < f(h) ? kSnapAnim : kScrollAnim;
        const float t = std::clamp(duration<float>(now - *anim_t0_) / duration<float>(dur), 0.0f, 1.0f);
        const float e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease-out cubic
        const float target = t >= 1 ? anim_total_ : anim_total_ * e;
        const float delta = target - anim_done_ + px_carry_;
        const auto d = static_cast<int32_t>(t >= 1 ? std::round(delta) : std::trunc(delta));
        px_carry_ = t >= 1 ? 0 : delta - f(d);
        anim_done_ = target;
        scroll_by_px(d);
        if (t >= 1) {
            anim_total_ = anim_done_ = 0;
            anim_t0_.reset();
            // Between rows after the animation (a precise delta came first): settle on a row.
            if (opts_.snap_scroll && pixel_ > 0) anim_total_ = f(pixel_ * 2 < h ? -pixel_ : h - pixel_);
        }
        return true;
    }
    // snap_wait_ with pixel_ > 0
    if (now - last_precise_ >= kSnapPause) {
        snap_wait_ = false;
        anim_total_ = f(pixel_ * 2 < h ? -pixel_ : h - pixel_);
        anim_done_ = 0;
        anim_t0_.reset();
    }
    return true;
}

void TerminalView::scroll_by(int32_t rows) {
    const int64_t v = std::max<int64_t>(0, int64_t{offset_} + rows);
    scroll_to(static_cast<uint32_t>(std::min<int64_t>(v, UINT32_MAX)));
}

void TerminalView::invalidate() { invalid_ = true; }

// ---------------------------------------------------------------- geometry

CellHit TerminalView::hit(float x, float y) const {
    const text::CellMetrics& m = metrics();
    const vt::GridSize g = last_size_.cols ? last_size_ : grid_for_bounds();
    const float gx = x - f(grid_x()), gy = y - f(grid_y() + pixel_);
    const int32_t cols = g.cols, rows = g.rows;
    const auto col = static_cast<int32_t>(std::floor(gx / f(std::max(1, m.cell_w))));
    const auto row = static_cast<int32_t>(std::floor(gy / f(std::max(1, m.cell_h))));
    CellHit h;
    h.inside = gx >= 0 && gy >= 0 && col < cols && row < rows;
    h.col = std::clamp(col, 0, cols - 1);
    h.row = std::clamp(row, 0, rows - 1);
    h.px = static_cast<uint16_t>(std::clamp(static_cast<int32_t>(std::floor(gx)), 0, std::max(0, cols * m.cell_w - 1)));
    h.py = static_cast<uint16_t>(std::clamp(static_cast<int32_t>(std::floor(gy)), 0, std::max(0, rows * m.cell_h - 1)));
    return h;
}

render::IRect TerminalView::cell_rect(int32_t col, int32_t row) const {
    const text::CellMetrics& m = metrics();
    return render::IRect{grid_x() + col * m.cell_w, grid_y() + pixel_ + row * m.cell_h, m.cell_w, m.cell_h};
}

render::IRect TerminalView::row_rect(int32_t row) const {
    const text::CellMetrics& m = metrics();
    return render::IRect{bounds_.x, grid_y() + pixel_ + row * m.cell_h, bounds_.w, m.cell_h};
}

// ---------------------------------------------------------------- building

std::optional<text::AtlasRef> TerminalView::curly_strip() {
    const text::CellMetrics& m = metrics();
    const int32_t cw = std::max(1, m.cell_w);
    const int32_t t = std::max(1, m.underline_thickness);
    const int32_t a = std::max(1, (3 * t + 1) / 2);   // amplitude
    const int32_t h = 2 * a + t + 2;
    text::GlyphKey key;
    key.face = kDecoFace;
    key.glyph = 1;   // curly
    key.variant = static_cast<uint32_t>(cw) | (static_cast<uint32_t>(t) << 12) | (static_cast<uint32_t>(a) << 20);
    text::GlyphAtlas& atlas = tex_.atlas();
    if (auto ref = atlas.find(key)) return ref;
    text::GlyphBitmap bm;
    bm.w = static_cast<uint16_t>(cw);
    bm.h = static_cast<uint16_t>(h);
    bm.format = text::PageFormat::a8;
    bm.pixels.assign(static_cast<size_t>(cw) * static_cast<size_t>(h), 0);
    const float half = static_cast<float>(t) * 0.5f;
    for (int32_t x = 0; x < cw; ++x) {
        // One period per cell, so neighbouring cells join.
        const float phase = 2.0f * std::numbers::pi_v<float> * (static_cast<float>(x) + 0.5f) / static_cast<float>(cw);
        const float yc = static_cast<float>(h) * 0.5f - static_cast<float>(a) * std::sin(phase);
        for (int32_t y = 0; y < h; ++y) {
            const float d = std::fabs(static_cast<float>(y) + 0.5f - yc);
            const float cov = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
            bm.pixels[static_cast<size_t>(y) * static_cast<size_t>(cw) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::lround(cov * 255.0f));
        }
    }
    auto ref = atlas.insert(key, bm);
    if (!ref) return std::nullopt;
    return *ref;
}

void TerminalView::build_row(RowCache& rc, int32_t r, std::span<const vt::Cell> cells, uint16_t flags, uint64_t abs_line,
                             const vt::Frame& frame, bool cursor_row, BuildResult& result) {
    (void)flags;
    rc.bg.clear();
    rc.glyphs.clear();
    rc.deco.clear();
    rc.valid = true;

    const text::CellMetrics& m = metrics();
    const Theme& th = opts_.theme;
    const uint16_t cols = frame.size.cols;
    const int32_t x0 = grid_x(), y0 = grid_y() + r * m.cell_h;   // unshifted: emit() adds pixel_
    const std::vector<vt::Style>* styles = frame.styles.get();
    const std::vector<std::u32string>* graphemes = frame.graphemes.get();

    // Cursor cells drawn as a block under the text (focused block cursor).
    int32_t block0 = -1, block1 = -1;
    if (cursor_row && focused_ && frame.cursor.shape == vt::CursorShape::Block) {
        block0 = frame.cursor.col;
        block1 = block0 + 1;
        if (block0 < static_cast<int32_t>(cells.size())) {
            const vt::CellWidth w = cells[static_cast<size_t>(block0)].width();
            if (w == vt::CellWidth::WideLead) block1 = block0 + 2;
            else if (w == vt::CellWidth::WideTail && block0 > 0) block0 -= 1;
        }
    }

    std::vector<CellLook> looks(cols);
    for (uint16_t c = 0; c < cols; ++c) {
        const vt::Cell cell = c < cells.size() ? cells[c] : vt::Cell{};
        const vt::Style st = (styles && cell.style < styles->size()) ? (*styles)[cell.style] : vt::Style{};
        CellLook& lk = looks[c];
        lk.fg = th.resolve_fg(st.fg, st.has(vt::kBold));
        lk.bg = th.resolve(st.bg, th.background);
        if (st.has(vt::kInverse)) std::swap(lk.fg, lk.bg);
        if (st.has(vt::kFaint)) lk.fg = mix_rgb(lk.bg, lk.fg, opts_.dim_factor);
        lk.hidden = st.has(vt::kInvisible);
        lk.attrs = st.attrs;
        lk.underline = st.underline;
        for (const Highlight& h : highlights_) {
            if (h.abs_line == abs_line && c >= h.col0 && c < h.col1) {
                lk.bg = h.background;
                if (h.foreground) lk.fg = *h.foreground;
            }
        }
        if (c >= block0 && c < block1) {
            lk.bg = th.cursor;
            lk.fg = th.cursor_text;
        }
        lk.ul = st.underline_color.tag() == vt::Color::Default ? lk.fg : th.resolve(st.underline_color, lk.fg);
    }

    // Backgrounds: runs of one color; the default background is drawn once for the whole view.
    for (uint16_t c = 0; c < cols;) {
        const uint32_t bg = looks[c].bg;
        uint16_t e = static_cast<uint16_t>(c + 1);
        while (e < cols && looks[e].bg == bg) ++e;
        if (bg != th.background) rc.bg.push_back(solid(x0 + c * m.cell_w, y0, (e - c) * m.cell_w, m.cell_h, bg));
        c = e;
    }

    // Glyphs.
    cluster_buf_.clear();
    std::vector<std::pair<uint32_t, uint32_t>> spans(cols, {0, 0});
    cells_in_.assign(cols, text::CellIn{});
    for (uint16_t c = 0; c < cols; ++c) {
        const vt::Cell cell = c < cells.size() ? cells[c] : vt::Cell{};
        const vt::Style st = (styles && cell.style < styles->size()) ? (*styles)[cell.style] : vt::Style{};
        text::CellIn& in = cells_in_[c];
        in.style = text_style(st);
        switch (cell.width()) {
            case vt::CellWidth::WideLead: in.width = 2; break;
            case vt::CellWidth::WideTail: in.width = 0; break;
            default: in.width = 1; break;
        }
        if (looks[c].hidden || cell.width() == vt::CellWidth::WideTail || cell.width() == vt::CellWidth::Empty) continue;
        const auto start = static_cast<uint32_t>(cluster_buf_.size());
        if (cell.cluster()) {
            if (graphemes && cell.cp() < graphemes->size()) cluster_buf_ += (*graphemes)[cell.cp()];
        } else if (cell.cp() != 0) {
            cluster_buf_.push_back(cell.cp());
        }
        spans[c] = {start, static_cast<uint32_t>(cluster_buf_.size()) - start};
        if (spans[c].second && in.width == 2) in.emoji_presentation = vt::width::is_emoji_presentation(cluster_buf_[start]);
    }
    const std::u32string_view all(cluster_buf_);
    for (uint16_t c = 0; c < cols; ++c) {
        if (spans[c].second) cells_in_[c].cluster = all.substr(spans[c].first, spans[c].second);
    }
    shaper_->set_cursor_col(cursor_row ? std::optional<uint16_t>(frame.cursor.col) : std::nullopt);
    glyphs_out_.clear();
    if (!shaper_->shape_row(cells_in_, glyphs_out_)) {
        result.incomplete = true;
        rc.valid = false;   // rebuilt by the next build()
    }
    for (const text::GlyphOut& g : glyphs_out_) {
        if (g.ref.empty() || g.col >= cols) continue;
        const uint32_t color = render_color(looks[g.col].fg);
        render::Quad q = render::glyph_quad(f(x0 + g.col * m.cell_w + g.dx), f(y0 + g.dy), g.ref.x, g.ref.y, g.ref.w, g.ref.h, color);
        if (g.ref.kind == static_cast<uint8_t>(text::GlyphKind::color)) q.kind = static_cast<uint8_t>(render::QuadKind::image);
        else if (g.ref.kind == static_cast<uint8_t>(text::GlyphKind::lcd)) q.kind = static_cast<uint8_t>(render::QuadKind::glyph_lcd);
        rc.glyphs.push_back(GlyphQuad{q, g.ref.page});
    }

    // Decorations: underline styles, strikeout, overline, in runs of equal kind and color.
    const int32_t t = std::max(1, m.underline_thickness);
    auto runs = [&](auto key_of, auto draw) {
        for (uint16_t c = 0; c < cols;) {
            const auto k = key_of(looks[c]);
            uint16_t e = static_cast<uint16_t>(c + 1);
            while (e < cols && key_of(looks[e]) == k) ++e;
            draw(k, c, e);
            c = e;
        }
    };
    // Underlines.
    runs([](const CellLook& l) { return std::pair{l.hidden ? vt::Underline::None : l.underline, l.ul}; },
         [&](std::pair<vt::Underline, uint32_t> k, uint16_t c, uint16_t e) {
             const int32_t rx = x0 + c * m.cell_w, rw = (e - c) * m.cell_w;
             const uint32_t color = k.second;
             switch (k.first) {
                 case vt::Underline::None: break;
                 case vt::Underline::Single: rc.deco.push_back(solid(rx, y0 + m.underline_pos, rw, t, color)); break;
                 case vt::Underline::Double: {
                     int32_t ya = m.underline_pos, yb = m.underline_pos + 2 * t;
                     if (yb + t > m.cell_h) {
                         const int32_t shift = yb + t - m.cell_h;
                         ya = std::max(0, ya - shift);
                         yb = std::max(ya + t + 1, yb - shift);
                     }
                     rc.deco.push_back(solid(rx, y0 + ya, rw, t, color));
                     rc.deco.push_back(solid(rx, y0 + yb, rw, t, color));
                     break;
                 }
                 case vt::Underline::Curly: {
                     auto ref = curly_strip();
                     if (!ref || ref->empty()) {
                         rc.deco.push_back(solid(rx, y0 + m.underline_pos, rw, t, color));
                         break;
                     }
                     const int32_t ytop = std::clamp(m.underline_pos + t / 2 - ref->h / 2, 0, std::max(0, m.cell_h - ref->h));
                     for (uint16_t cc = c; cc < e; ++cc) {
                         render::Quad q = render::glyph_quad(f(x0 + cc * m.cell_w), f(y0 + ytop), ref->x, ref->y, ref->w, ref->h,
                                                             render_color(color));
                         rc.glyphs.push_back(GlyphQuad{q, ref->page});
                     }
                     break;
                 }
                 case vt::Underline::Dotted: {
                     const int32_t d = t, period = 2 * t;
                     int32_t px = rx + (period - (rx - x0) % period) % period;
                     for (; px < rx + rw; px += period)
                         rc.deco.push_back(solid(px, y0 + m.underline_pos, std::min(d, rx + rw - px), t, color));
                     break;
                 }
                 case vt::Underline::Dashed: {
                     const int32_t dash = std::max(1, m.cell_w / 2);
                     for (uint16_t cc = c; cc < e; ++cc)
                         rc.deco.push_back(solid(x0 + cc * m.cell_w + m.cell_w / 4, y0 + m.underline_pos, dash, t, color));
                     break;
                 }
             }
         });
    // Strikeout and overline use the foreground color.
    runs([](const CellLook& l) { return std::pair{!l.hidden && (l.attrs & vt::kStrike) != 0, l.fg}; },
         [&](std::pair<bool, uint32_t> k, uint16_t c, uint16_t e) {
             if (k.first)
                 rc.deco.push_back(solid(x0 + c * m.cell_w, y0 + m.strikeout_pos, (e - c) * m.cell_w,
                                         std::max(1, m.strikeout_thickness), k.second));
         });
    runs([](const CellLook& l) { return std::pair{!l.hidden && (l.attrs & vt::kOverline) != 0, l.fg}; },
         [&](std::pair<bool, uint32_t> k, uint16_t c, uint16_t e) {
             if (k.first) rc.deco.push_back(solid(x0 + c * m.cell_w, y0, (e - c) * m.cell_w, t, k.second));
         });
}

void TerminalView::add_damage_rows(const std::vector<uint8_t>& rows, BuildResult& result) const {
    const text::CellMetrics& m = metrics();
    for (size_t r = 0; r < rows.size();) {
        if (!rows[r]) {
            ++r;
            continue;
        }
        size_t e = r + 1;
        while (e < rows.size() && rows[e]) ++e;
        render::IRect rect = row_rect(static_cast<int32_t>(r));
        rect.h = static_cast<int32_t>(e - r) * m.cell_h;
        result.damage.push_back(rect);
        r = e;
    }
}

void TerminalView::retain(const vt::Frame& frame) {
    Retained& k = kept_;
    const bool all = !k.valid || frame.damage.full || frame.size.cols != k.frame.size.cols || frame.size.rows != k.frame.size.rows;
    const size_t cols = frame.size.cols;
    if (all) {
        k.cells.assign(cols * frame.size.rows, vt::Cell{});
        k.rows.assign(frame.size.rows, vt::FrameRow{});
        for (size_t r = 0; r < k.rows.size(); ++r) k.rows[r].cells = std::span<const vt::Cell>(k.cells.data() + r * cols, cols);
    }
    for (size_t r = 0; r < k.rows.size() && r < frame.rows.size(); ++r) {
        if (!all && !frame.damage.test(r)) continue;
        const std::span<const vt::Cell> src = frame.rows[r].cells;
        const size_t n = std::min(src.size(), cols);
        std::copy_n(src.begin(), n, k.cells.begin() + static_cast<std::ptrdiff_t>(r * cols));
        std::fill_n(k.cells.begin() + static_cast<std::ptrdiff_t>(r * cols + n), cols - n, vt::Cell{});
        k.rows[r].flags = frame.rows[r].flags;
    }
    k.frame.seq = frame.seq;
    k.frame.size = frame.size;
    k.frame.rows = k.rows;
    k.frame.damage = vt::DamageSet{};
    k.frame.cursor = frame.cursor;
    k.frame.modes = frame.modes;
    k.frame.styles = frame.styles;
    k.frame.links = frame.links;
    k.frame.graphemes = frame.graphemes;
    k.frame.first_abs_line = frame.first_abs_line;
    k.frame.scrollback_lines = frame.scrollback_lines;
    k.valid = true;
}

bool TerminalView::redraw(render::DrawList& out, BuildResult& result, const ScrollbackSource* scrollback) {
    if (!kept_.valid) {
        result = BuildResult{};
        return false;
    }
    build_frame(kept_.frame, out, result, scrollback);
    return true;
}

void TerminalView::build(const vt::Frame& frame, render::DrawList& out, BuildResult& result, const ScrollbackSource* scrollback) {
    retain(frame);
    build_frame(frame, out, result, scrollback);
}

void TerminalView::build_frame(const vt::Frame& frame, render::DrawList& out, BuildResult& result,
                               const ScrollbackSource* scrollback) {
    result = BuildResult{};
    const vt::GridSize size = frame.size;
    const uint16_t rows = size.rows;
    bool full = invalid_ || size.cols != last_size_.cols || size.rows != last_size_.rows || frame.damage.full;

    // Viewport: rows above the screen come from the scrollback.
    const uint64_t sb_first = scrollback ? std::max(scrollback->first_abs_line(), frame.first_abs_line - std::min(frame.first_abs_line, frame.scrollback_lines))
                                         : frame.first_abs_line;
    const uint64_t available = frame.first_abs_line - std::min(sb_first, frame.first_abs_line);
    const uint32_t last_offset = last_offset_;   // of the last build: scroll_to() changed offset_ already
    if (!anchored_) {
        offset_ = static_cast<uint32_t>(std::min<uint64_t>(offset_, available));
        anchor_ = frame.first_abs_line - offset_;
        anchored_ = true;
    } else if (offset_ > 0) {
        // Keep the top row on its absolute line while output arrives.
        anchor_ = std::max(anchor_, sb_first);
        offset_ = static_cast<uint32_t>(std::min<uint64_t>(frame.first_abs_line - std::min(anchor_, frame.first_abs_line), available));
    }
    if (offset_ == 0) anchor_ = frame.first_abs_line;
    // Between rows only with a row above to show, and while smooth scrolling is on.
    if (!smooth_scroll() || offset_ >= available || pixel_ >= metrics().cell_h) pixel_ = 0;
    const uint64_t top_abs = frame.first_abs_line - offset_;
    if ((offset_ > 0 || last_offset > 0) && (top_abs != last_top_abs_ || offset_ != last_offset)) full = true;

    if (rows_.size() != rows) rows_.assign(rows, RowCache{});

    // Rows to rebuild.
    std::vector<uint8_t> dirty(rows, 0);
    for (uint16_t r = 0; r < rows; ++r) {
        if (full || !rows_[r].valid) dirty[r] = 1;
        else if (r >= offset_ && frame.damage.test(r - offset_)) dirty[r] = 1;
    }
    const uint32_t cursor_vrow = uint32_t{frame.cursor.row} + offset_;
    const bool cursor_drawn = frame.cursor.visible && !(blink_hidden_ && frame.cursor.blink) && cursor_vrow < rows &&
                              frame.cursor.col < size.cols;
    if (cursor_drawn != last_cursor_drawn_ || !(frame.cursor == last_cursor_) || focused_ != last_focused_ ||
        cursor_vrow != last_cursor_row_) {
        if (last_cursor_drawn_ && last_cursor_row_ < rows) dirty[last_cursor_row_] = 1;
        if (cursor_drawn) dirty[cursor_vrow] = 1;
    }

    const bool own_frame = !tex_.in_frame();
    if (own_frame) tex_.begin_frame();
    for (int pass = 0; pass < 2; ++pass) {
        for (uint16_t r = 0; r < rows; ++r) {
            if (!dirty[r]) continue;
            std::span<const vt::Cell> cells;
            uint16_t flags = 0;
            if (r < offset_) {
                if (scrollback) cells = scrollback->line(top_abs + r, flags);
            } else if (size_t{r} - offset_ < frame.rows.size()) {
                const vt::FrameRow& fr = frame.rows[r - offset_];
                cells = fr.cells;
                flags = fr.flags;
            }
            build_row(rows_[r], r, cells, flags, top_abs + r, frame, cursor_drawn && r == cursor_vrow, result);
            ++result.rows_rebuilt;
        }
        if (pixel_ > 0 && (full || !above_.valid || above_abs_ != top_abs - 1)) {
            std::span<const vt::Cell> cells;
            uint16_t flags = 0;
            if (scrollback) cells = scrollback->line(top_abs - 1, flags);
            build_row(above_, -1, cells, flags, top_abs - 1, frame, false, result);
            above_abs_ = top_abs - 1;
        }
        // A page evicted while shaping invalidates glyph refs of the cached rows: rebuild them all.
        if (!tex_.sync() || pass == 1) break;
        full = true;
        above_.valid = false;
        std::fill(dirty.begin(), dirty.end(), uint8_t{1});
    }
    if (own_frame) tex_.end_frame();

    // Cursor shapes drawn over the text.
    cursor_quads_.clear();
    if (cursor_drawn) {
        const text::CellMetrics& m = metrics();
        int32_t span = 1;
        if (cursor_vrow >= offset_) {
            const vt::FrameRow& fr = frame.rows[cursor_vrow - offset_];
            if (frame.cursor.col < fr.cells.size() && fr.cells[frame.cursor.col].width() == vt::CellWidth::WideLead) span = 2;
        }
        render::IRect cr = cell_rect(frame.cursor.col, static_cast<int32_t>(cursor_vrow));
        cr.y -= pixel_;   // emit() shifts the cursor with the rows
        const int32_t w = cr.w * span;
        const uint32_t color = opts_.theme.cursor;
        const int32_t line = std::max(1, m.box_light);
        switch (frame.cursor.shape) {
            case vt::CursorShape::Block:
                if (!focused_) {   // hollow box when the pane has no focus
                    cursor_quads_.push_back(solid(cr.x, cr.y, w, line, color));
                    cursor_quads_.push_back(solid(cr.x, cr.bottom() - line, w, line, color));
                    cursor_quads_.push_back(solid(cr.x, cr.y + line, line, cr.h - 2 * line, color));
                    cursor_quads_.push_back(solid(cr.x + w - line, cr.y + line, line, cr.h - 2 * line, color));
                }
                break;
            case vt::CursorShape::Underline: {
                const int32_t h = std::max(2, m.underline_thickness);
                cursor_quads_.push_back(solid(cr.x, cr.bottom() - h, w, h, color));
                break;
            }
            case vt::CursorShape::Bar: cursor_quads_.push_back(solid(cr.x, cr.y, std::max(2, line), cr.h, color)); break;
        }
    }

    // Damage. Between rows every row moves, so any change repaints the whole view.
    if (pixel_ != last_pixel_ || (pixel_ > 0 && std::find(dirty.begin(), dirty.end(), uint8_t{1}) != dirty.end())) full = true;
    if (full) {
        result.full = true;
        result.damage.assign(1, bounds_);
    } else {
        add_damage_rows(dirty, result);
    }

    last_size_ = size;
    last_cursor_ = frame.cursor;
    last_cursor_drawn_ = cursor_drawn;
    last_cursor_row_ = static_cast<uint16_t>(std::min<uint32_t>(cursor_vrow, 0xFFFF));
    last_focused_ = focused_;
    last_top_abs_ = top_abs;
    last_pixel_ = pixel_;
    last_offset_ = offset_;
    invalid_ = false;

    emit(out);
}

void TerminalView::emit(render::DrawList& out) {
    out.quad(solid(bounds_.x, bounds_.y, bounds_.w, bounds_.h, opts_.theme.background));
    const float dy = f(pixel_);
    // Between rows the rows move down by pixel_ and the row above comes in at the top; the view
    // clips what leaves its bounds.
    const bool shifted = pixel_ > 0;
    if (shifted) out.push_clip(bounds_);
    auto at = [dy](render::Quad q) {
        q.y += dy;
        return q;
    };
    auto each_row = [&](auto&& fn) {
        if (shifted && above_.valid) fn(above_);
        for (const RowCache& rc : rows_) fn(rc);
    };
    each_row([&](const RowCache& rc) {
        for (const render::Quad& q : rc.bg) out.quad(at(q));
    });
    each_row([&](const RowCache& rc) {
        for (const GlyphQuad& g : rc.glyphs) {
            const render::TextureId t = tex_.texture(g.page);
            if (t) out.quad(at(g.q), t);
        }
    });
    each_row([&](const RowCache& rc) {
        for (const render::Quad& q : rc.deco) out.quad(at(q));
    });
    for (const render::Quad& q : cursor_quads_) out.quad(at(q));
    if (shifted) out.pop_clip();
}

// ---------------------------------------------------------------- input

InputResult TerminalView::key(const KeyInput& in, const vt::InputModes& modes, std::string& out) {
    if (in.composing) return InputResult::ignored;
    vt::KeyEvent ev;
    ev.key = in.key;
    ev.mods = to_vt_mods(in.mods);
    ev.action = in.action;
    ev.text = in.text;
    ev.logical = in.logical;
    if (!vt::InputEncoder::encode_key(ev, modes, opts_.encoder, out)) return InputResult::ignored;
    scroll_to(0);   // typing returns to the live screen
    return InputResult::sent;
}

namespace {

vt::MouseButton vt_button(PointerButton b) {
    switch (b) {
        case PointerButton::left: return vt::MouseButton::Left;
        case PointerButton::middle: return vt::MouseButton::Middle;
        case PointerButton::right: return vt::MouseButton::Right;
        case PointerButton::back: return vt::MouseButton::Back;
        case PointerButton::forward: return vt::MouseButton::Forward;
        case PointerButton::none: break;
    }
    return vt::MouseButton::None;
}

}  // namespace

InputResult TerminalView::pointer(const PointerInput& in, const vt::InputModes& modes, std::string& out) {
    using Type = PointerInput::Type;
    if (in.type == Type::enter || in.type == Type::leave) return InputResult::ignored;
    if (in.type == Type::press) held_ = in.button;
    const PointerButton held = held_;
    if (in.type == Type::release && in.button == held_) held_ = PointerButton::none;

    // Shift always selects, so the user can copy from programs that track the mouse.
    if (modes.mouse_tracking == vt::MouseTracking::None || (in.mods & kPmShift))
        return (in.type == Type::move && held == PointerButton::none) ? InputResult::ignored : InputResult::selection;

    const CellHit h = hit(in.x, in.y);
    vt::MouseEvent ev;
    ev.action = in.type == Type::press ? vt::MouseAction::Press
                : in.type == Type::release ? vt::MouseAction::Release
                                           : vt::MouseAction::Move;
    ev.button = vt_button(in.type == Type::move ? held : in.button);
    ev.mods = to_vt_mods(in.mods);
    ev.col = static_cast<uint16_t>(h.col);
    ev.row = static_cast<uint16_t>(h.row);
    ev.px = h.px;
    ev.py = h.py;
    return vt::InputEncoder::encode_mouse(ev, modes, out) ? InputResult::sent : InputResult::ignored;
}

InputResult TerminalView::wheel(const WheelInput& in, const vt::InputModes& modes, std::string& out) {
    const bool tracking = modes.mouse_tracking != vt::MouseTracking::None && !(in.mods & kPmShift);
    const bool to_app = tracking || (modes.alt_screen && modes.alt_scroll && modes.mouse_tracking == vt::MouseTracking::None);
    if (!to_app && !modes.alt_screen && smooth_scroll()) {
        if (in.precise) {
            // Touchpad and high-resolution wheels: the content follows the fingers pixel by pixel.
            wheel_px_carry_ += in.dy;
            const auto d = static_cast<int32_t>(std::trunc(wheel_px_carry_));
            wheel_px_carry_ -= f(d);
            scroll_by_px(d);
            precise_seen_ = true;
            return InputResult::scrolled;
        }
        wheel_accum_ += in.dy;
        const auto n = static_cast<int32_t>(std::trunc(wheel_accum_));
        if (n == 0) return InputResult::ignored;
        wheel_accum_ -= static_cast<float>(n);
        scroll_by_animated(n * opts_.wheel_lines);
        return InputResult::scrolled;
    }
    const float lines = in.precise ? in.dy / f(std::max(1, metrics().cell_h)) : in.dy;
    wheel_accum_ += lines;
    const auto notches = static_cast<int32_t>(std::trunc(wheel_accum_));
    if (notches == 0) return InputResult::ignored;
    wheel_accum_ -= static_cast<float>(notches);

    if (to_app) {
        const CellHit h = hit(in.x, in.y);
        vt::MouseEvent ev;
        ev.action = vt::MouseAction::Wheel;
        ev.button = notches > 0 ? vt::MouseButton::WheelUp : vt::MouseButton::WheelDown;
        ev.mods = to_vt_mods(in.mods);
        ev.col = static_cast<uint16_t>(h.col);
        ev.row = static_cast<uint16_t>(h.row);
        ev.px = h.px;
        ev.py = h.py;
        bool any = false;
        for (int32_t i = 0; i < std::abs(notches); ++i) any = vt::InputEncoder::encode_mouse(ev, modes, out) || any;
        return any ? InputResult::sent : InputResult::ignored;
    }
    if (modes.alt_screen) return InputResult::ignored;   // the alternate screen has no scrollback
    scroll_by(notches * opts_.wheel_lines);
    return InputResult::scrolled;
}

void TerminalView::paste(std::u8string_view text, const vt::InputModes& modes, std::string& out) {
    scroll_to(0);
    vt::InputEncoder::encode_paste(text, modes, out);
}

void TerminalView::focus(bool focused, const vt::InputModes& modes, std::string& out) {
    set_focused(focused);
    vt::InputEncoder::encode_focus(focused, modes, out);
}

}  // namespace kt::view
