// The records of the history store's events kind: one JSON object each, at the
// output stream offset where it happened. The session host stores the terminal's size when its
// writer starts and every resize after, so a replay of the raw kind (an asciicast export) runs at
// the sizes the programs drew for.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kt::session {

struct TermSize {
    uint16_t cols = 0, rows = 0;
    bool operator==(const TermSize&) const = default;
};

// {"type":"resize","cols":C,"rows":R}
std::string resize_event(TermSize s);
// The size of a resize event; none for another event or a malformed one.
std::optional<TermSize> parse_resize_event(std::string_view payload);

}  // namespace kt::session
