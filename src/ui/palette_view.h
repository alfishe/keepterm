// The command palette's view: a field over a ranked list of
// rows in sections, at the top center of the window like Macterm's palette. Logic-free: the
// application feeds rows (from commands::palette::Engine) and reacts to the callbacks.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/widgets.h"

namespace kt::ui {

struct PaletteRow {
    std::string title, subtitle;
    std::string section;                   // a header is drawn where the section changes
    std::string keys;                      // key caps ("⌘D", "Ctrl+Shift+D")
    std::vector<uint32_t> highlights;      // byte offsets of matched characters in title
    Icon icon = Icon::none;
    bool enabled = true;
    bool checked = false;
    bool opens_scope = false;              // a chevron: Tab enters it
    std::string disabled_reason;
};

class PaletteList;

class PaletteView final : public Widget {
public:
    explicit PaletteView(std::string placeholder);

    TextField& field() { return *field_; }
    void set_rows(std::vector<PaletteRow> rows, std::string empty_text = {});
    const std::vector<PaletteRow>& rows() const;
    void set_scope(std::string name);      // the chip before the field; empty = root
    std::optional<size_t> selected() const;
    void select(size_t row);
    void move(int delta);                  // skips disabled rows; wraps
    void page(int dir);
    bool alt_held() const { return alt_; }

    std::function<void(const std::string& query)> on_query;
    std::function<void(size_t row, bool alt)> on_accept;
    std::function<void(size_t row)> on_enter_scope;
    std::function<bool()> on_pop_scope;    // Backspace on an empty field; false at the root
    std::function<void()> on_close;
    std::function<void(size_t row)> on_selection;

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(24); }
    EventResult on_key(const KeyEvent&) override;
    std::string describe() const override;

private:
    TextField* field_ = nullptr;
    Label* scope_ = nullptr;
    PaletteList* list_ = nullptr;
    bool alt_ = false;
};

// Shows the palette at the top center; returns the overlay id. `closed` runs when the overlay
// goes away for any reason (outside click, close_overlay).
WidgetId show_palette(Window&, std::unique_ptr<PaletteView>, std::function<void()> closed = {});

}  // namespace kt::ui
