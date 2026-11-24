// Minimal PNG writer for headless rendering (`keepterm --pty-render`): 8-bit RGB, stored deflate.
// The test support library has its own reader and writer; the app does not link test code.
#pragma once

#include <string>

#include "base/error.h"
#include "render/types.h"

namespace kt::app {

// Writes the opaque color of every pixel (alpha is dropped).
Result<void> write_png_rgb(const std::string& path, const render::Image&);

}  // namespace kt::app
