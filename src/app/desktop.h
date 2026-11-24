// Desktop integration the platform layer does not cover: open a URL or a folder with the
// user's default application, and draw the user's attention to the window (agent attention,
// OSC 9 / 777 notifications) when it is not focused.
//
// macOS: NSWorkspace, NSApp requestUserAttention and the Dock badge (desktop_mac.mm).
// Windows: ShellExecuteW and FlashWindowEx on the top-level window of the surface.
// Linux: xdg-open and notify-send, started detached; nothing when they are not installed.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace kt::app::desktop {

// Only http, https, ftp, mailto, ssh, file and git URLs are opened (a terminal link is untrusted
// text). False when the scheme is refused or nothing could be started.
bool open_url(const std::string& url);
// Opens a folder in the file manager.
bool open_folder(const std::string& path);
// Opens a file with its default application.
bool open_file(const std::string& path);
// Starts a program detached (a windowed $EDITOR); argv[0] is looked up on PATH. False when it
// could not be started (POSIX: when fork fails; the exec result is not waited for).
bool run_detached(const std::vector<std::string>& argv);
bool url_allowed(const std::string& url);
// The Dock / taskbar badge: the number of panes that need attention, 0 clears it (macOS only).
void set_badge(int count);

// The system folder chooser, starting in `initial`. `done` runs on the UI thread with the chosen
// folder (UTF-8, native separators), not at all on cancel. `post` runs a function on the UI
// thread (the Linux chooser is a helper process waited for on a worker).
// macOS: NSOpenPanel as a sheet of the window. Windows: IFileOpenDialog with FOS_PICKFOLDERS
// (modal). Linux: zenity or kdialog when installed (the portal needs D-Bus, not linked).
// False when there is no chooser: the caller asks for a typed path instead.
using Post = std::function<void(std::function<void()>)>;
bool pick_folder(void* native_view, const std::string& title, const std::string& initial,
                 std::function<void(std::string)> done, const Post& post);

}  // namespace kt::app::desktop
