// The platform object and its factory.
//
// This is the first subset: event loop, windows, clipboard. Popups, monitors, notifications,
// shell integration, the session monitor and the system theme are later steps.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/error.h"
#include "platform_gui/clipboard.h"
#include "platform_gui/event_loop.h"
#include "platform_gui/input.h"
#include "platform_gui/native_surface.h"
#include "platform_gui/window.h"

namespace kt::platform {

struct PlatformInfo {
    WindowSystem ws = WindowSystem::win32;
    uint32_t     os_build = 0;      // Windows build (14393, 17763, ...); 0 elsewhere
    std::string  os_version;        // "10.0.14393", "14.6", kernel release on Linux
    bool         server_core = false;
    bool         elevated = false;
};

// macos.option_as_alt: which Option keys act as Meta instead of composing characters.
enum class OptionAsAlt : uint8_t { none, left, right, both };

struct PlatformOptions {
    std::string app_id = "keepterm";       // AUMID, bundle id, Wayland app_id, WM_CLASS
    std::string profile;                  // --profile; changes app_id suffix
    std::optional<WindowSystem> force_ws; // --ws=x11 for tests
    OptionAsAlt option_as_alt = OptionAsAlt::none;
};

// A system-wide hotkey: a physical key, named after the US layout, plus
// mod_ctrl / mod_shift / mod_alt / mod_super.
struct GlobalHotkey {
    Key      key = Key::unknown;
    uint16_t mods = 0;
};
using HotkeyId = uint32_t;

// Which screen a panel opens on (quick_terminal.screen).
enum class ScreenPick : uint8_t { mouse, active, primary };

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual const PlatformInfo& info() const = 0;
    virtual IEventLoop&         loop() = 0;

    virtual Result<std::unique_ptr<IWindow>> create_window(const WindowDesc&, IWindowListener&) = 0;

    virtual IClipboard& clipboard() = 0;

    // The callback runs on the UI thread. Errors: Errc::busy (another app or the OS holds the
    // chord), Errc::not_supported (no mechanism on this desktop), Errc::unavailable.
    virtual Result<HotkeyId> register_global_hotkey(GlobalHotkey, std::function<void()>) {
        return make_error(Errc::not_supported, "global hotkeys are not supported here");
    }
    virtual void unregister_global_hotkey(HotkeyId) {}

    virtual std::optional<ScreenArea> screen_area(ScreenPick) const { return std::nullopt; }

    // The window or app in front now, as an opaque token (0: unknown), and a later request to
    // give it the focus back (the quick terminal's hide).
    virtual uint64_t foreground_token() const { return 0; }
    virtual void restore_foreground(uint64_t) {}
};

// One definition per OS. On Linux: Wayland if WAYLAND_DISPLAY is set and the Wayland backend
// is usable, else X11 (the Wayland backend is a stub for now, so this is X11 / Xwayland).
// Returns Errc::unavailable when there is no display (no DISPLAY, no window server session).
Result<std::unique_ptr<IPlatform>> create_platform(const PlatformOptions& = {});

// True when a window system session is reachable, without opening windows: DISPLAY or
// WAYLAND_DISPLAY on Linux, a GUI login session on macOS, an interactive window station on
// Windows. Tests use it to skip.
bool display_available();

}  // namespace kt::platform
