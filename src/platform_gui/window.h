// Top-level windows.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "base/error.h"
#include "platform_gui/geometry.h"
#include "platform_gui/input.h"
#include "platform_gui/native_surface.h"

namespace kt::platform {

using Clock = std::chrono::steady_clock;

enum class WindowKind : uint8_t {
    normal,         // main window
    quick_terminal, // borderless, top-most, no taskbar entry, on every desktop
    popup,          // only through create_popup(); later step
    utility,        // settings window
};

struct WindowPlacement {
    DipRect     rect;              // size in DIPs; origin (0, 0) with empty monitor_id = OS default
    std::string monitor_id;        // stable id (later step; ignored for now)
    bool        maximized = false;
    bool        fullscreen = false;
};

// A screen's work area (without the taskbar, Dock, panels) in the coordinates of
// WindowPlacement on that screen, its scale, and a stable key for remembered frames.
struct ScreenArea {
    DipRect     area;
    float       scale = 1.0f;
    std::string key;
};

struct WindowDesc {
    WindowKind      kind = WindowKind::normal;
    std::u8string   title;
    WindowPlacement placement{{0, 0, 800, 500}, {}, false, false};
    DipSize         min_size{320, 200};
    bool            transparent = false;    // per-pixel alpha; later step
    bool            blur_behind = false;    // vibrancy / acrylic; later step
};

enum class HitArea : uint8_t { client, caption, minimize, maximize, close,
                               resize_n, resize_s, resize_e, resize_w,
                               resize_ne, resize_nw, resize_se, resize_sw };

enum class CursorShape : uint8_t { arrow, ibeam, hand, crosshair, wait, move,
                                   resize_ew, resize_ns, resize_nesw, resize_nwse, hidden,
                                   // A window frame's edge or corner (the OS's window resize cursors)
                                   frame_n, frame_s, frame_e, frame_w, frame_ne, frame_nw, frame_se, frame_sw };

class IWindow {
public:
    virtual ~IWindow() = default;

    virtual void show(bool activate) = 0;
    virtual void hide() = 0;
    virtual void close() = 0;                     // destroys; listener gets on_destroyed
    virtual void set_title(std::u8string_view) = 0;
    virtual void set_placement(const WindowPlacement&) = 0;
    // Places the window at `r`, given in the coordinates of `screen.area` (a quick terminal
    // that opens on another monitor than the last time).
    virtual void place_on(const ScreenArea& screen, DipRect r) {
        (void)screen;
        WindowPlacement pl;
        pl.rect = r;
        set_placement(pl);
    }
    virtual WindowPlacement placement() const = 0;
    virtual void set_fullscreen(bool) = 0;
    virtual void minimize() = 0;
    virtual void toggle_maximize() = 0;
    virtual void set_cursor(CursorShape) = 0;
    // Custom title bar hit-testing. Stored; used once the toolkit draws the frame.
    virtual void set_hit_test(std::function<HitArea(DipPoint)>) = 0;

    virtual float   scale() const = 0;            // physical px per DIP: 1.0, 1.25, 1.5, 2.0 ...
    virtual PxSize  client_size_px() const = 0;
    virtual DipSize client_size() const = 0;
    virtual bool    focused() const = 0;

    // The render module calls these. A surface is a child of the window so that a backend
    // switch never destroys the top-level window.
    virtual Result<NativeSurface> create_surface(const SurfaceRequest&) = 0;
    virtual void                  destroy_surface() = 0;
    virtual void                  request_frame() = 0;   // calls on_frame once, paced

    // IME: where the caret is, so that candidate windows follow it.
    virtual void set_ime_enabled(bool) = 0;
    virtual void set_ime_cursor_rect(DipRect caret) = 0;

    virtual void set_secure_input(bool) = 0;
    // Asks for the user's attention while the window is in the background: the Dock icon
    // bounces (macOS), the taskbar button flashes (Windows), the urgency state is set (X11).
    // It ends when the window gets the focus.
    virtual void request_attention() {}      // macOS EnableSecureEventInput; no-op elsewhere

    // Called from inside on_key: the keymap took the key, so the platform suppresses the
    // matching on_text.
    virtual void consume_key() = 0;
};

class IWindowListener {
public:
    virtual ~IWindowListener() = default;
    virtual void on_close_requested() = 0;
    virtual void on_destroyed() {}
    virtual void on_resized(PxSize px, DipSize dip) = 0;
    virtual void on_scale_changed(float scale, DipRect suggested) = 0;
    virtual void on_focus(bool focused) = 0;
    // The user moves or sizes the window (a system size-move loop, or a frame drag from the
    // hit test): focus changes in between are not the user going elsewhere.
    virtual void on_move_size(bool /*active*/) {}
    virtual void on_occluded(bool /*occluded*/) {}  // minimized or fully covered: stop rendering
    // The window system dropped the surface contents (X11 Expose without backing store,
    // WM_PAINT on Win32): the next frame must have full damage.
    virtual void on_expose() {}
    virtual void on_frame(Clock::time_point target_present) = 0;
    virtual void on_key(const KeyEvent&) = 0;
    virtual void on_text(std::u8string_view committed) = 0;
    virtual void on_composition(const Composition&) = 0;
    virtual void on_mouse(const MouseEvent&) = 0;
    virtual void on_scroll(const ScrollEvent&) = 0;
    virtual void on_drop(const DropEvent&) {}
    virtual DropEffect on_drag_over(const DragOverEvent&) { return DropEffect::copy; }
};

}  // namespace kt::platform
