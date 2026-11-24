// The Settings window's chrome (Macterm's Settings layout): a fixed 200 DIP
// sidebar with the search field over the section list, and on the right the page title, a
// banner for config problems and the scrolling form. Logic-free: the controller (src/app)
// builds the pages and reacts to the callbacks.
//
// Hosted as the root of its own OS window (set_windowed, Macterm's Settings window), or as a
// modal sheet over the main window (show_settings) where a second window cannot open.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/form_widgets.h"

namespace kt::ui {

class SettingsView final : public Widget {
public:
    explicit SettingsView(std::vector<SectionItem> sections);

    SearchField& search() { return *search_; }
    SectionList& sections() { return *list_; }
    ScrollView& scroll() { return *scroll_; }
    Label& title() { return *title_; }

    // Highlights a section and runs on_section.
    void select(size_t i);
    // Replaces the page. keep_scroll: the same page rebuilt (scroll position stays).
    void set_page(std::string title, std::unique_ptr<Widget> content, bool keep_scroll = false);
    Widget* page() const { return scroll_->content(); }
    // The root of its own window: fills it, no shadow, no close button (the window has one).
    void set_windowed(bool);
    bool windowed() const { return windowed_; }
    // A banner over the page ("The config file has 2 problems"); "" hides it.
    void set_banner(std::string text, Tone tone = Tone::warning);
    const std::string& banner() const { return banner_text_; }

    std::function<void(size_t)> on_section;
    std::function<void(const std::string&)> on_search;
    std::function<void()> on_close;

    void paint(PaintContext&) const override;
    Insets paint_outset() const override { return Insets::all(24); }
    EventResult on_key(const KeyEvent&) override;
    std::string describe() const override;

private:
    SearchField* search_ = nullptr;
    SectionList* list_ = nullptr;
    Label* title_ = nullptr;
    Box* banner_ = nullptr;
    Label* banner_label_ = nullptr;
    std::string banner_text_;
    ScrollView* scroll_ = nullptr;
    Widget* close_ = nullptr;
    bool windowed_ = false;
};

// Shows the view as a modal sheet centered over the window (820 x 600 DIP, smaller windows
// get a margin of 16). Escape closes it unless a control uses the key (a key recorder).
// `closed` runs when the sheet goes away for any reason.
WidgetId show_settings(Window&, std::unique_ptr<SettingsView>, std::function<void()> closed = {});

}  // namespace kt::ui
