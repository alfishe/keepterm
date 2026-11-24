// Win32 windows: native frame, render child window, per-monitor DPI, keyboard with AltGr
// detection, IMM32, mouse, wheel, frame pacing.
#include <windows.h>
#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "platform_gui/dpi.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/utf.h"
#include "platform_gui/win_internal.h"

namespace kt::platform::win {

namespace {

constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr DWORD kExStyle = WS_EX_APPWINDOW;
constexpr UINT kToUnicodeKeepState = 0x4;   // ToUnicodeEx: do not change keyboard state (1607+)

int16_t lo16(LPARAM l) { return static_cast<int16_t>(static_cast<uint32_t>(l) & 0xFFFF); }
int16_t hi16(LPARAM l) { return static_cast<int16_t>((static_cast<uint32_t>(l) >> 16) & 0xFFFF); }
int16_t hi16w(WPARAM w) { return static_cast<int16_t>((static_cast<uint64_t>(w) >> 16) & 0xFFFF); }

bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

std::u8string text_of(std::u16string_view u) {
    std::u8string out;
    std::u8string all = detail::utf16_to_utf8(u);
    std::u8string_view v = all;
    while (!v.empty()) {
        size_t n = 0;
        const char32_t c = detail::utf8_first(v, &n);
        v.remove_prefix(n);
        if (detail::is_text_char(c)) detail::utf8_append(out, c);
    }
    return out;
}

UINT wheel_setting(UINT what, UINT fallback) {
    UINT v = fallback;
    if (!SystemParametersInfoW(what, 0, &v, 0)) return fallback;
    if (v == WHEEL_PAGESCROLL) return fallback;   // "one screen per notch": approximated
    return v;
}

}  // namespace

class WinWindow final : public IWindow {
public:
    WinWindow(WinPlatform& p, IWindowListener& l) : p_(p), l_(l) {}

    ~WinWindow() override {
        notify_destroy_ = false;
        if (hwnd_) DestroyWindow(hwnd_);
    }

    Result<void> init(const WindowDesc& d) {
        min_size_ = d.min_size;
        quick_ = d.kind == WindowKind::quick_terminal;
        const std::u16string title = detail::utf8_to_utf16(d.title);
        // The quick terminal: no frame, above other windows, no taskbar button.
        const DWORD style = quick_ ? (WS_POPUP | WS_CLIPCHILDREN) : kStyle;
        const DWORD ex = quick_ ? (WS_EX_TOOLWINDOW | WS_EX_TOPMOST) : kExStyle;
        hwnd_ = CreateWindowExW(ex, kWindowClass, reinterpret_cast<const wchar_t*>(title.c_str()), style,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                                p_.instance(), this);
        if (!hwnd_) return last_os_error("CreateWindowExW");
        accept_files(hwnd_);
        dpi_ = dpi_of(hwnd_);
        scale_ = detail::scale_from_dpi(dpi_);
        clicks_ = detail::ClickCounter(GetDoubleClickTime(),
                                       static_cast<float>(GetSystemMetrics(SM_CXDOUBLECLK)) / 2.0f / scale_);
        wheel_lines_ = wheel_setting(SPI_GETWHEELSCROLLLINES, 3);
        wheel_chars_ = wheel_setting(SPI_GETWHEELSCROLLCHARS, 3);
        update_refresh();

        // Size the client area in DIPs at the window's monitor DPI.
        const PxSize client = detail::to_px(DipSize{d.placement.rect.w, d.placement.rect.h}, scale_);
        RECT rc{0, 0, std::max(client.w, 1), std::max(client.h, 1)};
        adjust(rc, dpi_);
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        int x = 0, y = 0;
        if (d.placement.rect.x != 0 || d.placement.rect.y != 0) {
            x = detail::dip_to_px(d.placement.rect.x, scale_);
            y = detail::dip_to_px(d.placement.rect.y, scale_);
        } else {
            flags |= SWP_NOMOVE;
        }
        SetWindowPos(hwnd_, nullptr, x, y, rc.right - rc.left, rc.bottom - rc.top, flags);
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        px_ = PxSize{cr.right, cr.bottom};
        if (d.placement.maximized) start_maximized_ = true;
        if (d.placement.fullscreen) start_fullscreen_ = true;
        ready_ = true;
        return {};
    }

    // ------------------------------------------------------------ IWindow
    void show(bool activate) override {
        if (!hwnd_) return;
        ShowWindow(hwnd_, start_maximized_ ? SW_SHOWMAXIMIZED : (activate ? SW_SHOW : SW_SHOWNOACTIVATE));
        start_maximized_ = false;
        if (start_fullscreen_) {
            start_fullscreen_ = false;
            set_fullscreen(true);
        }
        if (activate && !SetForegroundWindow(hwnd_) && GetForegroundWindow() != hwnd_) raise_without_focus();
        sync_focus();
    }

    // A process started from a background task (a scheduled task, a service) may not take the
    // foreground: Windows refuses SetForegroundWindow and the new window opens behind the active
    // one. Put it on top of the z-order without activating it, and flash its taskbar button.
    void raise_without_focus() {
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, flags);
        if (!quick_) SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = hwnd_;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        FlashWindowEx(&fi);
    }

    void sync_focus() {
        const bool f = hwnd_ && has_keyboard_ && GetForegroundWindow() == hwnd_;
        if (f == focused_) return;
        focused_ = f;
        if (f) position_ime();
        l_.on_focus(f);
    }

    void hide() override {
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }

    void close() override {
        if (hwnd_) DestroyWindow(hwnd_);   // WM_NCDESTROY calls on_destroyed
    }

    void set_title(std::u8string_view t) override {
        if (!hwnd_) return;
        const std::u16string u = detail::utf8_to_utf16(t);
        SetWindowTextW(hwnd_, reinterpret_cast<const wchar_t*>(u.c_str()));
    }

    void set_placement(const WindowPlacement& pl) override {
        if (!hwnd_) return;
        if (pl.fullscreen != fullscreen_) set_fullscreen(pl.fullscreen);
        if (fullscreen_) return;
        const PxSize client = detail::to_px(DipSize{pl.rect.w, pl.rect.h}, scale_);
        RECT rc{0, 0, std::max(client.w, 1), std::max(client.h, 1)};
        adjust(rc, dpi_);
        SetWindowPos(hwnd_, nullptr, detail::dip_to_px(pl.rect.x, scale_), detail::dip_to_px(pl.rect.y, scale_),
                     rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (pl.maximized && !IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_MAXIMIZE);
    }

    void place_on(const ScreenArea& s, DipRect r) override {
        if (!hwnd_) return;
        const float k = s.scale > 0 ? s.scale : scale_;
        RECT pr{detail::dip_to_px(r.x, k), detail::dip_to_px(r.y, k), 0, 0};
        pr.right = pr.left + std::max(detail::dip_to_px(r.w, k), 1);
        pr.bottom = pr.top + std::max(detail::dip_to_px(r.h, k), 1);
        pinned_ = pr;   // a DPI change on the way keeps this rectangle (on_dpi_changed)
        SetWindowPos(hwnd_, quick_ ? HWND_TOPMOST : nullptr, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top,
                     SWP_NOACTIVATE | (quick_ ? 0u : static_cast<UINT>(SWP_NOZORDER)));
        pinned_.reset();
    }

    WindowPlacement placement() const override {
        WindowPlacement pl;
        if (!hwnd_) return pl;
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        pl.rect = detail::to_dip(wr.left, wr.top, px_.w, px_.h, scale_);
        pl.maximized = IsZoomed(hwnd_) != FALSE;
        pl.fullscreen = fullscreen_;
        return pl;
    }

    void set_fullscreen(bool on) override {
        if (!hwnd_ || on == fullscreen_) return;
        if (on) {
            saved_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
            saved_wp_.length = sizeof saved_wp_;
            GetWindowPlacement(hwnd_, &saved_wp_);
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
            fullscreen_ = true;
            SetWindowLongW(hwnd_, GWL_STYLE,
                           static_cast<LONG>((static_cast<DWORD>(saved_style_) & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW)) | static_cast<DWORD>(WS_POPUP)));
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            fullscreen_ = false;
            SetWindowLongW(hwnd_, GWL_STYLE, saved_style_);
            SetWindowPlacement(hwnd_, &saved_wp_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    void minimize() override {
        if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
    }

    void toggle_maximize() override {
        if (hwnd_) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    }

    void set_cursor(CursorShape s) override {
        const wchar_t* id = IDC_ARROW;
        switch (s) {
            case CursorShape::arrow: id = IDC_ARROW; break;
            case CursorShape::ibeam: id = IDC_IBEAM; break;
            case CursorShape::hand: id = IDC_HAND; break;
            case CursorShape::crosshair: id = IDC_CROSS; break;
            case CursorShape::wait: id = IDC_WAIT; break;
            case CursorShape::move: id = IDC_SIZEALL; break;
            case CursorShape::resize_ew: id = IDC_SIZEWE; break;
            case CursorShape::resize_ns: id = IDC_SIZENS; break;
            case CursorShape::resize_nesw: id = IDC_SIZENESW; break;
            case CursorShape::resize_nwse:
            case CursorShape::frame_nw:
            case CursorShape::frame_se: id = IDC_SIZENWSE; break;
            case CursorShape::frame_ne:
            case CursorShape::frame_sw: id = IDC_SIZENESW; break;
            case CursorShape::frame_e:
            case CursorShape::frame_w: id = IDC_SIZEWE; break;
            case CursorShape::frame_n:
            case CursorShape::frame_s: id = IDC_SIZENS; break;
            case CursorShape::hidden: id = nullptr; break;
        }
        cursor_ = id ? LoadCursorW(nullptr, id) : nullptr;
        if (hovering_) SetCursor(cursor_);
    }

    // The native title bar is used for now; the custom frame (WM_NCCALCSIZE, WM_NCHITTEST
    // through this callback) is a later step.
    void set_hit_test(std::function<HitArea(DipPoint)> fn) override { hit_test_ = std::move(fn); }

    float scale() const override { return scale_; }
    PxSize client_size_px() const override { return px_; }
    DipSize client_size() const override { return detail::to_dip(px_, scale_); }
    bool focused() const override { return focused_; }
    void request_attention() override {
        if (!hwnd_ || GetForegroundWindow() == hwnd_) return;
        FLASHWINFO f{};
        f.cbSize = sizeof f;
        f.hwnd = hwnd_;
        f.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;   // until the window comes to the front
        FlashWindowEx(&f);
    }

    Result<NativeSurface> create_surface(const SurfaceRequest&) override {
        if (!hwnd_) return make_error(Errc::unavailable, "window is closed");
        destroy_surface();
        child_ = CreateWindowExW(0, kSurfaceClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                 std::max(px_.w, 1), std::max(px_.h, 1), hwnd_, nullptr, p_.instance(), this);
        if (!child_) return last_os_error("CreateWindowExW for the render surface");
        accept_files(child_);   // the surface covers the client area; it forwards WM_DROPFILES
        NativeSurface s;
        s.ws = WindowSystem::win32;
        s.handle = child_;
        return s;
    }

    void destroy_surface() override {
        if (!child_) return;
        HWND c = child_;
        child_ = nullptr;
        DestroyWindow(c);
    }

    void request_frame() override {
        if (!hwnd_ || frame_pending_) return;
        frame_pending_ = true;
        schedule_frame();
    }

    void set_ime_enabled(bool on) override {
        ime_enabled_ = on;
        if (hwnd_) ImmAssociateContextEx(hwnd_, nullptr, on ? IACE_DEFAULT : 0);
    }

    void set_ime_cursor_rect(DipRect r) override {
        caret_ = r;
        position_ime();
    }

    void set_secure_input(bool) override {}

    void consume_key() override { consumed_ = true; }

    // ------------------------------------------------------------ messages
    LRESULT handle(UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
            case WM_CLOSE: l_.on_close_requested(); return 0;
            case WM_DROPFILES: on_drop_files(reinterpret_cast<HDROP>(w), hwnd_); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                EndPaint(hwnd_, &ps);
                if (ready_) l_.on_expose();
                return 0;
            }
            case WM_SIZE: on_size(w, l); return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(l);
                RECT rc{0, 0, detail::dip_to_px(min_size_.w, scale_), detail::dip_to_px(min_size_.h, scale_)};
                adjust(rc, dpi_);
                mm->ptMinTrackSize.x = rc.right - rc.left;
                mm->ptMinTrackSize.y = rc.bottom - rc.top;
                return 0;
            }
            case WM_ENTERSIZEMOVE:
                l_.on_move_size(true);
                [[fallthrough]];
            case WM_ENTERMENULOOP:
                in_modal_ = true;
                p_.win_loop().enter_modal();
                return 0;
            case WM_EXITSIZEMOVE:
            case WM_EXITMENULOOP:
                in_modal_ = false;
                p_.win_loop().exit_modal();
                update_refresh();
                if (msg == WM_EXITSIZEMOVE) l_.on_move_size(false);
                return 0;
            case WM_DPICHANGED: on_dpi_changed(static_cast<UINT>(w & 0xFFFF), *reinterpret_cast<RECT*>(l)); return 0;
            case kWmGetDpiScaledSize: return on_get_dpi_scaled_size(static_cast<UINT>(w), reinterpret_cast<SIZE*>(l));
            case WM_DISPLAYCHANGE: update_refresh(); break;
            case WM_SETTINGCHANGE:
                wheel_lines_ = wheel_setting(SPI_GETWHEELSCROLLLINES, 3);
                wheel_chars_ = wheel_setting(SPI_GETWHEELSCROLLCHARS, 3);
                break;
            // Focus as the user sees it: the keyboard focus of our thread AND the foreground
            // window. A window shown while another app keeps the foreground (background task,
            // focus-steal prevention) still gets WM_SETFOCUS, so both are tracked.
            case WM_SETFOCUS:
                has_keyboard_ = true;
                sync_focus();
                return 0;
            case WM_KILLFOCUS:
                has_keyboard_ = false;
                fake_lctrl_ = false;
                sync_focus();
                return 0;
            case WM_ACTIVATE:
            case WM_ACTIVATEAPP:
            case WM_NCACTIVATE: {
                const LRESULT r = DefWindowProcW(hwnd_, msg, w, l);
                sync_focus();
                // The foreground switch may complete after these messages: look again later.
                PostMessageW(hwnd_, kWmSyncFocus, 0, 0);
                return r;
            }
            case kWmSyncFocus:
                sync_focus();
                return 0;
            case WM_NCHITTEST:
                // A borderless window moves and resizes where the app's hit test says so (the
                // quick terminal's title bar and edges); the system then drags, sizes and sets
                // the cursor itself.
                if (hit_test_ && quick_) {
                    POINT pt{static_cast<int16_t>(LOWORD(l)), static_cast<int16_t>(HIWORD(l))};
                    ScreenToClient(hwnd_, &pt);
                    const float k = scale_ > 0 ? scale_ : 1.0f;
                    switch (hit_test_(DipPoint{static_cast<float>(pt.x) / k, static_cast<float>(pt.y) / k})) {
                        case HitArea::caption: return HTCAPTION;
                        case HitArea::resize_n: return HTTOP;
                        case HitArea::resize_s: return HTBOTTOM;
                        case HitArea::resize_e: return HTRIGHT;
                        case HitArea::resize_w: return HTLEFT;
                        case HitArea::resize_ne: return HTTOPRIGHT;
                        case HitArea::resize_nw: return HTTOPLEFT;
                        case HitArea::resize_se: return HTBOTTOMRIGHT;
                        case HitArea::resize_sw: return HTBOTTOMLEFT;
                        default: return HTCLIENT;
                    }
                }
                break;
            case WM_NCLBUTTONDBLCLK:
                if (quick_) return 0;   // no maximize on the panel's title bar
                break;
            case WM_SETCURSOR:
                if ((static_cast<uint32_t>(l) & 0xFFFF) == HTCLIENT) {
                    SetCursor(cursor_);
                    return TRUE;
                }
                break;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: return on_key(msg, w, l);
            case WM_CHAR:
            case WM_SYSCHAR: on_stray_char(static_cast<char16_t>(w)); return 0;
            case WM_DEADCHAR:
            case WM_SYSDEADCHAR: return 0;

            case WM_IME_SETCONTEXT:
                // Inline preedit: the system must not draw its own composition window.
                return DefWindowProcW(hwnd_, msg, w, l & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
            case WM_IME_STARTCOMPOSITION: position_ime(); return 0;
            case WM_IME_COMPOSITION: on_ime_composition(l); return 0;
            case WM_IME_ENDCOMPOSITION: l_.on_composition(Composition{}); return 0;

            case WM_MOUSEMOVE: on_mouse_move(l); return 0;
            case WM_MOUSELEAVE: {
                hovering_ = false;
                MouseEvent m;
                m.type = MouseEvent::Type::leave;
                m.mods = mods();
                l_.on_mouse(m);
                return 0;
            }
            case WM_LBUTTONDOWN: button(MouseButton::left, true, l); return 0;
            case WM_LBUTTONUP: button(MouseButton::left, false, l); return 0;
            case WM_RBUTTONDOWN: button(MouseButton::right, true, l); return 0;
            case WM_RBUTTONUP: button(MouseButton::right, false, l); return 0;
            case WM_MBUTTONDOWN: button(MouseButton::middle, true, l); return 0;
            case WM_MBUTTONUP: button(MouseButton::middle, false, l); return 0;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                button(hi16w(w) == XBUTTON1 ? MouseButton::back : MouseButton::forward, msg == WM_XBUTTONDOWN, l);
                return TRUE;
            case WM_CAPTURECHANGED: buttons_down_ = 0; return 0;
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: on_wheel(msg == WM_MOUSEHWHEEL, hi16w(w), l); return 0;
            default: break;
        }
        return DefWindowProcW(hwnd_, msg, w, l);
    }

    // WM_NCDESTROY: the last message. `this` may be deleted by the listener at the end.
    void destroyed() {
        if (frame_timer_) p_.loop().cancel_timer(frame_timer_);
        frame_timer_ = 0;
        if (in_modal_) p_.win_loop().exit_modal();
        in_modal_ = false;
        child_ = nullptr;   // destroyed with its parent
        hwnd_ = nullptr;
        if (notify_destroy_) l_.on_destroyed();
    }

    void surface_paint() {
        if (ready_) l_.on_expose();
    }

    HWND hwnd_ = nullptr;

private:
    static UINT dpi_of(HWND h) {
        const Fns& f = fns();
        if (f.GetDpiForWindow) {
            if (const UINT d = f.GetDpiForWindow(h)) return d;
        }
        if (f.GetDpiForMonitor) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(f.GetDpiForMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), 0, &x, &y))) return x;
        }
        HDC dc = GetDC(nullptr);
        const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        return static_cast<UINT>(d > 0 ? d : 96);
    }

    void adjust(RECT& rc, UINT dpi) const {
        const DWORD style = hwnd_ ? static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)) : kStyle;
        const DWORD ex = hwnd_ ? static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)) : kExStyle;
        if (fns().AdjustWindowRectExForDpi) fns().AdjustWindowRectExForDpi(&rc, style, FALSE, ex, dpi);
        else AdjustWindowRectEx(&rc, style, FALSE, ex);
    }

    void update_refresh() {
        if (!hwnd_) return;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return;
        // 0 and 1 mean "hardware default".
        if (dm.dmDisplayFrequency > 1 && dm.dmDisplayFrequency <= 500) {
            pacer_.set_interval(std::chrono::microseconds(1'000'000 / dm.dmDisplayFrequency));
        }
    }

    DipPoint pos_of(LPARAM l) const {
        return {detail::px_to_dip(lo16(l), scale_), detail::px_to_dip(hi16(l), scale_)};
    }

    uint16_t mods() const {
        detail::WinKeyState s;
        s.lshift = key_down(VK_LSHIFT);
        s.rshift = key_down(VK_RSHIFT);
        s.lctrl = key_down(VK_LCONTROL);
        s.rctrl = key_down(VK_RCONTROL);
        s.lalt = key_down(VK_LMENU);
        s.ralt = key_down(VK_RMENU);
        s.lwin = key_down(VK_LWIN);
        s.rwin = key_down(VK_RWIN);
        s.caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        s.num = (GetKeyState(VK_NUMLOCK) & 1) != 0;
        s.altgr = fake_lctrl_ && s.ralt;
        return detail::mods_from_win(s);
    }

    // ------------------------------------------------------------ size and DPI
    void on_size(WPARAM type, LPARAM l) {
        if (type == SIZE_MINIMIZED) {
            if (!occluded_) {
                occluded_ = true;
                l_.on_occluded(true);
            }
            return;
        }
        if (occluded_) {
            occluded_ = false;
            l_.on_occluded(false);
            if (frame_pending_ && !frame_timer_) schedule_frame();
        }
        const PxSize px{static_cast<uint16_t>(static_cast<uint32_t>(l) & 0xFFFF),
                        static_cast<uint16_t>((static_cast<uint32_t>(l) >> 16) & 0xFFFF)};
        if (px == px_) return;
        px_ = px;
        if (child_) SetWindowPos(child_, nullptr, 0, 0, px.w, px.h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        if (!ready_) return;
        l_.on_resized(px_, detail::to_dip(px_, scale_));
        // Live resize: render now so that the content follows the frame.
        if (in_modal_ && frame_pending_) deliver_frame();
    }

    void on_dpi_changed(UINT dpi, const RECT& suggested) {
        dpi_ = dpi;
        scale_ = detail::scale_from_dpi(dpi);
        const PxSize before = px_;
        const RECT to = pinned_ ? *pinned_ : suggested;
        SetWindowPos(hwnd_, nullptr, to.left, to.top, to.right - to.left, to.bottom - to.top, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        update_refresh();
        l_.on_scale_changed(scale_, detail::to_dip(to.left, to.top, cr.right, cr.bottom, scale_));
        // WM_SIZE already reported a changed pixel size; same pixels mean a new DIP size.
        if (hwnd_ && px_ == before) l_.on_resized(px_, detail::to_dip(px_, scale_));
    }

    LRESULT on_get_dpi_scaled_size(UINT new_dpi, SIZE* out) {
        if (!fns().AdjustWindowRectExForDpi || !out) return FALSE;
        // Keep the client area's DIP size exactly instead of the OS's linear guess.
        const PxSize client = detail::rescale_client(px_, dpi_, new_dpi);
        RECT rc{0, 0, client.w, client.h};
        adjust(rc, new_dpi);
        out->cx = rc.right - rc.left;
        out->cy = rc.bottom - rc.top;
        return TRUE;
    }

    // ------------------------------------------------------------ frames
    void schedule_frame() {
        if (occluded_) return;
        frame_timer_ = p_.loop().add_timer(pacer_.due(Clock::now()), [this] {
            frame_timer_ = 0;
            if (!frame_pending_ || occluded_ || !hwnd_) return;
            deliver_frame();
        });
    }

    void deliver_frame() {
        if (frame_timer_) {
            p_.loop().cancel_timer(frame_timer_);
            frame_timer_ = 0;
        }
        frame_pending_ = false;
        l_.on_frame(pacer_.delivered(Clock::now()));
    }

    // ------------------------------------------------------------ keyboard
    LRESULT on_key(UINT msg, WPARAM w, LPARAM l) {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        const auto vk = static_cast<UINT>(w);
        const auto lp = static_cast<uint32_t>(l);
        const bool extended = (lp >> 24) & 1;
        const LONG time = GetMessageTime();

        if (vk == VK_PROCESSKEY) {   // the IME owns this key
            if (down) translate(msg, w, l);
            return 0;
        }
        // AltGr arrives as a synthesized LCtrl followed by RAlt with the same time stamp.
        if (vk == VK_CONTROL && !extended) {
            if (down) {
                MSG next;
                if (PeekMessageW(&next, hwnd_, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE | PM_NOYIELD)) {
                    const detail::RawKeyMsg ctrl{msg, VK_CONTROL, false, static_cast<uint32_t>(time)};
                    const detail::RawKeyMsg nxt{next.message, static_cast<uint32_t>(next.wParam),
                                                ((static_cast<uint32_t>(next.lParam) >> 24) & 1) != 0,
                                                static_cast<uint32_t>(next.time)};
                    if (detail::is_altgr_lctrl(ctrl, nxt)) {
                        fake_lctrl_ = true;
                        return 0;
                    }
                }
            } else if (fake_lctrl_) {
                fake_lctrl_ = false;
                return 0;
            }
        }

        UINT scan = (lp >> 16) & 0xFF;
        if (!scan) scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        KeyEvent k;
        k.key = detail::key_from_win_scancode(scan, extended);
        k.native_code = vk;
        const bool was_down = (lp >> 30) & 1;
        k.action = !down ? KeyAction::release : (was_down ? KeyAction::repeat : KeyAction::press);
        k.mods = mods();
        k.logical = logical(vk, scan);

        if (down) {
            translate(msg, w, l);
            std::u16string chars;
            MSG m;
            for (;;) {
                if (PeekMessageW(&m, hwnd_, WM_CHAR, WM_DEADCHAR, PM_REMOVE | PM_NOYIELD) ||
                    PeekMessageW(&m, hwnd_, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE | PM_NOYIELD)) {
                    if (m.message == WM_DEADCHAR || m.message == WM_SYSDEADCHAR) k.composing = true;
                    else chars.push_back(static_cast<char16_t>(m.wParam));
                    continue;
                }
                break;
            }
            k.text = text_of(chars);
        }
        consumed_ = false;
        l_.on_key(k);
        if (!hwnd_) return 0;   // the listener closed the window
        if (down && !consumed_ && !k.text.empty()) l_.on_text(k.text);

        if (msg == WM_SYSKEYDOWN && !consumed_) {
            // Keep the system shortcuts that the terminal does not use: Alt+F4 closes, Alt+Space
            // opens the window menu.
            if (vk == VK_F4) return DefWindowProcW(hwnd_, msg, w, l);
            if (vk == VK_SPACE) {
                SendMessageW(hwnd_, WM_SYSCOMMAND, SC_KEYMENU, ' ');
                return 0;
            }
        }
        return 0;   // F10 and bare Alt must not enter menu mode
    }

    void translate(UINT msg, WPARAM w, LPARAM l) {
        MSG m{};
        m.hwnd = hwnd_;
        m.message = msg;
        m.wParam = w;
        m.lParam = l;
        m.time = static_cast<DWORD>(GetMessageTime());
        const DWORD pos = GetMessagePos();
        m.pt.x = lo16(static_cast<LPARAM>(pos));
        m.pt.y = hi16(static_cast<LPARAM>(pos));
        TranslateMessage(&m);
    }

    static char32_t logical(UINT vk, UINT scan) {
        BYTE state[256] = {};
        wchar_t buf[8] = {};
        const int n = ToUnicodeEx(vk, scan, state, buf, 8, kToUnicodeKeepState, GetKeyboardLayout(0));
        if (n == 0) return 0;
        const auto c = static_cast<char32_t>(static_cast<char16_t>(buf[0]));   // n < 0: the dead key's char
        if (c >= 0xD800 && c <= 0xDFFF) return 0;
        if (!detail::is_text_char(c)) return 0;
        return detail::ascii_lower(c);
    }

    void on_stray_char(char16_t c) {
        // Characters without a key message we handled: VK_PACKET from SendInput, Alt+numpad,
        // posted WM_CHAR. Surrogate pairs come as two messages.
        if (c >= 0xD800 && c <= 0xDBFF) {
            high_surrogate_ = c;
            return;
        }
        std::u16string u;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!high_surrogate_) return;
            u.push_back(high_surrogate_);
        }
        high_surrogate_ = 0;
        u.push_back(c);
        const std::u8string t = text_of(u);
        if (!t.empty()) l_.on_text(t);
    }

    // ------------------------------------------------------------ IME
    void position_ime() {
        if (!hwnd_) return;
        HIMC imc = ImmGetContext(hwnd_);
        if (!imc) return;
        const LONG x = detail::dip_to_px(caret_.x, scale_), y = detail::dip_to_px(caret_.y, scale_);
        const LONG w = detail::dip_to_px(caret_.w, scale_), h = detail::dip_to_px(caret_.h, scale_);
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{x, y};
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos = POINT{x, y + h};
        cand.rcArea = RECT{x, y, x + std::max<LONG>(w, 1), y + h};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd_, imc);
    }

    static std::u16string ime_string(HIMC imc, DWORD which) {
        const LONG bytes = ImmGetCompositionStringW(imc, which, nullptr, 0);
        if (bytes <= 0) return {};
        std::u16string s(static_cast<size_t>(bytes) / sizeof(char16_t), u'\0');
        ImmGetCompositionStringW(imc, which, s.data(), static_cast<DWORD>(bytes));
        return s;
    }

    void on_ime_composition(LPARAM l) {
        HIMC imc = ImmGetContext(hwnd_);
        if (!imc) return;
        const auto flags = static_cast<DWORD>(l);
        if (flags & GCS_RESULTSTR) {
            const std::u8string t = text_of(ime_string(imc, GCS_RESULTSTR));
            if (!t.empty()) l_.on_text(t);
        }
        if (flags & GCS_COMPSTR) {
            const std::u16string s16 = ime_string(imc, GCS_COMPSTR);
            Composition c;
            c.text = detail::utf16_to_utf8(s16);
            if (!s16.empty()) {
                if (flags & GCS_CURSORPOS) {
                    const LONG cur = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
                    if (cur >= 0) c.caret = detail::utf8_offset_of_utf16_index(s16, static_cast<size_t>(cur));
                } else {
                    c.caret = static_cast<uint32_t>(c.text.size());
                }
                if (flags & GCS_COMPATTR) {
                    const LONG n = ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
                    std::string attrs(static_cast<size_t>(std::max<LONG>(n, 0)), '\0');
                    if (n > 0) ImmGetCompositionStringW(imc, GCS_COMPATTR, attrs.data(), static_cast<DWORD>(n));
                    // One attribute byte per UTF-16 unit; target clauses are the selected segment.
                    size_t i = 0;
                    while (i < attrs.size() && i < s16.size()) {
                        const bool sel = attrs[i] == ATTR_TARGET_CONVERTED || attrs[i] == ATTR_TARGET_NOTCONVERTED;
                        size_t j = i + 1;
                        while (j < attrs.size() && j < s16.size() &&
                               (attrs[j] == ATTR_TARGET_CONVERTED || attrs[j] == ATTR_TARGET_NOTCONVERTED) == sel) {
                            ++j;
                        }
                        c.segments.push_back(Composition::Segment{detail::utf8_offset_of_utf16_index(s16, i),
                                                                  detail::utf8_offset_of_utf16_index(s16, j), sel});
                        i = j;
                    }
                }
            }
            l_.on_composition(c);   // empty text: the composition ended
        }
        ImmReleaseContext(hwnd_, imc);
        position_ime();
    }

    // ------------------------------------------------------------ mouse
    void on_mouse_move(LPARAM l) {
        MouseEvent m;
        m.mods = mods();
        m.pos = pos_of(l);
        if (!hovering_) {
            hovering_ = true;
            TRACKMOUSEEVENT t{};
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd_;
            TrackMouseEvent(&t);
            m.type = MouseEvent::Type::enter;
            l_.on_mouse(m);
        }
        m.type = MouseEvent::Type::move;
        l_.on_mouse(m);
    }

    void button(MouseButton b, bool press, LPARAM l) {
        const auto bit = static_cast<uint8_t>(1u << static_cast<unsigned>(b));
        if (press) {
            if (!buttons_down_) SetCapture(hwnd_);
            buttons_down_ = static_cast<uint8_t>(buttons_down_ | bit);
        } else {
            buttons_down_ = static_cast<uint8_t>(buttons_down_ & ~bit);
            if (!buttons_down_ && GetCapture() == hwnd_) ReleaseCapture();
        }
        MouseEvent m;
        m.type = press ? MouseEvent::Type::press : MouseEvent::Type::release;
        m.button = b;
        m.mods = mods();
        m.pos = pos_of(l);
        if (press) m.click_count = clicks_.press(b, static_cast<uint32_t>(GetMessageTime()), m.pos);
        l_.on_mouse(m);
    }

    // Files dropped from Explorer (WM_DROPFILES). The message filter lets a
    // drop from a normal Explorer reach an elevated keepterm (UIPI).
    // Files only (WM_DROPFILES). UIPI blocks drops from a non-elevated Explorer into an elevated
    // window; the only workaround opens an undocumented message, so those drops stay unsupported
    //.
    static void accept_files(HWND h) { DragAcceptFiles(h, TRUE); }

public:
    void on_drop_files(HDROP drop, HWND from) {
        DropEvent e;
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (!len) continue;
            std::wstring buf(len + 1, L'\0');
            if (DragQueryFileW(drop, i, buf.data(), len + 1) == 0) continue;
            buf.resize(len);
            e.paths.emplace_back(buf);
        }
        POINT pt{};
        DragQueryPoint(drop, &pt);                // in the client area of the window that took it
        if (from && from != hwnd_) MapWindowPoints(from, hwnd_, &pt, 1);
        DragFinish(drop);
        e.pos = DipPoint{detail::px_to_dip(pt.x, scale_), detail::px_to_dip(pt.y, scale_)};
        if (e.paths.empty()) return;
        DragOverEvent d;
        d.pos = e.pos;
        d.has_paths = true;
        if (l_.on_drag_over(d) == DropEffect::copy) l_.on_drop(e);
    }

private:
    void on_wheel(bool horizontal, int16_t delta, LPARAM l) {
        POINT pt{lo16(l), hi16(l)};   // screen coordinates
        ScreenToClient(hwnd_, &pt);
        ScrollEvent s;
        s.mods = mods();
        s.pos = DipPoint{detail::px_to_dip(pt.x, scale_), detail::px_to_dip(pt.y, scale_)};
        // Fractional lines for high-resolution wheels; `precise` (DIP deltas) is not used on
        // Windows because WM_MOUSEWHEEL has no pixel deltas.
        if (horizontal) s.dx = -detail::wheel_lines(delta, wheel_chars_);   // positive = right
        else s.dy = detail::wheel_lines(delta, wheel_lines_);
        l_.on_scroll(s);
    }

    WinPlatform& p_;
    IWindowListener& l_;
    HWND child_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1;
    PxSize px_;
    DipSize min_size_{320, 200};
    bool ready_ = false;
    bool notify_destroy_ = true;
    bool focused_ = false;        // has_keyboard_ and the foreground window
    bool has_keyboard_ = false;   // WM_SETFOCUS .. WM_KILLFOCUS
    bool occluded_ = false;
    bool fullscreen_ = false;
    bool start_maximized_ = false;
    bool quick_ = false;              // WindowKind::quick_terminal
    std::optional<RECT> pinned_;      // place_on in progress
    bool start_fullscreen_ = false;
    bool in_modal_ = false;
    LONG saved_style_ = 0;
    WINDOWPLACEMENT saved_wp_{};
    bool frame_pending_ = false;
    TimerId frame_timer_ = 0;
    detail::FramePacer pacer_;
    detail::ClickCounter clicks_;
    UINT wheel_lines_ = 3, wheel_chars_ = 3;
    HCURSOR cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    bool hovering_ = false;
    uint8_t buttons_down_ = 0;
    std::function<HitArea(DipPoint)> hit_test_;
    bool ime_enabled_ = true;
    DipRect caret_;
    bool consumed_ = false;
    bool fake_lctrl_ = false;
    char16_t high_surrogate_ = 0;
};

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<WinWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // Per-monitor v1 (1607) scales the non-client area only when asked; v2 ignores this.
        if (fns().EnableNonClientDpiScaling) fns().EnableNonClientDpiScaling(hwnd);
        return DefWindowProcW(hwnd, msg, w, l);
    }
    auto* self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, w, l);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        const LRESULT r = DefWindowProcW(hwnd, msg, w, l);
        self->destroyed();   // last: the listener may delete the window object
        return r;
    }
    return self->handle(msg, w, l);
}

LRESULT CALLBACK surface_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, w, l);
    }
    auto* owner = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;   // input goes to the top-level window
        case WM_DROPFILES:
            if (owner) owner->on_drop_files(reinterpret_cast<HDROP>(w), hwnd);
            else DragFinish(reinterpret_cast<HDROP>(w));
            return 0;
        case WM_ERASEBKGND: return 1;              // the presenter covers every pixel
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (owner) owner->surface_paint();
            return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

Result<std::unique_ptr<IWindow>> create_win_window(WinPlatform& p, const WindowDesc& d, IWindowListener& l) {
    auto w = std::make_unique<WinWindow>(p, l);
    if (auto r = w->init(d); !r) return Unexpected(r.error());
    return std::unique_ptr<IWindow>(std::move(w));
}

}  // namespace kt::platform::win
