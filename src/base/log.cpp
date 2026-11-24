#include "base/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace kt::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::warn)};
std::mutex g_mutex;

const char* name(Level l) {
    switch (l) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
        case Level::off: return "off";
    }
    return "?";
}

}  // namespace

void set_level(Level l) { g_level.store(static_cast<int>(l), std::memory_order_relaxed); }
Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

void write(Level l, std::string_view category, std::string_view message) {
    if (static_cast<int>(l) < g_level.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "[%s] %.*s: %.*s\n", name(l), static_cast<int>(category.size()), category.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace kt::log
