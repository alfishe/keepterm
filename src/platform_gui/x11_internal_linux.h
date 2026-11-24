// Internal declarations shared by the X11 files.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "platform_gui/loop_parts.h"
#include "platform_gui/platform.h"
#include "platform_gui/x11_api_linux.h"

namespace kt::platform::x11 {

class X11Window;
class X11Platform;

struct Atoms {
    Atom clipboard, primary, utf8_string, targets, text, string, incr, kt_selection, atom, cardinal;
    Atom wm_protocols, wm_delete_window, net_wm_ping, net_wm_name, net_wm_pid, net_wm_icon, net_wm_state,
        net_wm_state_fullscreen, net_wm_state_demands_attention, net_wm_state_max_vert, net_wm_state_max_horz, net_wm_window_type,
        net_wm_window_type_normal, resource_manager;
    // The quick terminal and screens.
    Atom net_wm_state_above, net_wm_state_sticky, net_wm_state_skip_taskbar, net_wm_state_skip_pager, motif_wm_hints,
        net_active_window, net_workarea, net_current_desktop;
    // XDND (drag and drop, version 5).
    Atom xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave, xdnd_drop, xdnd_finished,
        xdnd_selection, xdnd_type_list, xdnd_action_copy, uri_list, text_plain_utf8, kt_xdnd, image_png, kt_image;
};

// The poll() loop: X connection fd, an eventfd (pipe off Linux) for post(), waitable fds.
class X11Loop final : public IEventLoop {
public:
    explicit X11Loop(X11Platform& p);
    ~X11Loop() override;
    bool init();

    int  run() override;
    void quit(int exit_code) override;
    void post(std::function<void()> task) override;
    TimerId add_timer(Clock::time_point deadline, std::function<void()> fn) override;
    void    cancel_timer(TimerId) override;
    Result<void> add_waitable(Waitable, std::function<void()> on_signalled) override;
    void         remove_waitable(Waitable) override;

private:
    void drain_wake_fd();

    X11Platform& p_;
    detail::TaskQueue tasks_;
    detail::TimerHeap timers_;
    int wake_read_ = -1, wake_write_ = -1;
    struct Waiter {
        int fd;
        std::shared_ptr<std::function<void()>> fn;
    };
    std::vector<Waiter> waiters_;
    bool quit_ = false;
    int exit_code_ = 0;
};

class X11Clipboard final : public IClipboard {
public:
    explicit X11Clipboard(X11Platform& p) : p_(p) {}
    ~X11Clipboard() override;
    bool init();

    void read_text(Selection, std::function<void(Result<std::u8string>)>) override;
    Result<void> write_text(Selection, std::u8string_view text, bool sensitive) override;
    bool has_primary() const override { return true; }
    void read_image(std::function<void(Result<std::vector<uint8_t>>)>) override;

    void shutdown();
    Window window() const { return win_; }
    void handle(const XEvent&);   // events for the helper window

private:
    // Slots: 0 CLIPBOARD text, 1 PRIMARY text, 2 CLIPBOARD image/png (its own property, so it
    // can run beside a text read). Waiters get the raw bytes (text already UTF-8).
    struct Read {
        std::vector<std::function<void(Result<std::string>)>> waiters;
        bool active = false;
        bool incr = false;
        Atom target = None;
        std::string data;
        TimerId timeout = 0;
    };
    Atom sel_atom(Selection s) const;
    int index_of(Atom selection) const;
    void start_read(int idx, Atom target);
    void finish(int idx, Result<std::string> r);
    void finish_data(int idx, std::string data);
    void on_selection_notify(const XSelectionEvent&);
    void on_property_new(const XPropertyEvent&);
    void on_request(const XSelectionRequestEvent&);
    bool read_property(Atom prop, std::string& out, Atom& type, bool& too_big);
    Atom prop_of(int idx) const;

    X11Platform& p_;
    Window win_ = 0;
    std::array<std::u8string, 2> owned_;
    std::array<bool, 2> owns_{false, false};
    std::array<Read, 3> reads_;
};

class X11Platform final : public IPlatform {
public:
    X11Platform(const PlatformOptions&, const Api*);
    ~X11Platform() override;
    Result<void> init();

    const PlatformInfo& info() const override { return info_; }
    IEventLoop& loop() override { return loop_; }
    Result<std::unique_ptr<IWindow>> create_window(const WindowDesc&, IWindowListener&) override;
    IClipboard& clipboard() override { return clipboard_; }
    Result<HotkeyId> register_global_hotkey(GlobalHotkey, std::function<void()>) override;
    void unregister_global_hotkey(HotkeyId) override;
    std::optional<ScreenArea> screen_area(ScreenPick) const override;
    uint64_t foreground_token() const override;
    void restore_foreground(uint64_t) override;
    // Asks the window manager to activate `w` (_NET_ACTIVE_WINDOW), or sets the focus when
    // there is none.
    void activate(Window w, bool as_pager);

    // Shared state for windows and the clipboard.
    const Api& x() const { return *x_; }
    Display* dpy() const { return dpy_; }
    int screen() const { return screen_; }
    Window root() const { return root_; }
    const Atoms& atoms() const { return atoms_; }
    XIM im() const { return im_; }
    float scale() const { return scale_; }
    Time last_time() const { return last_time_; }
    void note_time(Time t) {
        if (t != CurrentTime) last_time_ = t;
    }
    bool detectable_repeat() const { return detectable_repeat_; }
    const std::string& app_id() const { return app_id_; }
    Cursor cursor(CursorShape);
    X11Loop& x11_loop() { return loop_; }

    void register_window(Window, X11Window*);
    void unregister_window(Window);
    void dispatch_pending();    // reads and handles every queued X event

private:
    void handle(XEvent&);
    void reload_scale(bool notify);
    std::string read_resource_manager();

    PlatformInfo info_;
    std::string app_id_;
    const Api* x_;
    Display* dpy_ = nullptr;
    int screen_ = 0;
    Window root_ = 0;
    Atoms atoms_{};
    XIM im_ = nullptr;
    float scale_ = 1;
    Time last_time_ = CurrentTime;
    bool detectable_repeat_ = false;
    std::unordered_map<Window, X11Window*> windows_;
    struct Grab {
        int keycode = 0;
        unsigned mods = 0;
        bool down = false;   // auto-repeat presses until the release are ignored
        std::function<void()> fn;
    };
    std::map<HotkeyId, Grab> grabs_;
    HotkeyId next_grab_ = 1;
    bool handle_grab(const XKeyEvent&);
    std::array<Cursor, 19> cursors_{};
    X11Loop loop_;
    X11Clipboard clipboard_;
};

Result<std::unique_ptr<IWindow>> create_x11_window(X11Platform&, const WindowDesc&, IWindowListener&);

// Called by X11Platform::handle for windows found in the registry.
void dispatch_to_window(X11Window&, XEvent&);
void window_scale_changed(X11Window&, float scale);

}  // namespace kt::platform::x11
