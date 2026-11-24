// Linux backend selection: Wayland when WAYLAND_DISPLAY is set and the
// Wayland backend is usable, else X11. --ws=x11 (PlatformOptions::force_ws) skips Wayland.
#include <cstdlib>

#include "base/log.h"
#include "platform_gui/platform.h"

namespace kt::platform {

namespace wayland {
Result<std::unique_ptr<IPlatform>> create_wayland_platform(const PlatformOptions&);
}
namespace x11 {
Result<std::unique_ptr<IPlatform>> create_x11_platform(const PlatformOptions&);
}

namespace {

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

}  // namespace

bool display_available() { return env_set("DISPLAY") || env_set("WAYLAND_DISPLAY"); }

Result<std::unique_ptr<IPlatform>> create_platform(const PlatformOptions& opts) {
    const bool want_wayland = opts.force_ws ? *opts.force_ws == WindowSystem::wayland : env_set("WAYLAND_DISPLAY");
    if (opts.force_ws && *opts.force_ws != WindowSystem::wayland && *opts.force_ws != WindowSystem::x11) {
        return make_error(Errc::invalid_argument, "only x11 and wayland exist on Linux");
    }
    if (want_wayland) {
        auto w = wayland::create_wayland_platform(opts);
        if (w) return w;
        if (opts.force_ws) return w;
        log::info("platform", "Wayland backend unavailable (" + w.error().what + "); using X11");
    }
    if (!env_set("DISPLAY")) return make_error(Errc::unavailable, "no X display (DISPLAY is not set)");
    return x11::create_x11_platform(opts);
}

}  // namespace kt::platform
