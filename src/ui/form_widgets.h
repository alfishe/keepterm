// Form controls of the Settings window: slider,
// stepper, dropdown, segmented control, color well, key-chord recorder, search field, the
// section list of a sidebar, a color scheme preview, and the grouped form layout (groups of
// rows with a label, a description, the control and an inline error).
//
// Logic-free like the rest of the toolkit: controls report user changes through callbacks and
// take values through setters that never call back (so a reload can update them in place).
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/widgets.h"

namespace kt::ui {

// ---------------------------------------------------------------- Slider

// A horizontal track with a knob and a value readout on the right. Drag, click on the track,
// arrow keys (step), Page Up/Down (10 steps), Home/End. on_change runs while dragging;
// on_commit once when the drag ends or after a key step.
class Slider : public Widget {
public:
    void on_text_metrics_changed() override {
        run_.reset();
        Widget::on_text_metrics_changed();
    }
    Slider(double lo, double hi, double step, double value, std::string id = {});
    void set_value(double v);   // no callback
    double value() const { return value_; }
    void set_range(double lo, double hi, double step);
    // Readout text; default: the value with the step's decimals.
    void set_format(std::function<std::string(double)> f);
    std::string readout() const;
    std::function<void(double)> on_change;
    std::function<void(double)> on_commit;

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override { return readout(); }

protected:
    Size intrinsic_size(Size) const override { return {200, 22}; }

private:
    Rect track() const;
    double snap_value(double v) const;
    double value_at(float x) const;
    void user_set(double v, bool commit);
    double lo_, hi_, step_, value_;
    bool dragging_ = false;
    std::function<std::string(double)> format_;
    mutable TextRunPtr run_;
    mutable std::string run_text_;
};

// ---------------------------------------------------------------- Stepper

// A value with - and + buttons (integers or floats). Arrow keys step too.
class Stepper : public Widget {
public:
    void on_text_metrics_changed() override {
        run_.reset();
        Widget::on_text_metrics_changed();
    }
    Stepper(double lo, double hi, double step, double value, std::string id = {});
    void set_value(double v);
    double value() const { return value_; }
    void set_format(std::function<std::string(double)> f);
    std::string text() const;
    std::function<void(double)> on_change;
    void step_by(int n);   // as if a button was pressed

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override { return text(); }

protected:
    Size intrinsic_size(Size) const override;

private:
    double lo_, hi_, step_, value_;
    int pressed_ = 0;   // -1, 0, +1
    std::function<std::string(double)> format_;
    mutable TextRunPtr run_;
    mutable std::string run_text_;
};

// ---------------------------------------------------------------- Dropdown

struct DropdownOption {
    std::string label;
    std::string detail;            // muted text after the label in the menu ("default")
    std::optional<Color> swatch;   // a color dot (themes)
};

// A button showing the selected option with a chevron; a click opens a menu of the options
// below it. Up/Down change the selection without opening.
class Dropdown : public Widget {
public:
    void on_text_metrics_changed() override {
        run_.reset();
        Widget::on_text_metrics_changed();
    }
    explicit Dropdown(std::vector<DropdownOption> options, size_t selected = 0, std::string id = {});
    void set_options(std::vector<DropdownOption> options, size_t selected);
    void set_selected(size_t i);   // no callback
    size_t selected() const { return selected_; }
    const std::vector<DropdownOption>& options() const { return options_; }
    // Text shown when the selection is outside the options (a value the file sets).
    void set_placeholder(std::string s);
    void open();
    void choose(size_t i);         // as if picked in the menu
    std::function<void(size_t)> on_change;

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    void on_hover(bool) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override;

protected:
    Size intrinsic_size(Size) const override;

private:
    std::string shown() const;
    std::vector<DropdownOption> options_;
    size_t selected_;
    std::string placeholder_;
    WidgetId menu_ = 0;
    Animated hover_;
    mutable TextRunPtr run_;
    mutable std::string run_text_;
};

// ---------------------------------------------------------------- SegmentedControl

class SegmentedControl : public Widget {
public:
    void on_text_metrics_changed() override {
        runs_.clear();
        Widget::on_text_metrics_changed();
    }
    explicit SegmentedControl(std::vector<std::string> labels, size_t selected = 0, std::string id = {});
    void set_selected(size_t i);   // no callback; npos shows none selected
    size_t selected() const { return selected_; }
    size_t count() const { return labels_.size(); }
    void choose(size_t i);
    std::function<void(size_t)> on_change;

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override;
    static constexpr size_t npos = static_cast<size_t>(-1);

protected:
    Size intrinsic_size(Size) const override;

private:
    std::vector<float> widths() const;
    std::optional<size_t> segment_at(float x) const;
    std::vector<std::string> labels_;
    size_t selected_;
    mutable std::vector<TextRunPtr> runs_;
};

// ---------------------------------------------------------------- ColorWell

// A color swatch with its hex text. A click opens a menu of preset colors (and "Use theme
// color" when the value may be empty); the text field takes #RRGGBB / #RRGGBBAA.
class ColorWell : public Widget {
public:
    explicit ColorWell(std::string value = {}, std::string id = {});
    void set_value(std::string hex);   // "" = from theme; no callback
    const std::string& value() const { return value_; }
    // Shown for an empty value (the theme's color).
    void set_fallback(std::optional<Color> c);
    void set_presets(std::vector<std::pair<std::string, Color>> presets) { presets_ = std::move(presets); }
    void set_allow_empty(bool b) { allow_empty_ = b; }
    TextField& field() { return *field_; }
    void open_presets();
    std::function<void(const std::string&)> on_change;

    void paint(PaintContext&) const override;
    EventResult on_pointer(PointerEvent&) override;
    std::string describe() const override { return value_.empty() ? std::string("theme") : value_; }
    static std::optional<Color> parse(std::string_view hex);

private:
    Rect swatch() const;
    std::string value_;
    std::optional<Color> fallback_;
    std::vector<std::pair<std::string, Color>> presets_;
    bool allow_empty_ = true;
    TextField* field_ = nullptr;
};

// ---------------------------------------------------------------- KeyRecorder

// Shows a binding's key caps. A click (or Enter / Space) starts recording: the next key press
// with its modifiers is reported through on_record. While recording, Escape cancels and a bare
// Backspace or Delete reports on_clear (unbind). Losing focus cancels.
class KeyRecorder : public Widget {
public:
    explicit KeyRecorder(std::string caps = {}, std::string id = {});
    void set_caps(std::string caps);   // "" shows the placeholder
    const std::string& caps() const { return caps_; }
    void set_placeholder(std::string p) { placeholder_ = std::move(p); invalidate_paint(); }
    void set_conflict(bool c) { conflict_ = c; invalidate_paint(); }
    bool recording() const { return recording_; }
    void start();
    void cancel();
    std::function<void(const KeyEvent&)> on_record;
    std::function<void()> on_clear;
    std::function<void(bool)> on_recording;   // started / stopped

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(4); }
    EventResult on_pointer(PointerEvent&) override;
    EventResult on_key(const KeyEvent&) override;
    void on_focus(FocusChange) override;
    void on_hover(bool) override;
    bool accepts_focus() const override { return true; }
    std::string describe() const override;
    static bool is_modifier(platform::Key k);

protected:
    Size intrinsic_size(Size) const override { return {150, 24}; }

private:
    std::string caps_, placeholder_ = "Not bound";
    bool recording_ = false, conflict_ = false;
    Animated hover_;
};

// ---------------------------------------------------------------- SearchField

// A rounded field with a magnifier and a clear button.
class SearchField : public Widget {
public:
    explicit SearchField(std::string placeholder = "Search", std::string id = "search");
    TextField& field() { return *field_; }
    const std::string& text() const { return field_->text(); }
    void set_text(std::string t);   // no callback
    std::function<void(const std::string&)> on_change;

    void paint(PaintContext&) const override;
    EventResult on_pointer(PointerEvent&) override;

private:
    TextField* field_ = nullptr;
    Button* clear_ = nullptr;
};

// ---------------------------------------------------------------- SectionList

struct SectionItem {
    std::string id, title;
    Icon icon = Icon::none;
    std::string badge;   // a short count ("3") on the right
};

// The sidebar navigation of Settings: icon and title rows, one selected.
class SectionList : public ListView {
public:
    explicit SectionList(std::vector<SectionItem> items, std::string id = "sections");
    const std::vector<SectionItem>& items() const { return items_; }
    void set_badge(size_t i, std::string b);
    std::optional<size_t> index_of(std::string_view id) const;
    std::string describe() const override;

private:
    std::vector<SectionItem> items_;
};

// ---------------------------------------------------------------- SchemePreview

// A terminal color scheme at a glance: background, a few lines of sample text in the scheme's
// colors, the cursor, the selection and the 16 ANSI swatches.
struct SchemeColors {
    Color background, foreground, cursor, selection;
    std::array<Color, 16> palette{};
    std::string name;
};

class SchemePreview : public Widget {
public:
    explicit SchemePreview(std::string id = "preview");
    void set_scheme(SchemeColors s);
    const SchemeColors& scheme() const { return s_; }
    void paint(PaintContext&) const override;
    std::string describe() const override { return s_.name; }

protected:
    Size intrinsic_size(Size avail) const override { return {std::min(avail.w, 520.0f), 112}; }

private:
    SchemeColors s_;
};

// ---------------------------------------------------------------- form layout

// One setting: label (and description) on the left, the control on the right, an inline
// error under both. `wide` puts the control under the label at full width (lists, editors).
class FormRow : public Widget {
public:
    FormRow(std::string label, std::string description, std::unique_ptr<Widget> control, std::string id = {},
            bool wide = false);
    Widget* control() const { return control_; }
    Label& label() { return *label_; }
    void set_error(std::string e);   // "" hides
    const std::string& error() const { return error_; }
    // A note under the control ("Set by --set"); Tone::warning for keymap conflicts.
    void set_note(std::string n, Tone tone = Tone::muted);
    const std::string& note() const;
    void set_dimmed(bool d);
    std::function<void(Vec2)> on_context;   // right click (window position)
    EventResult on_pointer(PointerEvent&) override;
    std::string describe() const override;

private:
    Label* label_ = nullptr;
    Label* desc_ = nullptr;
    Label* error_label_ = nullptr;
    Label* note_ = nullptr;
    Widget* control_ = nullptr;
    std::string error_;
};

// A titled group of rows on a raised surface with separators; an optional footnote under it.
class FormGroup : public Widget {
public:
    explicit FormGroup(std::string title, std::string id = {});
    template <class T>
    T& add_row(std::unique_ptr<T> row) {
        T& r = *row;
        body_->add_child(std::move(row));
        return r;
    }
    Widget& body() { return *body_; }
    size_t rows() const { return body_->child_count(); }
    void set_footnote(std::string f);
    void paint(PaintContext&) const override;

private:
    Label* title_ = nullptr;
    Label* foot_ = nullptr;
    Box* body_ = nullptr;
};

}  // namespace kt::ui
