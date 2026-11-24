// Opens a file with the user's default application (Settings → Open in Editor): Launch
// Services through /usr/bin/open on macOS, ShellExecuteW on Windows, xdg-open on Linux. The
// file is created empty first when it does not exist, so the editor has something to open.
#pragma once

#include <filesystem>

#include "base/error.h"

namespace kt::app {

Result<void> open_with_default_app(const std::filesystem::path& file);

}  // namespace kt::app
