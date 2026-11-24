// X11 windows: EWMH top-level, render child window, XIM input context, keyboard, mouse,
// scroll, frame pacing.
#include <unistd.h>

#include <X11/Xatom.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "platform_gui/app_icon.h"
#include "platform_gui/dpi.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/utf.h"
#include "platform_gui/x11_internal_linux.h"

namespace kt::platform::x11 {

namespace {

constexpr long kEventMask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            EnterWindowMask | LeaveWindowMask | FocusChangeMask | StructureNotifyMask |
                            PropertyChangeMask | ExposureMask;

std::u8string filter_text(std::u8string_view s) {
    std::u8string out;
    while (!s.empty()) {
        size_t n = 0;
        const char32_t c = detail::utf8_first(s, &n);
        s.remove_prefix(n);
        if (detail::is_text_char(c)) detail::utf8_append(out, c);
    }
    return out;
}

bool is_dead_keysym(KeySym ks) { return ks >= 0xFE50 && ks <= 0xFE8F; }   // XK_dead_grave .. XK_dead_*

}  // namespace

class X11Window final : public IWindow {
public:
    X11Window(X11Platform& p, IWindowListener& l) : p_(p), x_(p.x()), l_(l) {}
    ~X11Window() override { destroy(false); }

    Result<void> init(const WindowDesc& d) {
        Display* dpy = p_.dpy();
        scale_ = p_.scale();
        const float w = d.placement.rect.w > 0 ? d.placement.rect.w : 800;
        const float h = d.placement.rect.h > 0 ? d.placement.rect.h : 500;
        px_ = detail::to_px(DipSize{w, h}, scale_);
        px_.w = std::max(px_.w, 1);
        px_.h = std::max(px_.h, 1);
        const int x = detail::dip_to_px(d.placement.rect.x, scale_), y = detail::dip_to_px(d.placement.rect.y, scale_);

        XSetWindowAttributes a{};
        a.background_pixel = 0;
        a.bit_gravity = NorthWestGravity;   // keep the old pixels at the top-left while resizing
        a.event_mask = kEventMask;
        win_ = x_.XCreateWindow(dpy, p_.root(), x, y, static_cast<unsigned>(px_.w), static_cast<unsigned>(px_.h), 0,
                                CopyFromParent, InputOutput, nullptr, CWBackPixel | CWBitGravity | CWEventMask, &a);
        if (!win_) return make_error(Errc::os_error, "XCreateWindow failed");
        p_.register_window(win_, this);

        const Atoms& at = p_.atoms();
        Atom protocols[] = {at.wm_delete_window, at.net_wm_ping};
        x_.XSetWMProtocols(dpy, win_, protocols, 2);
        const long pid = static_cast<long>(getpid());
        x_.XChangeProperty(dpy, win_, at.net_wm_pid, at.cardinal, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(&pid), 1);
        // The application icon (title bar, task switcher, panels), every size.
        static const std::vector<unsigned long> icon = app_icon_net_wm_icon();
        x_.XChangeProperty(dpy, win_, at.net_wm_icon, at.cardinal, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(icon.data()), static_cast<int>(icon.size()));
        const long xdnd_version = 5;
        x_.XChangeProperty(dpy, win_, at.xdnd_aware, at.atom, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(&xdnd_version), 1);
        const Atom type = at.net_wm_window_type_normal;
        x_.XChangeProperty(dpy, win_, at.net_wm_window_type, at.atom, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(&type), 1);
        quick_ = d.kind == WindowKind::quick_terminal;
        if (quick_) quick_props();
        std::string id = p_.app_id();
        XClassHint ch{id.data(), id.data()};
        x_.XSetClassHint(dpy, win_, &ch);
        XSizeHints sh{};
        sh.flags = PMinSize | (x || y || quick_ ? (PPosition | USPosition) : 0) | (quick_ ? (PSize | USSize) : 0);
        sh.width = px_.w;
        sh.height = px_.h;
        sh.min_width = std::max(1, detail::dip_to_px(d.min_size.w, scale_));
        sh.min_height = std::max(1, detail::dip_to_px(d.min_size.h, scale_));
        min_px_ = PxSize{sh.min_width, sh.min_height};
        sh.x = x;
        sh.y = y;
        x_.XSetWMNormalHints(dpy, win_, &sh);
        set_title(d.title);
        if (!quick_ && (d.placement.fullscreen || d.placement.maximized)) {
            // Before mapping, the state is a plain property (EWMH).
            std::vector<Atom> st;
            if (d.placement.fullscreen) st.push_back(at.net_wm_state_fullscreen);
            if (d.placement.maximized) {
                st.push_back(at.net_wm_state_max_vert);
                st.push_back(at.net_wm_state_max_horz);
            }
            x_.XChangeProperty(dpy, win_, at.net_wm_state, at.atom, 32, PropModeReplace,
                               reinterpret_cast<const unsigned char*>(st.data()), static_cast<int>(st.size()));
            fullscreen_ = d.placement.fullscreen;
        }
        create_ic();
        return {};
    }

    // Before every map: the window manager drops _NET_WM_STATE when a window is withdrawn.
    // No decorations (Motif hints: flags = decorations, decorations = 0), above other windows,
    // on every desktop, not in the taskbar or the pager (EWMH).
    void quick_props() {
        Display* dpy = p_.dpy();
        const Atoms& at = p_.atoms();
        const long motif[5] = {2, 0, 0, 0, 0};
        x_.XChangeProperty(dpy, win_, at.motif_wm_hints, at.motif_wm_hints, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(motif), 5);
        const Atom st[4] = {at.net_wm_state_above, at.net_wm_state_sticky, at.net_wm_state_skip_taskbar,
                            at.net_wm_state_skip_pager};
        x_.XChangeProperty(dpy, win_, at.net_wm_state, at.atom, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(st), 4);
        const long all_desktops = 0xFFFFFFFF;
        x_.XChangeProperty(dpy, win_, x_.XInternAtom(dpy, "_NET_WM_DESKTOP", False), at.cardinal, 32, PropModeReplace,
                           reinterpret_cast<const unsigned char*>(&all_desktops), 1);
    }

    // ------------------------------------------------------------ IWindow
    void show(bool activate) override {
        if (!win_) return;
        if (quick_) quick_props();
        x_.XMapWindow(p_.dpy(), win_);
        if (activate || quick_) x_.XRaiseWindow(p_.dpy(), win_);
        if (quick_) {
            // The window manager activates it; without one, MapNotify sets the focus.
            focus_on_map_ = true;
            p_.activate(win_, false);
        }
        x_.XFlush(p_.dpy());
    }

    void hide() override {
        if (win_) x_.XUnmapWindow(p_.dpy(), win_);
    }

    void close() override { destroy(true); }

    void set_title(std::u8string_view t) override {
        if (!win_) return;
        const auto* data = reinterpret_cast<const unsigned char*>(t.data());
        const int n = static_cast<int>(t.size());
        x_.XChangeProperty(p_.dpy(), win_, p_.atoms().net_wm_name, p_.atoms().utf8_string, 8, PropModeReplace, data, n);
        x_.XChangeProperty(p_.dpy(), win_, XA_WM_NAME, p_.atoms().utf8_string, 8, PropModeReplace, data, n);
    }

    void set_placement(const WindowPlacement& pl) override {
        if (!win_) return;
        const int x = detail::dip_to_px(pl.rect.x, scale_), y = detail::dip_to_px(pl.rect.y, scale_);
        const int w = std::max(1, detail::dip_to_px(pl.rect.w, scale_)), h = std::max(1, detail::dip_to_px(pl.rect.h, scale_));
        x_.XMoveResizeWindow(p_.dpy(), win_, x, y, static_cast<unsigned>(w), static_cast<unsigned>(h));
        if (pl.fullscreen != fullscreen_) set_fullscreen(pl.fullscreen);
    }

    WindowPlacement placement() const override {
        WindowPlacement pl;
        if (!win_) return pl;
        int rx = 0, ry = 0;
        Window child = 0;
        x_.XTranslateCoordinates(p_.dpy(), win_, p_.root(), 0, 0, &rx, &ry, &child);
        pl.rect = detail::to_dip(rx, ry, px_.w, px_.h, scale_);
        pl.fullscreen = fullscreen_;
        return pl;
    }

    void set_fullscreen(bool on) override {
        if (!win_) return;
        fullscreen_ = on;
        send_state(on ? 1 : 0, p_.atoms().net_wm_state_fullscreen, 0);
    }

    void minimize() override {
        if (win_) x_.XIconifyWindow(p_.dpy(), win_, p_.screen());
    }

    void toggle_maximize() override {
        if (win_) send_state(2, p_.atoms().net_wm_state_max_vert, p_.atoms().net_wm_state_max_horz);
    }

    void set_cursor(CursorShape s) override {
        if (!win_) return;
        x_.XDefineCursor(p_.dpy(), win_, p_.cursor(s));
        x_.XFlush(p_.dpy());
    }

    void set_hit_test(std::function<HitArea(DipPoint)> fn) override { hit_test_ = std::move(fn); }

    float scale() const override { return scale_; }
    PxSize client_size_px() const override { return px_; }
    DipSize client_size() const override { return detail::to_dip(px_, scale_); }
    bool focused() const override { return focused_; }
    // ICCCM XUrgencyHint in WM_HINTS, beside the EWMH state: panels honour it, and it needs no
    // window manager to take effect.
    void set_urgency_hint(bool on) {
        if (!x_.XGetWMHints || !x_.XSetWMHints) return;
        XWMHints* got = x_.XGetWMHints(p_.dpy(), win_);
        XWMHints h{};
        if (got) {
            h = *got;
            x_.XFree(got);
        }
        if (on) h.flags |= XUrgencyHint;
        else h.flags &= ~XUrgencyHint;
        x_.XSetWMHints(p_.dpy(), win_, &h);
        x_.XFlush(p_.dpy());
    }
    void request_attention() override {
        if (!win_ || focused_ || urgent_) return;
        urgent_ = true;
        send_state(1, p_.atoms().net_wm_state_demands_attention, 0);
        set_urgency_hint(true);
    }

    Result<NativeSurface> create_surface(const SurfaceRequest& req) override {
        if (!win_) return make_error(Errc::unavailable, "window is closed");
        if (req.x11_visual_id != 0) return make_error(Errc::not_supported, "non-default visuals are not supported yet");
        destroy_surface();
        XSetWindowAttributes a{};
        a.background_pixmap = None;   // no server-side clear before each present
        a.bit_gravity = NorthWestGravity;
        a.event_mask = ExposureMask;  // input propagates to the top-level, which selects it
        a.backing_store = WhenMapped;
        child_ = x_.XCreateWindow(p_.dpy(), win_, 0, 0, static_cast<unsigned>(std::max(px_.w, 1)),
                                  static_cast<unsigned>(std::max(px_.h, 1)), 0, CopyFromParent, InputOutput, nullptr,
                                  CWBackPixmap | CWBitGravity | CWEventMask | CWBackingStore, &a);
        if (!child_) return make_error(Errc::os_error, "XCreateWindow failed for the render surface");
        p_.register_window(child_, this);
        x_.XMapWindow(p_.dpy(), child_);
        x_.XFlush(p_.dpy());
        NativeSurface s;
        s.ws = WindowSystem::x11;
        s.display = p_.dpy();
        s.xid = child_;
        return s;
    }

    void destroy_surface() override {
        if (!child_) return;
        p_.unregister_window(child_);
        x_.XDestroyWindow(p_.dpy(), child_);
        child_ = 0;
    }

    void request_frame() override {
        if (!win_ || frame_pending_) return;
        frame_pending_ = true;
        schedule_frame();
    }

    void set_ime_enabled(bool on) override {
        ime_enabled_ = on;
        if (!ic_) return;
        if (on && focused_) x_.XSetICFocus(ic_);
        else x_.XUnsetICFocus(ic_);
    }

    void set_ime_cursor_rect(DipRect r) override {
        caret_ = r;
        if (!ic_ || !(ic_style_ & XIMPreeditPosition)) return;
        XPoint spot{static_cast<short>(detail::dip_to_px(r.x, scale_)),
                    static_cast<short>(detail::dip_to_px(r.y + r.h, scale_))};
        XVaNestedList list = x_.XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        x_.XSetICValues(ic_, XNPreeditAttributes, list, nullptr);
        x_.XFree(list);
    }

    void set_secure_input(bool) override {}

    void consume_key() override { consumed_ = true; }

    // ------------------------------------------------------------ events
    void handle(XEvent& e) {
        switch (e.type) {
            case KeyPress: key(e.xkey, true); break;
            case KeyRelease: key(e.xkey, false); break;
            case ButtonPress:
            case ButtonRelease: button(e.xbutton); break;
            case MotionNotify: {
                if (drag_) {
                    frame_drag_to(e.xmotion.x_root, e.xmotion.y_root);
                    break;
                }
                MouseEvent m;
                m.type = MouseEvent::Type::move;
                m.mods = detail::mods_from_x11_state(e.xmotion.state, sides_);
                m.pos = pos(e.xmotion.x, e.xmotion.y);
                l_.on_mouse(m);
                break;
            }
            case EnterNotify:
            case LeaveNotify: {
                if (e.xcrossing.detail == NotifyInferior) break;   // into or out of the render child
                MouseEvent m;
                m.type = e.type == EnterNotify ? MouseEvent::Type::enter : MouseEvent::Type::leave;
                m.mods = detail::mods_from_x11_state(e.xcrossing.state, sides_);
                m.pos = pos(e.xcrossing.x, e.xcrossing.y);
                l_.on_mouse(m);
                break;
            }
            case FocusIn:
            case FocusOut: {
                if (e.xfocus.mode == NotifyGrab || e.xfocus.mode == NotifyUngrab) break;
                if (e.xfocus.detail == NotifyPointer) break;
                const bool in = e.type == FocusIn;
                if (in == focused_) break;
                focused_ = in;
                if (in && urgent_) {   // most window managers clear it themselves; be sure
                    urgent_ = false;
                    send_state(0, p_.atoms().net_wm_state_demands_attention, 0);
                    set_urgency_hint(false);
                }
                if (ic_) {
                    if (in && ime_enabled_) x_.XSetICFocus(ic_);
                    else x_.XUnsetICFocus(ic_);
                }
                if (!in) {
                    sides_ = 0;
                    std::memset(pressed_, 0, sizeof pressed_);
                }
                l_.on_focus(in);
                break;
            }
            case ConfigureNotify: {
                if (e.xconfigure.window != win_) break;
                const PxSize px{e.xconfigure.width, e.xconfigure.height};
                if (px == px_) break;
                px_ = px;
                if (child_) x_.XResizeWindow(p_.dpy(), child_, static_cast<unsigned>(px.w), static_cast<unsigned>(px.h));
                l_.on_resized(px_, detail::to_dip(px_, scale_));
                break;
            }
            case Expose:
                if (e.xexpose.count == 0) l_.on_expose();
                break;
            case MapNotify:
                if (e.xmap.window == win_ && focus_on_map_) {
                    focus_on_map_ = false;
                    if (x_.XSetInputFocus) x_.XSetInputFocus(p_.dpy(), win_, RevertToParent, p_.last_time());
                }
                if (e.xmap.window == win_ && occluded_) {
                    occluded_ = false;
                    l_.on_occluded(false);
                    if (frame_pending_ && !frame_timer_) schedule_frame();
                }
                break;
            case UnmapNotify:
                if (e.xunmap.window == win_ && !occluded_) {
                    occluded_ = true;
                    l_.on_occluded(true);
                }
                break;
            case ClientMessage: client_message(e.xclient); break;
            case SelectionNotify:
                if (e.xselection.property == p_.atoms().kt_xdnd || e.xselection.selection == p_.atoms().xdnd_selection) xdnd_data(e.xselection);
                break;
            case DestroyNotify:
                if (e.xdestroywindow.window == win_) {
                    // Destroyed from outside (rare: the WM or a killclient).
                    p_.unregister_window(win_);
                    if (child_) p_.unregister_window(child_);
                    win_ = 0;
                    child_ = 0;
                    destroy(true);
                }
                break;
            default: break;
        }
    }

    void scale_changed(float s) {
        if (s == scale_) return;
        scale_ = s;
        // X11 keeps the pixel size; the DIP size follows the new scale.
        const DipSize dip = detail::to_dip(px_, scale_);
        const WindowPlacement pl = placement();
        l_.on_scale_changed(s, DipRect{pl.rect.x, pl.rect.y, dip.w, dip.h});
        if (win_) l_.on_resized(px_, dip);
    }

private:
    DipPoint pos(int x, int y) const { return {detail::px_to_dip(x, scale_), detail::px_to_dip(y, scale_)}; }

    void create_ic() {
        XIM im = p_.im();
        if (!im) return;
        XIMStyles* styles = nullptr;
        if (x_.XGetIMValues(im, XNQueryInputStyle, &styles, nullptr) != nullptr || !styles) return;
        // Over-the-spot when the IM supports it (the IM draws the preedit at the caret), else
        // root style. Inline preedit callbacks are a later step.
        const XIMStyle wanted[] = {XIMPreeditPosition | XIMStatusNothing, XIMPreeditNothing | XIMStatusNothing,
                                   XIMPreeditNone | XIMStatusNone};
        XIMStyle chosen = 0;
        for (XIMStyle w : wanted) {
            for (unsigned short i = 0; i < styles->count_styles && !chosen; ++i) {
                if (styles->supported_styles[i] == w) chosen = w;
            }
            if (chosen) break;
        }
        x_.XFree(styles);
        if (!chosen) return;
        if (chosen & XIMPreeditPosition) {
            XPoint spot{0, 0};
            XVaNestedList list = x_.XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
            ic_ = x_.XCreateIC(im, XNInputStyle, chosen, XNClientWindow, win_, XNFocusWindow, win_, XNPreeditAttributes,
                               list, nullptr);
            x_.XFree(list);
        } else {
            ic_ = x_.XCreateIC(im, XNInputStyle, chosen, XNClientWindow, win_, XNFocusWindow, win_, nullptr);
        }
        if (!ic_) return;
        ic_style_ = chosen;
        unsigned long filter = 0;
        if (x_.XGetICValues(ic_, XNFilterEvents, &filter, nullptr) == nullptr) {
            x_.XSelectInput(p_.dpy(), win_, kEventMask | static_cast<long>(filter));
        }
    }

    void send_state(long action, Atom a1, Atom a2) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win_;
        ev.xclient.message_type = p_.atoms().net_wm_state;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = action;   // 0 remove, 1 add, 2 toggle
        ev.xclient.data.l[1] = static_cast<long>(a1);
        ev.xclient.data.l[2] = static_cast<long>(a2);
        ev.xclient.data.l[3] = 1;        // source: normal application
        x_.XSendEvent(p_.dpy(), p_.root(), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        x_.XFlush(p_.dpy());
    }

    // ------------------------------------------------------------ XDND (drag and drop)
    struct Dnd {
        Window source = 0;
        Atom target = None;   // the type we read: text/uri-list, else UTF-8 text
        bool accept = false;
        DipPoint pos;
        TimerId timeout = 0;
    } dnd_;

    void xdnd_send(Atom type, long l1, long l2, long l4) {
        if (!dnd_.source) return;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.display = p_.dpy();
        ev.xclient.window = dnd_.source;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(win_);
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[4] = l4;
        x_.XSendEvent(p_.dpy(), dnd_.source, False, NoEventMask, &ev);
        x_.XFlush(p_.dpy());
    }
    void xdnd_reset() {
        if (dnd_.timeout) p_.loop().cancel_timer(dnd_.timeout);
        dnd_ = Dnd{};
    }
    void xdnd_enter(const XClientMessageEvent& c) {
        const Atoms& at = p_.atoms();
        xdnd_reset();
        dnd_.source = static_cast<Window>(c.data.l[0]);
        std::vector<Atom> types;
        if (c.data.l[1] & 1) {   // more than three types: XdndTypeList on the source
            Atom type = None;
            int format = 0;
            unsigned long n = 0, after = 0;
            unsigned char* data = nullptr;
            if (x_.XGetWindowProperty(p_.dpy(), dnd_.source, at.xdnd_type_list, 0, 64, False, at.atom, &type, &format, &n, &after,
                                      &data) == Success &&
                data) {
                if (format == 32)
                    for (unsigned long k = 0; k < n; ++k) types.push_back(reinterpret_cast<Atom*>(data)[k]);
                x_.XFree(data);
            }
        } else {
            for (int k = 2; k < 5; ++k)
                if (c.data.l[k]) types.push_back(static_cast<Atom>(c.data.l[k]));
        }
        for (Atom t : types)
            if (t == at.uri_list) dnd_.target = t;
        if (!dnd_.target)
            for (Atom t : types)
                if (t == at.text_plain_utf8 || t == at.utf8_string) dnd_.target = t;
    }
    void xdnd_position(const XClientMessageEvent& c) {
        if (static_cast<Window>(c.data.l[0]) != dnd_.source) return;
        const int rx = static_cast<int>((c.data.l[2] >> 16) & 0xFFFF), ry = static_cast<int>(c.data.l[2] & 0xFFFF);
        int wx = 0, wy = 0;
        Window child = 0;
        x_.XTranslateCoordinates(p_.dpy(), p_.root(), win_, rx, ry, &wx, &wy, &child);
        const float s = scale_ > 0 ? scale_ : 1;
        dnd_.pos = DipPoint{static_cast<float>(wx) / s, static_cast<float>(wy) / s};
        DragOverEvent d;
        d.pos = dnd_.pos;
        d.has_paths = dnd_.target == p_.atoms().uri_list;
        d.has_text = dnd_.target && !d.has_paths;
        dnd_.accept = dnd_.target && l_.on_drag_over(d) == DropEffect::copy;
        // l1 bit 0: accept, bit 1: keep sending positions (the answer depends on the point).
        xdnd_send(p_.atoms().xdnd_status, dnd_.accept ? 3 : 2, 0, dnd_.accept ? static_cast<long>(p_.atoms().xdnd_action_copy) : 0);
    }
    void xdnd_drop(const XClientMessageEvent& c) {
        if (static_cast<Window>(c.data.l[0]) != dnd_.source) return;
        if (!dnd_.accept) {
            xdnd_send(p_.atoms().xdnd_finished, 0, 0, 0);
            xdnd_reset();
            return;
        }
        const auto t = static_cast<Time>(c.data.l[2]);
        x_.XConvertSelection(p_.dpy(), p_.atoms().xdnd_selection, dnd_.target, p_.atoms().kt_xdnd, win_, t ? t : CurrentTime);
        x_.XFlush(p_.dpy());
        // A source that never answers must not leave the drag hanging.
        dnd_.timeout = p_.loop().add_timer(Clock::now() + std::chrono::seconds(5), [this] {
            dnd_.timeout = 0;
            xdnd_send(p_.atoms().xdnd_finished, 0, 0, 0);
            xdnd_reset();
        });
    }
    void xdnd_data(const XSelectionEvent& se) {
        if (!dnd_.source) return;
        const Atoms& at = p_.atoms();
        std::string bytes;
        if (se.property != None) {
            Atom type = None;
            int format = 0;
            unsigned long n = 0, after = 0;
            unsigned char* data = nullptr;
            if (x_.XGetWindowProperty(p_.dpy(), win_, at.kt_xdnd, 0, 1 << 20, True, AnyPropertyType, &type, &format, &n, &after,
                                      &data) == Success &&
                data) {
                if (format == 8) bytes.assign(reinterpret_cast<const char*>(data), n);
                x_.XFree(data);
            }
        }
        DropEvent e;
        e.pos = dnd_.pos;
        if (dnd_.target == at.uri_list) {
            for (std::string& path : detail::paths_from_uri_list(bytes)) e.paths.emplace_back(std::u8string(path.begin(), path.end()));
        } else {
            e.text.assign(bytes.begin(), bytes.end());
        }
        const bool ok = !e.paths.empty() || !e.text.empty();
        xdnd_send(at.xdnd_finished, ok ? 1 : 0, ok ? static_cast<long>(at.xdnd_action_copy) : 0, 0);
        xdnd_reset();
        if (ok) l_.on_drop(e);   // last: the listener may do anything
    }

    void client_message(XClientMessageEvent& c) {
        const Atoms& at = p_.atoms();
        if (c.message_type == at.xdnd_enter) return xdnd_enter(c);
        if (c.message_type == at.xdnd_position) return xdnd_position(c);
        if (c.message_type == at.xdnd_drop) return xdnd_drop(c);
        if (c.message_type == at.xdnd_leave) {
            if (static_cast<Window>(c.data.l[0]) == dnd_.source) xdnd_reset();
            return;
        }
        if (c.message_type != at.wm_protocols) return;
        const auto proto = static_cast<Atom>(c.data.l[0]);
        if (proto == at.wm_delete_window) {
            l_.on_close_requested();
        } else if (proto == at.net_wm_ping) {
            XEvent reply{};
            reply.xclient = c;
            reply.xclient.window = p_.root();
            x_.XSendEvent(p_.dpy(), p_.root(), False, SubstructureRedirectMask | SubstructureNotifyMask, &reply);
        }
    }

    void key(XKeyEvent& e, bool press) {
        const unsigned keycode = e.keycode;
        KeyEvent k;
        k.key = keycode >= 8 ? detail::key_from_evdev(keycode - 8) : Key::unknown;
        k.native_code = keycode;
        const bool was_down = keycode < 256 && pressed_[keycode];

        if (!press) {
            // Without detectable auto-repeat the server sends release+press pairs with the same
            // time stamp: fold them into one repeat.
            if (!p_.detectable_repeat() && x_.XEventsQueued(p_.dpy(), QueuedAfterReading) > 0) {
                XEvent next;
                x_.XPeekEvent(p_.dpy(), &next);
                if (next.type == KeyPress && next.xkey.keycode == keycode && next.xkey.time == e.time) {
                    folded_repeat_ = true;
                    return;
                }
            }
            if (keycode < 256) pressed_[keycode] = false;
        }
        const bool repeat = press && (was_down || folded_repeat_);
        if (press) {
            folded_repeat_ = false;
            if (keycode < 256) pressed_[keycode] = true;
        }
        k.action = !press ? KeyAction::release : (repeat ? KeyAction::repeat : KeyAction::press);
        sides_ = detail::update_side_mods(sides_, k.key, press);
        k.mods = detail::mods_from_x11_state(e.state, sides_);
        // The state is from before the event: apply the modifier key itself.
        if (const uint16_t side = detail::side_mod_of(k.key)) {
            if (press) {
                k.mods = static_cast<uint16_t>(k.mods | side | detail::generic_from_sides(side));
            } else {
                // Drop the released side; the generic bit stays only if the other side is held.
                const auto rest = static_cast<uint16_t>(k.mods & ~side & ~detail::generic_from_sides(side));
                k.mods = static_cast<uint16_t>(rest | detail::generic_from_sides(static_cast<uint16_t>(rest & 0xFF00)));
            }
        }
        // Logical key: the first group's base keysym, usually the Latin layout.
        const KeySym base = x_.XLookupKeysym(&e, 0);
        k.logical = detail::ascii_lower(detail::keysym_to_ucs(static_cast<uint32_t>(base)));

        if (press) {
            KeySym ks = NoSymbol;
            std::u8string text = lookup(e, ks);
            k.composing = is_dead_keysym(ks);
            k.text = filter_text(text);
        }
        consumed_ = false;
        l_.on_key(k);
        if (press && !consumed_ && !k.text.empty()) l_.on_text(k.text);
    }

    std::u8string lookup(XKeyEvent& e, KeySym& ks) {
        if (ic_ && ime_enabled_) {
            char buf[64];
            Status st = 0;
            int n = x_.Xutf8LookupString(ic_, &e, buf, sizeof buf, &ks, &st);
            if (st == XBufferOverflow && n > 0) {
                std::string big(static_cast<size_t>(n), '\0');
                n = x_.Xutf8LookupString(ic_, &e, big.data(), n, &ks, &st);
                if (st == XLookupChars || st == XLookupBoth) {
                    return std::u8string(reinterpret_cast<const char8_t*>(big.data()), static_cast<size_t>(std::max(n, 0)));
                }
                return {};
            }
            if ((st == XLookupChars || st == XLookupBoth) && n > 0) {
                return std::u8string(reinterpret_cast<const char8_t*>(buf), static_cast<size_t>(n));
            }
            return {};
        }
        char buf[32];
        const int n = x_.XLookupString(&e, buf, sizeof buf, &ks, nullptr);
        // XLookupString returns Latin-1; keysyms outside Latin-1 come through keysym_to_ucs.
        if (n > 0) return detail::latin1_to_utf8(std::string_view(buf, static_cast<size_t>(n)));
        std::u8string out;
        if (const char32_t c = detail::keysym_to_ucs(static_cast<uint32_t>(ks))) detail::utf8_append(out, c);
        return out;
    }

    // A borderless window moves and resizes by itself where the app's hit test says so (the
    // quick terminal's title bar and edges). Done here rather than with _NET_WM_MOVERESIZE, so
    // it works without a window manager too; the press's implicit grab keeps the motion coming.
    struct FrameDrag {
        HitArea area;
        int x0, y0;      // pointer, root coordinates
        int l, t, w, h;  // the window at the press, px
    };
    std::optional<FrameDrag> drag_;
    PxSize min_px_{1, 1};

    bool frame_drag_press(const XButtonEvent& b) {
        if (!quick_ || !hit_test_ || b.button != 1) return false;
        const HitArea a = hit_test_(pos(b.x, b.y));
        if (a == HitArea::client || a == HitArea::minimize || a == HitArea::maximize || a == HitArea::close) return false;
        int rx = 0, ry = 0;
        Window child = 0;
        x_.XTranslateCoordinates(p_.dpy(), win_, p_.root(), 0, 0, &rx, &ry, &child);
        drag_ = FrameDrag{a, b.x_root, b.y_root, rx, ry, px_.w, px_.h};
        l_.on_move_size(true);
        return true;
    }

    void frame_drag_to(int x, int y) {
        const FrameDrag& d = *drag_;
        const int dx = x - d.x0, dy = y - d.y0;
        int l = d.l, t = d.t, w = d.w, h = d.h;
        const HitArea a = d.area;
        if (a == HitArea::caption) {
            l += dx;
            t += dy;
        } else {
            if (a == HitArea::resize_e || a == HitArea::resize_ne || a == HitArea::resize_se) w = std::max(min_px_.w, d.w + dx);
            if (a == HitArea::resize_w || a == HitArea::resize_nw || a == HitArea::resize_sw) {
                w = std::max(min_px_.w, d.w - dx);
                l = d.l + d.w - w;
            }
            if (a == HitArea::resize_s || a == HitArea::resize_sw || a == HitArea::resize_se) h = std::max(min_px_.h, d.h + dy);
            if (a == HitArea::resize_n || a == HitArea::resize_nw || a == HitArea::resize_ne) {
                h = std::max(min_px_.h, d.h - dy);
                t = d.t + d.h - h;
            }
        }
        x_.XMoveResizeWindow(p_.dpy(), win_, l, t, static_cast<unsigned>(w), static_cast<unsigned>(h));
    }

    void button(XButtonEvent& b) {
        if (b.type == ButtonPress && !drag_ && frame_drag_press(b)) return;
        if (b.type == ButtonRelease && drag_ && b.button == 1) {
            frame_drag_to(b.x_root, b.y_root);
            drag_.reset();
            l_.on_move_size(false);
            return;
        }
        if (drag_) return;
        const uint16_t mods = detail::mods_from_x11_state(b.state, sides_);
        const DipPoint p = pos(b.x, b.y);
        if (b.button >= 4 && b.button <= 7) {
            if (b.type != ButtonPress) return;
            ScrollEvent s;
            s.mods = mods;
            s.pos = p;
            switch (b.button) {
                case 4: s.dy = 1; break;
                case 5: s.dy = -1; break;
                case 6: s.dx = 1; break;
                default: s.dx = -1; break;
            }
            l_.on_scroll(s);
            return;
        }
        MouseEvent m;
        m.type = b.type == ButtonPress ? MouseEvent::Type::press : MouseEvent::Type::release;
        m.mods = mods;
        m.pos = p;
        switch (b.button) {
            case 1: m.button = MouseButton::left; break;
            case 2: m.button = MouseButton::middle; break;
            case 3: m.button = MouseButton::right; break;
            case 8: m.button = MouseButton::back; break;
            case 9: m.button = MouseButton::forward; break;
            default: return;
        }
        if (m.type == MouseEvent::Type::press) {
            m.click_count = clicks_.press(m.button, static_cast<uint32_t>(b.time), p);
        }
        l_.on_mouse(m);
    }

    void schedule_frame() {
        if (occluded_) return;
        const auto due = pacer_.due(Clock::now());
        frame_timer_ = p_.loop().add_timer(due, [this] {
            frame_timer_ = 0;
            if (!frame_pending_ || occluded_ || !win_) return;
            frame_pending_ = false;
            l_.on_frame(pacer_.delivered(Clock::now()));
        });
    }

    void destroy(bool notify) {
        if (dnd_.timeout) {
            p_.loop().cancel_timer(dnd_.timeout);
            dnd_.timeout = 0;
        }
        if (frame_timer_) {
            p_.loop().cancel_timer(frame_timer_);
            frame_timer_ = 0;
        }
        const bool had = win_ != 0 || notify_pending_;
        if (ic_) {
            x_.XDestroyIC(ic_);
            ic_ = nullptr;
        }
        destroy_surface();
        if (win_) {
            p_.unregister_window(win_);
            x_.XDestroyWindow(p_.dpy(), win_);
            x_.XFlush(p_.dpy());
            win_ = 0;
        }
        notify_pending_ = false;
        if (notify && had) l_.on_destroyed();   // last: the listener may delete this window
    }

    X11Platform& p_;
    const Api& x_;
    IWindowListener& l_;
    Window win_ = 0;
    Window child_ = 0;
    XIC ic_ = nullptr;
    XIMStyle ic_style_ = 0;
    float scale_ = 1;
    PxSize px_;
    bool focused_ = false;
    bool quick_ = false;          // WindowKind::quick_terminal
    bool focus_on_map_ = false;
    bool occluded_ = true;   // until the first MapNotify
    bool fullscreen_ = false;
    bool urgent_ = false;   // _NET_WM_STATE_DEMANDS_ATTENTION set by request_attention()
    bool frame_pending_ = false;
    bool notify_pending_ = false;
    TimerId frame_timer_ = 0;
    detail::FramePacer pacer_;   // 60 Hz until XRandR refresh rates are read (later step)
    detail::ClickCounter clicks_;
    std::function<HitArea(DipPoint)> hit_test_;
    bool ime_enabled_ = true;
    DipRect caret_;
    bool consumed_ = false;
    bool folded_repeat_ = false;
    uint16_t sides_ = 0;
    bool pressed_[256] = {};
};

void dispatch_to_window(X11Window& w, XEvent& e) { w.handle(e); }

void window_scale_changed(X11Window& w, float scale) { w.scale_changed(scale); }

Result<std::unique_ptr<IWindow>> create_x11_window(X11Platform& p, const WindowDesc& d, IWindowListener& l) {
    auto w = std::make_unique<X11Window>(p, l);
    if (auto r = w->init(d); !r) return Unexpected(r.error());
    return std::unique_ptr<IWindow>(std::move(w));
}

}  // namespace kt::platform::x11
