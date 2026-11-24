// The find bar of a terminal pane: a query field, "3 of 12", previous / next
// and close, shown as a non-modal overlay at the top right of the pane. The owner searches.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ui/widgets.h"

namespace kt::ui {

struct FindBarHooks {
    // Runs the search; returns the match count and the current match.
    std::function<std::pair<size_t, std::optional<size_t>>(const std::string&)> query;
    std::function<std::pair<size_t, std::optional<size_t>>(int delta)> step;
    std::function<void()> closed;
};

class FindBar final : public Widget {
public:
    explicit FindBar(FindBarHooks hooks, std::string initial = {});
    TextField& field() { return *field_; }
    void step(int delta);
    void close();
    void paint(PaintContext&) const override;
    EventResult on_key(const KeyEvent&) override;
    Insets paint_outset() const override { return Insets::all(16); }
    std::string describe() const override;

private:
    void show(std::pair<size_t, std::optional<size_t>> r);
    FindBarHooks hooks_;
    TextField* field_ = nullptr;
    Label* count_ = nullptr;
    bool closed_ = false;
};

// Opens the bar with its top-right corner at `top_right` (window DIP); returns the overlay id.
WidgetId show_find_bar(Window&, FindBarHooks, Vec2 top_right, std::string initial = {});

}  // namespace kt::ui
