// Minimal logging until the logging module lands: level filter, stderr sink.
// Log lines never contain terminal text.
#pragma once

#include <string_view>

namespace kt::log {

enum class Level : int { trace, debug, info, warn, error, off };

void set_level(Level);
Level level();
void write(Level, std::string_view category, std::string_view message);

inline void info(std::string_view cat, std::string_view msg) { write(Level::info, cat, msg); }
inline void warn(std::string_view cat, std::string_view msg) { write(Level::warn, cat, msg); }
inline void error(std::string_view cat, std::string_view msg) { write(Level::error, cat, msg); }

}  // namespace kt::log
