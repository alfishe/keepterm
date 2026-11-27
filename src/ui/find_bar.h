// The find bar of a terminal pane: a query field, a regular expression toggle (.*), "3 of 12",
// previous / next and close, shown as a non-modal overlay at the top right of the pane. The
// owner searches.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ui/widgets.h"

namespace kt::ui {

struct FindResult {
    size_t count = 0;
    std::optional<size_t> current;
    std::string error;     // the pattern is not a valid regular expression: why
    std::string partial;   // a limit stopped the search: the matches are not all ("4 of 6+")
};

struct FindBarHooks {
    // Runs the search (a literal, or with `regex` a regular expression); both case-insensitive.
    std::function<FindResult(const std::string&, bool regex)> query;
    std::function<FindResult(int delta)> step;
    std::function<void()> closed;
};

class FindBar final : public Widget {
public:
    explicit FindBar(FindBarHooks hooks, std::string initial = {}, bool regex = false);
    TextField& field() { return *field_; }
    bool regex() const { return regex_; }
    void set_regex(bool);   // searches again
    void step(int delta);
    void close();
    void paint(PaintContext&) const override;
    EventResult on_key(const KeyEvent&) override;
    Insets paint_outset() const override { return Insets::all(16); }
    std::string describe() const override;

private:
    void show(const FindResult& r);
    void query();
    FindBarHooks hooks_;
    TextField* field_ = nullptr;
    Button* regex_btn_ = nullptr;
    Label* count_ = nullptr;
    bool regex_ = false;
    bool closed_ = false;
};

// Opens the bar with its top-right corner at `top_right` (window DIP); returns the overlay id.
WidgetId show_find_bar(Window&, FindBarHooks, Vec2 top_right, std::string initial = {}, bool regex = false);

}  // namespace kt::ui
