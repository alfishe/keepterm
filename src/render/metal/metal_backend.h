// Metal backend: macOS 13+, Metal.framework linked weakly.
#pragma once

#include <memory>

#include "render/backend_factory.h"

namespace kt::render::metal {

// Errc::unavailable when Metal or a Metal device is missing.
Result<std::unique_ptr<IRenderer>> create(const BackendOptions&);

}  // namespace kt::render::metal
