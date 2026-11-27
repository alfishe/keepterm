#include "session/history_events.h"

#include "session/json.h"

namespace kt::session {

std::string resize_event(TermSize s) {
    return "{\"type\":\"resize\",\"cols\":" + std::to_string(s.cols) + ",\"rows\":" + std::to_string(s.rows) + "}";
}

std::optional<TermSize> parse_resize_event(std::string_view payload) {
    auto v = json::parse(payload);
    if (!v) return std::nullopt;
    if (v->str_or("type") != "resize") return std::nullopt;
    const int64_t c = v->int_or("cols", 0), r = v->int_or("rows", 0);
    if (c < 1 || r < 1 || c > 65535 || r > 65535) return std::nullopt;
    return TermSize{static_cast<uint16_t>(c), static_cast<uint16_t>(r)};
}

}  // namespace kt::session
