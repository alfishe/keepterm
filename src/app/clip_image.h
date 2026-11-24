// Pasted clipboard images (an image in the clipboard pastes as the path of a saved PNG, which
// agents such as Claude Code attach).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/error.h"

namespace kt::app {

// Saves `png` as `<dir>/clip-<stamp>-<n>.png`: the folder is private (0700), the file 0600.
// Prunes the folder first: files older than `max_age_days`, and the oldest above `max_files`.
// Returns the file's path.
Result<std::filesystem::path> save_clip_image(const std::filesystem::path& dir, const std::vector<uint8_t>& png,
                                              int max_age_days = 7, size_t max_files = 100);

}  // namespace kt::app
