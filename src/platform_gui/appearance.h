// OS appearance, light or dark (theme.mode = "system").
//
//   Windows  AppsUseLightTheme under
//            HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize; changes
//            through RegNotifyChangeKeyValue on that key (the value that WM_SETTINGCHANGE
//            "ImmersiveColorSet" announces). Missing value (older builds): light.
//   macOS    NSApp.effectiveAppearance (Aqua or Dark Aqua), observed with KVO; before NSApp
//            exists, the AppleInterfaceStyle user default.
//   Linux    org.freedesktop.appearance color-scheme through the settings portal (libdbus-1 is
//            loaded at run time), else GTK_THEME / gtk-3.0 and gtk-4.0 settings.ini
//            (gtk-application-prefer-dark-theme, a theme name ending in "-dark"). Polled every
//            2 s while watched: the portal's SettingChanged signal would need a D-Bus main loop.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "platform_gui/event_loop.h"

namespace kt::platform {

enum class Appearance : uint8_t { light, dark };

// The current OS appearance. UI thread on macOS; any thread elsewhere.
Appearance system_appearance();

// Calls `on_change` on the loop's thread when the OS appearance changes. Destroy the watch to
// stop. Never null: where the OS offers no change signal it polls.
class AppearanceWatch {
public:
    virtual ~AppearanceWatch() = default;
    virtual Appearance current() const = 0;

protected:
    AppearanceWatch() = default;
};
std::unique_ptr<AppearanceWatch> watch_appearance(IEventLoop& loop, std::function<void(Appearance)> on_change);

}  // namespace kt::platform
