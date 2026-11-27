#include "app/output_viewer_text.h"

#include "ui/widget.h"

namespace kt::app {

std::string grouped_number(uint64_t n) {
    std::string s = std::to_string(n);
    for (std::size_t i = s.size(); i > 3; i -= 3) s.insert(i - 3, ",");
    return s;
}

ViewerText viewer_text(const session::HistoryOutput& o) {
    ViewerText v;
    v.lines.reserve(o.lines.size() + 2);
    auto note = [&](uint64_t n, std::string_view one, std::string_view many) {
        v.lines.push_back({"\xE2\x94\x80\xE2\x94\x80 " + (n == 1 ? std::string(ui::tr(one)) : grouped_number(n) + std::string(ui::tr(many))) +
                               " \xE2\x94\x80\xE2\x94\x80",
                           false, true});
    };
    if (o.lines_before) note(o.lines_before, "1 earlier line is not shown", " earlier lines are not shown");
    const std::size_t first = v.lines.size();
    for (const auto& l : o.lines) v.lines.push_back({l.text, false, false});
    for (const auto& c : o.commands) {
        if (c.line && *c.line < o.lines.size()) v.lines[first + *c.line].prompt = true;
    }
    if (o.lines_after) note(o.lines_after, "1 later line is not shown", " later lines are not shown");
    if (o.target && *o.target < o.lines.size()) v.target = first + *o.target;
    return v;
}

}  // namespace kt::app
