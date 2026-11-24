// Wayland backend: not implemented yet. create_platform() falls back to X11,
// which runs under Xwayland on Wayland sessions. The native backend (libwayland-client,
// xdg-shell, wl_shm, text-input-v3, fractional-scale-v1, data-device) replaces this stub.
#include "platform_gui/platform.h"

namespace kt::platform::wayland {

Result<std::unique_ptr<IPlatform>> create_wayland_platform(const PlatformOptions&) {
    return make_error(Errc::not_supported, "the Wayland backend is not implemented yet; use X11 (Xwayland)");
}

}  // namespace kt::platform::wayland
