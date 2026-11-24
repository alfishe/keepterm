// macOS pieces of the main window that need AppKit (mac_window.mm): the unified title bar
// (full-size content view, transparent title bar, the window's own title
// bar drawn by the toolkit), the window drag from that title bar and the menu bar (NSMenu).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kt::app::mac {

// `ns_view` is the NSView* of the window's surface (platform::NativeSurface::handle).
void use_unified_title_bar(void* ns_view);
// Called from a mouse-down on the toolkit's title bar: the system moves the window.
void drag_window(void* ns_view);
// A double-click on the title bar: the user's choice (zoom, minimize or nothing).
void title_bar_double_click(void* ns_view);

struct MenuEntry {
    std::string title;
    std::string command;               // command id ("tab.new"); empty for submenus and separators
    std::string key;                   // key equivalent ("t", "\\", "="); empty for none
    bool cmd = false, shift = false, alt = false, ctrl = false;
    bool separator = false;
    std::vector<MenuEntry> submenu;
    static MenuEntry sep() {
        MenuEntry e;
        e.separator = true;
        return e;
    }
};

// Replaces the menu bar: the application menu (About, Settings, Hide, Quit) plus `menus`.
// `invoke` runs a command id on the UI thread; `enabled` asks before a menu opens.
void install_menu_bar(const std::string& app_name, const std::vector<MenuEntry>& menus,
                      std::function<void(const std::string&)> invoke, std::function<bool(const std::string&)> enabled);

}  // namespace kt::app::mac
