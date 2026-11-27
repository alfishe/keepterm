// The core widgets: label, icon, button, toggle, text field, scroll view,
// virtualized list, tooltip, toast, menu, dialog, badge, gauge, sparkline.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/widget.h"
#include "ui/window.h"

namespace kt::ui {

// Text styles resolved against the theme (font scale) at measure time.
enum class TextStyle : uint8_t { body, body_bold, small, small_bold, title, large_title, mono_small, mono_body };
FontRole role_for(const Theme&, TextStyle);

// Semantic colors.
enum class Tone : uint8_t { normal, muted, dim, accent, success, warning, failure, on_accent };
Color tone_color(const Theme&, Tone);

// The modifier of standard shortcuts (copy, select all): Cmd on macOS, Ctrl elsewhere.
inline uint16_t primary_mod() {
#if defined(__APPLE__)
    return platform::mod_super;
#else
    return platform::mod_ctrl;
#endif
}

// A 2 DIP accent ring 2 DIP outside `r` (focus-visible only). Needs a paint_outset of 4.
void paint_focus_ring(PaintContext&, Rect r, float radius);

// ---------------------------------------------------------------- Label

class Label : public Widget {
public:
    enum class Elide : uint8_t { none, end, middle };
    enum class Align : uint8_t { start, center, end };
    explicit Label(std::string text = {}, TextStyle style = TextStyle::body, Tone tone = Tone::normal, std::string id = {});

    void set_text(std::string);
    const std::string& text() const { return text_; }
    void set_text_style(TextStyle);
    void set_tone(Tone);
    void set_color(std::optional<Color>);   // overrides the tone
    void set_elide(Elide e);
    void set_align(Align a);
    void set_wrap(bool);                    // word wrap to the width (dialogs)
    // A fixed slot: the label is at least as wide as the widest of these texts, so changing
    // figures never move their neighbours (with Align::end: right-aligned numbers).
    void set_width_samples(std::vector<std::string>);
    bool truncated() const;
    std::string describe() const override { return text_; }

    void paint(PaintContext&) const override;
    void arrange(const Rect&) override;
    void on_text_metrics_changed() override;

protected:
    Size intrinsic_size(Size available) const override;

private:
    std::vector<TextRunPtr> wrap_lines(float width) const;
    std::string text_;
    TextStyle style_;
    Tone tone_;
    std::optional<Color> color_;
    Elide elide_ = Elide::end;
    Align align_ = Align::start;
    bool wrap_ = false;
    std::vector<std::string> samples_;
    mutable TextRunPtr full_, shown_;
    mutable std::vector<TextRunPtr> lines_;
    mutable float shown_w_ = -1;
    mutable float line_h_ = 0;
};

// ---------------------------------------------------------------- IconView

class IconView : public Widget {
public:
    explicit IconView(Icon icon = Icon::none, float size = 16, Tone tone = Tone::muted, std::string id = {});
    void set_icon(Icon);
    Icon icon() const { return icon_; }
    void set_tone(Tone);
    void set_color(std::optional<Color>);
    // A theme-dependent color (an agent's or a project tag's); wins over set_color.
    void set_color_fn(std::function<Color(const Theme&)>);
    void set_size(float);
    void paint(PaintContext&) const override;
    std::string describe() const override { return std::string(icon_name(icon_)); }

protected:
    Size intrinsic_size(Size) const override { return {size_, size_}; }

private:
    Icon icon_;
    float size_;
    Tone tone_;
    std::optional<Color> color_;
    std::function<Color(const Theme&)> color_fn_;
};

// ---------------------------------------------------------------- Button

class Button : public Widget {
public:
    void on_text_metrics_changed() override {
        run_.reset();
        Widget::on_text_metrics_changed();
    }
    enum class Kind : uint8_t { primary, secondary, plain, icon, danger };
    Button(std::string label, std::function<void()> on_click, Kind kind = Kind::secondary, std::string id = {});
    static std::unique_ptr<Button> icon_button(Icon icon, std::string tooltip, std::function<void()> on_click, std::string id = {},
                                               float size = 22);

    void set_label(std::string);
    const std::string& label() const { return label_; }
    void set_icon(Icon i, float size = 14);
    void set_kind(Kind);
    void set_on_click(std::function<void()> f) { on_click_ = std::move(f); }
    void set_checked(bool);   // icon buttons: a selected state (sidebar toggle, zoom)
    void set_align_start(bool on) { align_start_ = on; }   // content at the left edge (table headers)
    bool checked() const { return checked_; }
    void click();             // as if clicked (scripts, keyboard)
    bool pressed() const { return pressed_; }

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    void on_hover(bool) override;
    bool accepts_focus() const override { return kind_ != Kind::icon && kind_ != Kind::plain; }
    std::string describe() const override;

protected:
    Size intrinsic_size(Size) const override;

private:
    std::string label_;
    std::function<void()> on_click_;
    Kind kind_;
    Icon icon_ = Icon::none;
    float icon_size_ = 14;
    float square_ = 0;   // icon buttons: side length
    bool checked_ = false, pressed_ = false, align_start_ = false;
    Animated hover_, press_;
    mutable TextRunPtr run_;
};

// ---------------------------------------------------------------- Toggle

class Toggle : public Widget {
public:
    explicit Toggle(bool on = false, std::function<void(bool)> on_change = {}, std::string id = {});
    void set_on(bool on, bool animate = true);
    bool on() const { return on_; }
    void set_on_change(std::function<void(bool)> f) { on_change_ = std::move(f); }
    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override { return on_ ? "on" : "off"; }

protected:
    Size intrinsic_size(Size) const override { return {30, 18}; }

private:
    bool on_;
    std::function<void(bool)> on_change_;
    Animated knob_;
};

// ---------------------------------------------------------------- TextField

class TextField : public Widget, public ImeClient {
public:
    explicit TextField(std::string placeholder = {}, std::string id = {});
    void set_text(std::string t);
    const std::string& text() const { return text_; }
    void set_placeholder(std::string p);
    void select_all();
    size_t caret() const { return caret_; }
    std::pair<size_t, size_t> selection() const;
    std::function<void(const std::string&)> on_change;
    std::function<void(const std::string&)> on_submit;
    std::function<void()> on_cancel;
    // Keys the field does not use (up/down for a list below a filter field).
    std::function<bool(const KeyEvent&)> on_other_key;
    void set_frameless(bool f) { frameless_ = f; invalidate_paint(); }
    void set_text_style(TextStyle s) { style_ = s; invalidate_layout(); }
    const std::string& preedit() const { return preedit_; }

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    EventResult on_text(std::string_view) override;
    bool accepts_focus() const override { return true; }
    ImeClient* ime_client() override { return this; }
    platform::CursorShape cursor_at(Vec2) const override { return platform::CursorShape::ibeam; }
    std::string describe() const override { return text_; }

    // ImeClient
    void ime_preedit(std::string_view text, std::span<const ImeSpan> spans, int caret) override;
    void ime_commit(std::string_view text) override;
    void ime_cancel() override;
    Rect ime_caret_rect() const override;

protected:
    Size intrinsic_size(Size) const override;

private:
    void insert(std::string_view s);
    void erase_selection();
    void move_caret(size_t to, bool extend);
    float x_of(size_t byte) const;     // in the shaped display text, DIP
    size_t byte_at(float x) const;
    void changed();
    void reshape() const;
    std::string display() const;       // text with the preedit at the caret

    std::string text_, placeholder_, preedit_;
    std::vector<ImeSpan> preedit_spans_;
    size_t caret_ = 0, anchor_ = 0;
    TextStyle style_ = TextStyle::body;
    bool frameless_ = false;
    bool dragging_ = false;
    mutable TextRunPtr run_;
    mutable float scroll_ = 0;
};

// ---------------------------------------------------------------- ScrollView

class ScrollView : public Widget {
public:
    explicit ScrollView(std::unique_ptr<Widget> content = nullptr, std::string id = {});
    Widget* content() const { return child(0); }
    void set_content(std::unique_ptr<Widget>);
    float offset() const { return offset_; }
    void scroll_to(float y);
    void scroll_into_view(const Rect& content_rect);
    float content_height() const { return content_h_; }

    void arrange(const Rect&) override;
    void paint_over(PaintContext&) const override;
    EventResult on_wheel(WheelEvent&) override;
    EventResult on_pointer(PointerEvent&) override;

private:
    Rect thumb() const;
    float offset_ = 0, content_h_ = 0;
    bool dragging_ = false;
    float drag_from_ = 0, drag_offset_ = 0;
    Animated bar_;
    uint64_t fade_timer_ = 0;
};

// ---------------------------------------------------------------- ListView (virtualized)

class ListView : public Widget {
public:
    struct RowState {
        bool hovered, selected, focused;
    };
    using PaintRow = std::function<void(size_t index, PaintContext&, Rect row, RowState)>;
    explicit ListView(PaintRow paint_row = {}, std::string id = {});

    void set_paint_row(PaintRow f) { paint_row_ = std::move(f); invalidate_paint(); }
    void set_count(size_t n);
    size_t count() const { return count_; }
    void set_row_height(float h);
    float row_height() const { return row_h_; }
    std::optional<size_t> selected() const { return selected_; }
    void set_selected(std::optional<size_t>, bool scroll = true);
    void scroll_to_row(size_t);
    float offset() const { return offset_; }
    // Rows painted by the last paint (tests check virtualization).
    size_t painted_rows() const { return painted_; }

    std::function<void(size_t)> on_select;
    // Instead of on_select when set: with the modifiers of the click (Shift, Cmd/Ctrl), or
    // mod_shift for Shift with a navigation key; the owner keeps a selection of several rows.
    std::function<void(size_t, uint16_t mods)> on_select_mods;
    std::function<void(size_t)> on_activate;
    std::function<void(size_t, Vec2)> on_context;

    void paint(PaintContext&) const override;
    EventResult on_wheel(WheelEvent&) override;
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    void on_hover(bool) override;
    bool accepts_focus() const override { return true; }
    void arrange(const Rect&) override;
    std::string describe() const override;
    std::optional<size_t> row_at(float local_y) const;

private:
    void clamp_offset();
    PaintRow paint_row_;
    size_t count_ = 0;
    float row_h_ = 26, offset_ = 0;
    std::optional<size_t> selected_, hover_;
    mutable size_t painted_ = 0;
};

// ---------------------------------------------------------------- Badge, Gauge, Sparkline

// A pill: optional icon and a short text ("512 MB", "Zoomed").
class Badge : public Widget {
public:
    void on_text_metrics_changed() override {
        run_.reset();
        Widget::on_text_metrics_changed();
    }
    explicit Badge(std::string text = {}, Icon icon = Icon::none, Tone tone = Tone::muted, std::string id = {});
    void set_text(std::string);
    const std::string& text() const { return text_; }
    void set_icon(Icon);
    void set_tone(Tone);
    void set_filled(bool f) { filled_ = f; invalidate_paint(); }   // surface fill and border
    void paint(PaintContext&) const override;
    std::string describe() const override { return text_; }

protected:
    Size intrinsic_size(Size) const override;

private:
    std::string text_;
    Icon icon_;
    Tone tone_;
    bool filled_ = true;
    mutable TextRunPtr run_;
};

// A horizontal level bar (storage use): fraction 0..1, warning above 0.75, failure above 0.9.
class Gauge : public Widget {
public:
    explicit Gauge(std::string id = {});
    void set_fraction(float f);
    float fraction() const { return f_; }
    // The bar's height in DIP (default 6).
    void set_thickness(float t);
    // Grey fill instead of the accent/warning/failure ramp (figures that may be out of date).
    void set_muted(bool m);
    void paint(PaintContext&) const override;
    std::string describe() const override;

protected:
    Size intrinsic_size(Size) const override { return {60, thickness_}; }

private:
    float f_ = 0;
    float thickness_ = 6;
    bool muted_ = false;
};

// Recent values as columns, newest on the right.
class Sparkline : public Widget {
public:
    explicit Sparkline(size_t capacity = 60, std::string id = {});
    void push(float v);
    void set_values(std::vector<float> v);
    const std::deque<float>& values() const { return v_; }
    void set_tone(Tone t) { tone_ = t; invalidate_paint(); }
    void set_max(std::optional<float> m) { max_ = m; invalidate_paint(); }   // default: the data max
    void paint(PaintContext&) const override;

protected:
    Size intrinsic_size(Size) const override { return {48, 14}; }

private:
    size_t cap_;
    std::deque<float> v_;
    Tone tone_ = Tone::accent;
    std::optional<float> max_;
};

// ---------------------------------------------------------------- Tooltip and toasts

class Tooltip : public Widget {
public:
    explicit Tooltip(std::string text);
    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(8); }
};

struct ToastSpec {
    std::string title, subtitle;
    Icon icon = Icon::none;
    Tone tone = Tone::normal;
    std::chrono::milliseconds duration{3000};
};

class Toast : public Widget {
public:
    explicit Toast(const ToastSpec&);
    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(16); }
    void fade(float to, std::function<void()> done = {});
    std::string describe() const override { return title_; }

private:
    std::string title_;
    Animated alpha_;
};

// Shows a toast at the top center for spec.duration; a new toast replaces the shown one.
WidgetId show_toast(Window&, const ToastSpec&);

// ---------------------------------------------------------------- menus

struct MenuItem {
    std::string label;
    std::string shortcut;          // display text ("⌘T", "Ctrl+Shift+T")
    std::function<void()> action;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    Icon icon = Icon::none;
    std::optional<Color> swatch;   // a color dot (project color menu)
    std::vector<MenuItem> submenu;
    std::string id;                // automation id; default: the label
    static MenuItem sep() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

class Menu : public Widget {
public:
    explicit Menu(std::vector<MenuItem> items, std::string id = "menu");
    const std::vector<MenuItem>& items() const { return items_; }
    std::optional<size_t> highlighted() const { return hl_; }
    void activate(size_t i);
    void set_parent_menu(Menu* p) { parent_menu_ = p; }

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(16); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    void on_hover(bool) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override;

protected:
    Size intrinsic_size(Size) const override;

private:
    float row_y(size_t i) const;
    std::optional<size_t> item_at(float y) const;
    void open_submenu(size_t i);
    void close_submenu();
    void close_all();
    std::vector<MenuItem> items_;
    std::optional<size_t> hl_;
    Menu* parent_menu_ = nullptr;
    WidgetId sub_ = 0;
    std::optional<size_t> sub_index_;
};

// Opens a context menu at a window point (or below an anchor rect). Returns the overlay id.
WidgetId open_menu(Window&, std::vector<MenuItem> items, Vec2 at);
WidgetId open_menu_below(Window&, std::vector<MenuItem> items, Rect anchor);

// ---------------------------------------------------------------- dialogs

struct DialogSpec {
    std::string title, message;
    struct ButtonSpec {
        std::string label;
        std::function<void()> action;
        Button::Kind kind = Button::Kind::secondary;
        bool is_default = false, is_cancel = false;
    };
    std::vector<ButtonSpec> buttons;
    std::unique_ptr<Widget> body;   // optional extra content (a text field)
    float width = 380;
};

class Dialog : public Widget {
public:
    explicit Dialog(DialogSpec spec);
    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(24); }
    EventResult on_key(const KeyEvent&) override;
    void press(size_t button);
    void set_overlay(WidgetId id) { overlay_ = id; }
    std::shared_ptr<bool> pressed_flag() const { return pressed_; }

private:
    std::vector<DialogSpec::ButtonSpec> buttons_;
    std::shared_ptr<bool> pressed_ = std::make_shared<bool>(false);
    WidgetId overlay_ = 0;
};

WidgetId show_dialog(Window&, DialogSpec spec);

// Draws a text with its matches (byte offset, length) on a highlight, at most max_w wide, the top
// of the line at `top`; a long text is cut in front so that the first match shows. Returns the
// drawn width.
float paint_marked(PaintContext& p, std::string text, std::vector<std::pair<size_t, size_t>> marks, FontRole role, float x,
                   float top, float max_w, Color fg);

}  // namespace kt::ui
