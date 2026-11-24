// Errors of the workspace module.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "base/expected.h"

namespace kt::ws {

enum class WsErrc : std::uint16_t {
    NotFound,
    Busy,              // a pane runs a program other than an idle shell; confirm, then force=true
    LastPane,
    InvalidRatio,
    InvalidName,
    ScratchImmutable,  // the scratch and quick workspaces cannot be renamed, colored or removed
    SameProject,
    LayoutParse,
    LayoutNoTabs,
    LayoutNoFile,
    StateCorrupt,
    StateNewerSchema,
    StateReadOnly,
    Io,
    InvalidArgument,
};

std::string_view to_string(WsErrc);

// domain = "workspace"; message is English and never holds terminal text.
struct WsError {
    WsErrc code = WsErrc::NotFound;
    std::string message;
    std::string detail;
};

template <class T>
using WsResult = Expected<T, WsError>;

inline Unexpected<WsError> ws_error(WsErrc code, std::string message, std::string detail = {}) {
    return Unexpected<WsError>(WsError{code, std::move(message), std::move(detail)});
}

}  // namespace kt::ws
