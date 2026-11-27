// What the output viewer's terminal shows for a window of a session's stored output
// (session/history_output.h): its lines, a faint note where lines are left out above or below,
// and the prompt mark on each command's line.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/pane_session.h"
#include "session/history_output.h"

namespace kt::app {

struct ViewerText {
    std::vector<PaneSession::StaticLine> lines;
    std::optional<std::size_t> target;   // the line the viewer opens at
};

ViewerText viewer_text(const session::HistoryOutput&);

// "1,234,567".
std::string grouped_number(uint64_t n);

}  // namespace kt::app
