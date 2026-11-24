// X11 platform: display connection, the poll() event loop, XIM, the CLIPBOARD and PRIMARY
// selections.
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

#include <X11/Xatom.h>

#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "base/log.h"
#include "platform_gui/dpi.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/utf.h"
#include "platform_gui/x11_internal_linux.h"

namespace kt::platform::x11 {

Result<std::unique_ptr<IPlatform>> create_x11_platform(const PlatformOptions& opts);

namespace {

constexpr auto kClipboardTimeout = std::chrono::seconds(2);

int on_x_error(Display* dpy, XErrorEvent* e) {
    // Requests on windows that the WM destroyed concurrently are routine; never exit here.
    char text[128] = {};
    if (const Api* x = api()) x->XGetErrorText(dpy, e->error_code, text, sizeof text);
    log::write(log::Level::debug, "x11",
               std::string("X error: ") + text + " (request " + std::to_string(e->request_code) + ")");
    return 0;
}

int on_x_io_error(Display*) {
    // The X server went away: sessions keep running in their hosts.
    log::error("x11", "display connection lost");
    std::_Exit(4);
}

[[maybe_unused]] bool set_nonblock_cloexec(int fd) {   // the pipe fallback off Linux
    const int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return false;
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

}  // namespace

// ---------------------------------------------------------------- X11Loop

X11Loop::X11Loop(X11Platform& p) : p_(p) {}

X11Loop::~X11Loop() {
    if (wake_read_ >= 0) close(wake_read_);
    if (wake_write_ >= 0 && wake_write_ != wake_read_) close(wake_write_);
}

bool X11Loop::init() {
#if defined(__linux__)
    wake_read_ = wake_write_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return wake_read_ >= 0;
#else
    int fds[2];
    if (pipe(fds) != 0) return false;
    wake_read_ = fds[0];
    wake_write_ = fds[1];
    return set_nonblock_cloexec(wake_read_) && set_nonblock_cloexec(wake_write_);
#endif
}

void X11Loop::drain_wake_fd() {
    uint64_t buf[8];
    while (read(wake_read_, buf, sizeof buf) > 0) {
    }
}

int X11Loop::run() {
    Display* dpy = p_.dpy();
    const Api& x = p_.x();
    const int xfd = x.XConnectionNumber(dpy);
    std::vector<pollfd> fds;
    while (!quit_) {
        p_.dispatch_pending();
        tasks_.drain();
        timers_.run_due(Clock::now());
        if (quit_) break;

        // Events that arrived while handling the above (XSync in a presenter, a round trip)
        // sit in Xlib's queue, not in the socket: never sleep on them.
        int timeout = detail::wait_timeout_ms(timers_.next_deadline(), Clock::now(), 60'000);
        if (!tasks_.empty() || x.XEventsQueued(dpy, QueuedAfterFlush) > 0) timeout = 0;

        fds.clear();
        fds.push_back(pollfd{xfd, POLLIN, 0});
        fds.push_back(pollfd{wake_read_, POLLIN, 0});
        for (const Waiter& w : waiters_) fds.push_back(pollfd{w.fd, POLLIN, 0});
        const int n = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            log::error("x11", std::string("poll failed: ") + std::strerror(errno));
            return 1;
        }
        if (fds[1].revents) drain_wake_fd();
        // Copy: a callback may add or remove waitables.
        std::vector<std::shared_ptr<std::function<void()>>> ready;
        for (size_t i = 2; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(waiters_[i - 2].fn);
        }
        for (auto& fn : ready) (*fn)();
    }
    quit_ = false;
    return exit_code_;
}

void X11Loop::quit(int exit_code) {
    exit_code_ = exit_code;
    quit_ = true;
}

void X11Loop::post(std::function<void()> task) {
    if (tasks_.push(std::move(task))) {
        const uint64_t one = 1;
        ssize_t r;
        do {
            r = write(wake_write_, &one, sizeof one);
        } while (r < 0 && errno == EINTR);
    }
}

TimerId X11Loop::add_timer(Clock::time_point deadline, std::function<void()> fn) {
    return timers_.add(deadline, std::move(fn));
}

void X11Loop::cancel_timer(TimerId id) { timers_.cancel(id); }

Result<void> X11Loop::add_waitable(Waitable fd, std::function<void()> on_signalled) {
    if (fd < 0) return make_error(Errc::invalid_argument, "negative fd");
    for (const Waiter& w : waiters_) {
        if (w.fd == fd) return make_error(Errc::busy, "fd already registered");
    }
    waiters_.push_back(Waiter{fd, std::make_shared<std::function<void()>>(std::move(on_signalled))});
    return {};
}

void X11Loop::remove_waitable(Waitable fd) {
    std::erase_if(waiters_, [fd](const Waiter& w) { return w.fd == fd; });
}

// ---------------------------------------------------------------- X11Clipboard

X11Clipboard::~X11Clipboard() { shutdown(); }

void X11Clipboard::shutdown() {
    for (int idx = 0; idx < 3; ++idx) {
        if (reads_[static_cast<size_t>(idx)].active) finish(idx, make_error(Errc::unavailable, "platform shut down"));
    }
    if (win_ && p_.dpy()) p_.x().XDestroyWindow(p_.dpy(), win_);
    win_ = 0;
}

bool X11Clipboard::init() {
    XSetWindowAttributes a{};
    a.event_mask = PropertyChangeMask;
    win_ = p_.x().XCreateWindow(p_.dpy(), p_.root(), -10, -10, 1, 1, 0, 0, InputOnly, nullptr, CWEventMask, &a);
    return win_ != 0;
}

Atom X11Clipboard::sel_atom(Selection s) const {
    return s == Selection::clipboard ? p_.atoms().clipboard : p_.atoms().primary;
}

int X11Clipboard::index_of(Atom selection) const {
    if (selection == p_.atoms().clipboard) return 0;
    if (selection == p_.atoms().primary) return 1;
    return -1;
}

Result<void> X11Clipboard::write_text(Selection s, std::u8string_view text, bool /*sensitive*/) {
    // X11 has no clipboard history protocol; `sensitive` has nothing to mark.
    const int idx = s == Selection::clipboard ? 0 : 1;
    const Atom a = sel_atom(s);
    owned_[static_cast<size_t>(idx)] = std::u8string(text);
    p_.x().XSetSelectionOwner(p_.dpy(), a, win_, p_.last_time());
    if (p_.x().XGetSelectionOwner(p_.dpy(), a) != win_) {
        owns_[static_cast<size_t>(idx)] = false;
        owned_[static_cast<size_t>(idx)].clear();
        return make_error(Errc::busy, "could not take the selection");
    }
    owns_[static_cast<size_t>(idx)] = true;
    return {};
}

void X11Clipboard::read_text(Selection s, std::function<void(Result<std::u8string>)> cb) {
    const int idx = s == Selection::clipboard ? 0 : 1;
    const auto i = static_cast<size_t>(idx);
    if (owns_[i]) {
        p_.loop().post([cb = std::move(cb), text = owned_[i]] { cb(text); });
        return;
    }
    reads_[i].waiters.push_back([cb = std::move(cb)](Result<std::string> r) {
        if (!r) {
            cb(make_error(r.error().code, r.error().what));
            return;
        }
        cb(std::u8string(reinterpret_cast<const char8_t*>(r->data()), r->size()));
    });
    if (!reads_[i].active) start_read(idx, p_.atoms().utf8_string);
}

void X11Clipboard::read_image(std::function<void(Result<std::vector<uint8_t>>)> cb) {
    if (owns_[0]) {   // our own clipboard holds text only
        p_.loop().post([cb = std::move(cb)] { cb(std::vector<uint8_t>{}); });
        return;
    }
    reads_[2].waiters.push_back([cb = std::move(cb)](Result<std::string> r) {
        if (!r) {
            cb(make_error(r.error().code, r.error().what));
            return;
        }
        static constexpr char kSig[] = "\x89PNG\r\n\x1a\n";
        if (r->size() < 8 || r->compare(0, 8, kSig, 8) != 0) {
            cb(std::vector<uint8_t>{});
            return;
        }
        cb(std::vector<uint8_t>(r->begin(), r->end()));
    });
    if (!reads_[2].active) start_read(2, p_.atoms().image_png);
}

Atom X11Clipboard::prop_of(int idx) const { return idx == 2 ? p_.atoms().kt_image : p_.atoms().kt_selection; }

void X11Clipboard::start_read(int idx, Atom target) {
    Read& r = reads_[static_cast<size_t>(idx)];
    r.active = true;
    r.incr = false;
    r.target = target;
    r.data.clear();
    const Atom sel = idx == 1 ? p_.atoms().primary : p_.atoms().clipboard;
    p_.x().XDeleteProperty(p_.dpy(), win_, prop_of(idx));
    p_.x().XConvertSelection(p_.dpy(), sel, target, prop_of(idx), win_, p_.last_time());
    if (r.timeout) p_.loop().cancel_timer(r.timeout);
    r.timeout = p_.loop().add_timer(Clock::now() + kClipboardTimeout, [this, idx] {
        reads_[static_cast<size_t>(idx)].timeout = 0;
        finish(idx, make_error(Errc::timeout, "selection owner did not answer"));
    });
}

void X11Clipboard::finish_data(int idx, std::string data) {
    if (reads_[static_cast<size_t>(idx)].target == p_.atoms().string) {
        const std::u8string u = detail::latin1_to_utf8(data);
        data.assign(reinterpret_cast<const char*>(u.data()), u.size());
    }
    finish(idx, std::move(data));
}

void X11Clipboard::finish(int idx, Result<std::string> result) {
    Read& r = reads_[static_cast<size_t>(idx)];
    if (r.timeout) p_.loop().cancel_timer(r.timeout);
    r.timeout = 0;
    r.active = false;
    r.incr = false;
    r.data.clear();
    auto waiters = std::move(r.waiters);
    r.waiters.clear();
    for (auto& w : waiters) w(result);
}

bool X11Clipboard::read_property(Atom prop, std::string& out, Atom& type, bool& too_big) {
    Atom actual = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    const long max_longs = static_cast<long>(kMaxBytes / 4);
    if (p_.x().XGetWindowProperty(p_.dpy(), win_, prop, 0, max_longs, True, AnyPropertyType,
                                  &actual, &format, &items, &after, &data) != Success) {
        return false;
    }
    type = actual;
    too_big = after > 0;
    if (data) {
        const size_t bytes = items * static_cast<size_t>(format / 8);
        out.assign(reinterpret_cast<const char*>(data), bytes);
        p_.x().XFree(data);
    } else {
        out.clear();
    }
    return true;
}

void X11Clipboard::on_selection_notify(const XSelectionEvent& e) {
    int idx = index_of(e.selection);
    if (idx < 0) return;
    if (idx == 0 && reads_[2].active && e.target == reads_[2].target) idx = 2;
    Read& r = reads_[static_cast<size_t>(idx)];
    if (!r.active) return;
    if (e.property == None) {
        if (r.target == p_.atoms().utf8_string) {
            start_read(idx, p_.atoms().string);   // old owners: Latin-1 STRING
            return;
        }
        finish(idx, std::string());   // nothing of that type in the selection
        return;
    }
    std::string data;
    Atom type = None;
    bool too_big = false;
    if (!read_property(prop_of(idx), data, type, too_big)) {
        finish(idx, make_error(Errc::os_error, "XGetWindowProperty failed"));
        return;
    }
    if (type == p_.atoms().incr) {
        // INCR: the owner sends chunks on PropertyNotify(NewValue); the delete above asked
        // for the first one (ICCCM §2.7.2).
        r.incr = true;
        r.data.clear();
        return;
    }
    if (too_big) {
        finish(idx, make_error(Errc::invalid_argument, "selection exceeds the size limit"));
        return;
    }
    finish_data(idx, std::move(data));
}

void X11Clipboard::on_property_new(const XPropertyEvent& e) {
    if (e.state != PropertyNewValue) return;
    for (int idx = 0; idx < 3; ++idx) {
        Read& r = reads_[static_cast<size_t>(idx)];
        if (!r.active || !r.incr || e.atom != prop_of(idx)) continue;
        std::string chunk;
        Atom type = None;
        bool too_big = false;
        if (!read_property(prop_of(idx), chunk, type, too_big)) {
            finish(idx, make_error(Errc::os_error, "XGetWindowProperty failed"));
            return;
        }
        if (chunk.empty()) {   // a zero-length chunk ends the transfer
            finish_data(idx, std::move(r.data));
            return;
        }
        if (r.data.size() + chunk.size() > kMaxBytes) {
            finish(idx, make_error(Errc::invalid_argument, "selection exceeds the size limit"));
            return;
        }
        r.data += chunk;
        // Keep the timeout fresh while chunks arrive.
        if (r.timeout) p_.loop().cancel_timer(r.timeout);
        r.timeout = p_.loop().add_timer(Clock::now() + kClipboardTimeout, [this, idx] {
            reads_[static_cast<size_t>(idx)].timeout = 0;
            finish(idx, make_error(Errc::timeout, "selection owner stopped sending"));
        });
        return;
    }
}

void X11Clipboard::on_request(const XSelectionRequestEvent& req) {
    const Atoms& a = p_.atoms();
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = req.display;
    reply.xselection.requestor = req.requestor;
    reply.xselection.selection = req.selection;
    reply.xselection.target = req.target;
    reply.xselection.time = req.time;
    reply.xselection.property = None;
    const Atom prop = req.property != None ? req.property : req.target;   // obsolete clients
    const int idx = index_of(req.selection);
    if (idx >= 0 && owns_[static_cast<size_t>(idx)]) {
        const std::u8string& text = owned_[static_cast<size_t>(idx)];
        // Larger transfers need INCR, which is not implemented for writes yet.
        const size_t max_bytes = static_cast<size_t>(p_.x().XExtendedMaxRequestSize(p_.dpy()) > 0
                                                         ? p_.x().XExtendedMaxRequestSize(p_.dpy())
                                                         : p_.x().XMaxRequestSize(p_.dpy())) * 4 - 64;
        if (req.target == a.targets) {
            const Atom targets[] = {a.targets, a.utf8_string, a.text, a.string};
            p_.x().XChangeProperty(p_.dpy(), req.requestor, prop, a.atom, 32, PropModeReplace,
                                   reinterpret_cast<const unsigned char*>(targets), 4);
            reply.xselection.property = prop;
        } else if ((req.target == a.utf8_string || req.target == a.text) && text.size() <= max_bytes) {
            p_.x().XChangeProperty(p_.dpy(), req.requestor, prop, a.utf8_string, 8, PropModeReplace,
                                   reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
            reply.xselection.property = prop;
        } else if (req.target == a.string && text.size() <= max_bytes) {
            std::string latin1;
            std::u8string_view v = text;
            while (!v.empty()) {
                size_t n = 0;
                const char32_t c = detail::utf8_first(v, &n);
                v.remove_prefix(n);
                latin1.push_back(c < 0x100 ? static_cast<char>(c) : '?');
            }
            p_.x().XChangeProperty(p_.dpy(), req.requestor, prop, a.string, 8, PropModeReplace,
                                   reinterpret_cast<const unsigned char*>(latin1.data()), static_cast<int>(latin1.size()));
            reply.xselection.property = prop;
        } else if (text.size() > max_bytes) {
            log::warn("x11", "clipboard text too large for a single property; INCR writes are not implemented");
        }
    }
    p_.x().XSendEvent(p_.dpy(), req.requestor, False, 0, &reply);
    p_.x().XFlush(p_.dpy());
}

void X11Clipboard::handle(const XEvent& e) {
    switch (e.type) {
        case SelectionNotify: on_selection_notify(e.xselection); break;
        case SelectionRequest: on_request(e.xselectionrequest); break;
        case SelectionClear: {
            const int idx = index_of(e.xselectionclear.selection);
            if (idx >= 0) {
                owns_[static_cast<size_t>(idx)] = false;
                owned_[static_cast<size_t>(idx)].clear();
            }
            break;
        }
        case PropertyNotify: on_property_new(e.xproperty); break;
        default: break;
    }
}

// ---------------------------------------------------------------- X11Platform

X11Platform::X11Platform(const PlatformOptions& opts, const Api* x)
    : x_(x), loop_(*this), clipboard_(*this) {
    app_id_ = opts.app_id + (opts.profile.empty() ? "" : "." + opts.profile);
    info_.ws = WindowSystem::x11;
    info_.elevated = geteuid() == 0;
    utsname u{};
    if (uname(&u) == 0) info_.os_version = u.release;
}

X11Platform::~X11Platform() {
    if (!dpy_) return;
    for (Cursor c : cursors_) {
        if (c) x_->XFreeCursor(dpy_, c);
    }
    if (im_) x_->XCloseIM(im_);
    clipboard_.shutdown();   // the helper window must go before the connection
    x_->XCloseDisplay(dpy_);
    dpy_ = nullptr;
}

Result<void> X11Platform::init() {
    // XIM needs a locale that Xlib supports; a GUI process started with LC_CTYPE=C gets the
    // user's locale for character classification only (LC_CTYPE, nothing else).
    if (const char* cur = std::setlocale(LC_CTYPE, nullptr); cur && (!std::strcmp(cur, "C") || !std::strcmp(cur, "POSIX"))) {
        std::setlocale(LC_CTYPE, "");
    }
    dpy_ = x_->XOpenDisplay(nullptr);
    if (!dpy_) return make_error(Errc::unavailable, "cannot open the X display");
    x_->XSetErrorHandler(&on_x_error);
    x_->XSetIOErrorHandler(&on_x_io_error);
    screen_ = x_->XDefaultScreen(dpy_);
    root_ = x_->XRootWindow(dpy_, screen_);
    if (fcntl(x_->XConnectionNumber(dpy_), F_SETFD, FD_CLOEXEC) != 0) {
        log::warn("x11", "cannot set FD_CLOEXEC on the X connection");
    }

    auto atom = [&](const char* name) { return x_->XInternAtom(dpy_, name, False); };
    atoms_.clipboard = atom("CLIPBOARD");
    atoms_.primary = XA_PRIMARY;
    atoms_.utf8_string = atom("UTF8_STRING");
    atoms_.targets = atom("TARGETS");
    atoms_.text = atom("TEXT");
    atoms_.string = XA_STRING;
    atoms_.incr = atom("INCR");
    atoms_.kt_selection = atom("KT_SELECTION");
    atoms_.atom = XA_ATOM;
    atoms_.cardinal = XA_CARDINAL;
    atoms_.xdnd_aware = atom("XdndAware");
    atoms_.xdnd_enter = atom("XdndEnter");
    atoms_.xdnd_position = atom("XdndPosition");
    atoms_.xdnd_status = atom("XdndStatus");
    atoms_.xdnd_leave = atom("XdndLeave");
    atoms_.xdnd_drop = atom("XdndDrop");
    atoms_.xdnd_finished = atom("XdndFinished");
    atoms_.xdnd_selection = atom("XdndSelection");
    atoms_.xdnd_type_list = atom("XdndTypeList");
    atoms_.xdnd_action_copy = atom("XdndActionCopy");
    atoms_.uri_list = atom("text/uri-list");
    atoms_.image_png = atom("image/png");
    atoms_.kt_image = atom("KT_IMAGE");
    atoms_.text_plain_utf8 = atom("text/plain;charset=utf-8");
    atoms_.kt_xdnd = atom("KT_XDND");
    atoms_.wm_protocols = atom("WM_PROTOCOLS");
    atoms_.wm_delete_window = atom("WM_DELETE_WINDOW");
    atoms_.net_wm_ping = atom("_NET_WM_PING");
    atoms_.net_wm_name = atom("_NET_WM_NAME");
    atoms_.net_wm_pid = atom("_NET_WM_PID");
    atoms_.net_wm_icon = atom("_NET_WM_ICON");
    atoms_.net_wm_state = atom("_NET_WM_STATE");
    atoms_.net_wm_state_fullscreen = atom("_NET_WM_STATE_FULLSCREEN");
    atoms_.net_wm_state_demands_attention = atom("_NET_WM_STATE_DEMANDS_ATTENTION");
    atoms_.net_wm_state_max_vert = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    atoms_.net_wm_state_max_horz = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    atoms_.net_wm_window_type = atom("_NET_WM_WINDOW_TYPE");
    atoms_.net_wm_window_type_normal = atom("_NET_WM_WINDOW_TYPE_NORMAL");
    atoms_.resource_manager = XA_RESOURCE_MANAGER;
    atoms_.net_wm_state_above = atom("_NET_WM_STATE_ABOVE");
    atoms_.net_wm_state_sticky = atom("_NET_WM_STATE_STICKY");
    atoms_.net_wm_state_skip_taskbar = atom("_NET_WM_STATE_SKIP_TASKBAR");
    atoms_.net_wm_state_skip_pager = atom("_NET_WM_STATE_SKIP_PAGER");
    atoms_.motif_wm_hints = atom("_MOTIF_WM_HINTS");
    atoms_.net_active_window = atom("_NET_ACTIVE_WINDOW");
    atoms_.net_workarea = atom("_NET_WORKAREA");
    atoms_.net_current_desktop = atom("_NET_CURRENT_DESKTOP");

    if (x_->XkbSetDetectableAutoRepeat) {
        Bool supported = False;
        x_->XkbSetDetectableAutoRepeat(dpy_, True, &supported);
        detectable_repeat_ = supported != False;
    }

    // XIM: XMODIFIERS=@im=ibus / fcitx is honoured through the modifiers.
    if (x_->has_xim() && (!x_->XSupportsLocale || x_->XSupportsLocale())) {
        if (x_->XSetLocaleModifiers) x_->XSetLocaleModifiers("");
        im_ = x_->XOpenIM(dpy_, nullptr, nullptr, nullptr);
        if (!im_ && x_->XSetLocaleModifiers) {
            x_->XSetLocaleModifiers("@im=none");   // the built-in compose handling only
            im_ = x_->XOpenIM(dpy_, nullptr, nullptr, nullptr);
        }
    }
    if (!im_) log::info("x11", "no input method; dead keys and compose are unavailable");

    // Xft.dpi changes arrive as RESOURCE_MANAGER property changes on the root window.
    x_->XSelectInput(dpy_, root_, PropertyChangeMask);
    reload_scale(false);

    if (!loop_.init()) return last_os_error("eventfd failed");
    if (!clipboard_.init()) return make_error(Errc::os_error, "cannot create the clipboard window");
    return {};
}

std::string X11Platform::read_resource_manager() {
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    std::string out;
    if (x_->XGetWindowProperty(dpy_, root_, atoms_.resource_manager, 0, 1 << 16, False, XA_STRING, &type, &format,
                               &items, &after, &data) == Success && data) {
        out.assign(reinterpret_cast<const char*>(data), items);
        x_->XFree(data);
    }
    return out;
}

void X11Platform::reload_scale(bool notify) {
    const float s = detail::x11_scale(read_resource_manager(), std::getenv("GDK_SCALE"));
    if (s == scale_) return;
    scale_ = s;
    if (!notify) return;
    // Copy: a listener may close windows.
    std::vector<X11Window*> ws;
    for (auto& [id, w] : windows_) ws.push_back(w);
    for (X11Window* w : ws) {
        bool alive = false;
        for (auto& [id, w2] : windows_) alive = alive || w2 == w;
        if (alive) window_scale_changed(*w, s);
    }
}

Cursor X11Platform::cursor(CursorShape s) {
    const auto i = static_cast<size_t>(s);
    if (i >= cursors_.size()) return 0;
    if (cursors_[i]) return cursors_[i];
    // Font cursor glyphs (X11/cursorfont.h).
    unsigned shape = 68;   // XC_left_ptr
    switch (s) {
        case CursorShape::arrow: shape = 68; break;
        case CursorShape::ibeam: shape = 152; break;        // XC_xterm
        case CursorShape::hand: shape = 60; break;          // XC_hand2
        case CursorShape::crosshair: shape = 34; break;     // XC_crosshair
        case CursorShape::wait: shape = 150; break;         // XC_watch
        case CursorShape::move: shape = 52; break;          // XC_fleur
        case CursorShape::resize_ew: shape = 108; break;    // XC_sb_h_double_arrow
        case CursorShape::resize_ns: shape = 116; break;    // XC_sb_v_double_arrow
        case CursorShape::resize_nesw: shape = 12; break;   // XC_bottom_left_corner
        case CursorShape::resize_nwse: shape = 14; break;   // XC_bottom_right_corner
        case CursorShape::frame_n: shape = 138; break;      // XC_top_side
        case CursorShape::frame_s: shape = 16; break;       // XC_bottom_side
        case CursorShape::frame_e: shape = 96; break;       // XC_right_side
        case CursorShape::frame_w: shape = 70; break;       // XC_left_side
        case CursorShape::frame_ne: shape = 136; break;     // XC_top_right_corner
        case CursorShape::frame_nw: shape = 134; break;     // XC_top_left_corner
        case CursorShape::frame_se: shape = 14; break;      // XC_bottom_right_corner
        case CursorShape::frame_sw: shape = 12; break;      // XC_bottom_left_corner
        case CursorShape::hidden: {
            const Pixmap pm = x_->XCreatePixmap(dpy_, root_, 1, 1, 1);
            XColor black{};
            cursors_[i] = x_->XCreatePixmapCursor(dpy_, pm, pm, &black, &black, 0, 0);
            x_->XFreePixmap(dpy_, pm);
            return cursors_[i];
        }
    }
    cursors_[i] = x_->XCreateFontCursor(dpy_, shape);
    return cursors_[i];
}

void X11Platform::register_window(Window w, X11Window* win) { windows_[w] = win; }

void X11Platform::unregister_window(Window w) { windows_.erase(w); }

void X11Platform::dispatch_pending() {
    while (x_->XPending(dpy_) > 0) {
        XEvent e;
        x_->XNextEvent(dpy_, &e);
        // The input method sees every event first.
        if (x_->XFilterEvent(&e, None)) continue;
        handle(e);
    }
}

void X11Platform::handle(XEvent& e) {
    switch (e.type) {
        case KeyPress:
        case KeyRelease: note_time(e.xkey.time); break;
        case ButtonPress:
        case ButtonRelease: note_time(e.xbutton.time); break;
        case MotionNotify: note_time(e.xmotion.time); break;
        case PropertyNotify: note_time(e.xproperty.time); break;
        default: break;
    }
    const Window w = e.xany.window;
    if (w == clipboard_.window() || e.type == SelectionRequest || e.type == SelectionClear) {
        clipboard_.handle(e);
        return;
    }
    if ((e.type == KeyPress || e.type == KeyRelease) && !grabs_.empty() && handle_grab(e.xkey)) return;
    if (w == root_) {
        if (e.type == PropertyNotify && e.xproperty.atom == atoms_.resource_manager) reload_scale(true);
        return;
    }
    auto it = windows_.find(w);
    if (it != windows_.end()) dispatch_to_window(*it->second, e);
}

// ---------------------------------------------------------------- global hotkeys, screens

namespace {

// BadAccess from XGrabKey arrives asynchronously: a temporary handler after XSync catches it.
int g_grab_error = 0;
int on_grab_error(Display*, XErrorEvent* e) {
    if (e->request_code == 33 /* X_GrabKey */) g_grab_error = e->error_code;
    return 0;
}

// Caps Lock and Num Lock must not change the chord.
constexpr unsigned kLockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

unsigned x11_mods(uint16_t m) {
    unsigned x = 0;
    if (m & mod_ctrl) x |= ControlMask;
    if (m & mod_shift) x |= ShiftMask;
    if (m & mod_alt) x |= Mod1Mask;
    if (m & mod_super) x |= Mod4Mask;
    return x;
}

// libXrandr (dlopen, optional). XRRMonitorInfo as in Xrandr.h (RandR 1.5).
struct RRMonitor {
    Atom name;
    Bool primary, automatic;
    int noutput, x, y, width, height, mwidth, mheight;
    unsigned long* outputs;
};
struct Randr {
    RRMonitor* (*get)(Display*, Window, Bool, int*) = nullptr;
    void (*free)(RRMonitor*) = nullptr;
};
const Randr& randr() {
    static const Randr r = [] {
        Randr o;
        if (void* lib = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL)) {
            o.get = reinterpret_cast<RRMonitor* (*)(Display*, Window, Bool, int*)>(dlsym(lib, "XRRGetMonitors"));
            o.free = reinterpret_cast<void (*)(RRMonitor*)>(dlsym(lib, "XRRFreeMonitors"));
            if (!o.get || !o.free) o = Randr{};
        }
        return o;
    }();
    return r;
}

}  // namespace

Result<HotkeyId> X11Platform::register_global_hotkey(GlobalHotkey hk, std::function<void()> fn) {
    if (!x_->XGrabKey || !x_->XUngrabKey) return make_error(Errc::not_supported, "XGrabKey is missing");
    int evdev = -1;
    for (int c = 1; c < 248 && evdev < 0; ++c)
        if (detail::key_from_evdev(static_cast<uint32_t>(c)) == hk.key) evdev = c;
    if (evdev < 0) return make_error(Errc::invalid_argument, "this key cannot be a global hotkey");
    Grab g;
    g.keycode = evdev + 8;   // X keycodes are evdev codes plus 8 (xf86-input-evdev, libinput)
    g.mods = x11_mods(hk.mods);
    g.fn = std::move(fn);
    x_->XSync(dpy_, False);
    g_grab_error = 0;
    auto* old = x_->XSetErrorHandler(&on_grab_error);
    for (unsigned v : kLockVariants) x_->XGrabKey(dpy_, g.keycode, g.mods | v, root_, False, GrabModeAsync, GrabModeAsync);
    x_->XSync(dpy_, False);
    x_->XSetErrorHandler(old);
    if (g_grab_error) {
        for (unsigned v : kLockVariants) x_->XUngrabKey(dpy_, g.keycode, g.mods | v, root_);
        x_->XFlush(dpy_);
        if (g_grab_error == BadAccess) return make_error(Errc::busy, "the shortcut is used by another app");
        return make_error(Errc::unavailable, "XGrabKey failed (" + std::to_string(g_grab_error) + ")");
    }
    const HotkeyId id = next_grab_++;
    grabs_[id] = std::move(g);
    return id;
}

void X11Platform::unregister_global_hotkey(HotkeyId id) {
    auto it = grabs_.find(id);
    if (it == grabs_.end()) return;
    for (unsigned v : kLockVariants) x_->XUngrabKey(dpy_, it->second.keycode, it->second.mods | v, root_);
    x_->XFlush(dpy_);
    grabs_.erase(it);
}

bool X11Platform::handle_grab(const XKeyEvent& k) {
    const unsigned mods = k.state & (ControlMask | ShiftMask | Mod1Mask | Mod4Mask);
    for (auto& [id, g] : grabs_) {
        if (static_cast<int>(k.keycode) != g.keycode) continue;
        if (k.type == KeyRelease) {
            if (!g.down) continue;
            g.down = false;
            return k.window == root_;
        }
        if (mods != g.mods) continue;
        if (g.down) return true;   // auto-repeat
        g.down = true;
        auto fn = g.fn;   // may unregister itself
        if (fn) fn();
        return true;
    }
    return false;
}

std::optional<ScreenArea> X11Platform::screen_area(ScreenPick pick) const {
    XWindowAttributes ra{};
    if (!x_->XGetWindowAttributes(dpy_, root_, &ra)) return std::nullopt;
    int px = 0, py = 0;
    if (pick != ScreenPick::primary && x_->XQueryPointer) {
        Window r = 0, c = 0;
        int wx = 0, wy = 0;
        unsigned m = 0;
        x_->XQueryPointer(dpy_, root_, &r, &c, &px, &py, &wx, &wy, &m);
    }
    // The monitor: RandR 1.5 monitors when libXrandr is there, else the whole root window.
    int mx = 0, my = 0, mw = ra.width, mh = ra.height;
    std::string key = "screen@" + std::to_string(ra.width) + "x" + std::to_string(ra.height);
    if (const Randr& rr = randr(); rr.get) {
        int n = 0;
        if (RRMonitor* ms = rr.get(dpy_, root_, True, &n)) {
            int pick_i = -1;
            for (int i = 0; i < n; ++i) {
                const RRMonitor& m = ms[i];
                const bool under = px >= m.x && px < m.x + m.width && py >= m.y && py < m.y + m.height;
                if (pick == ScreenPick::primary ? m.primary : under) pick_i = i;
            }
            if (pick_i < 0 && n > 0) pick_i = 0;
            if (pick_i >= 0) {
                const RRMonitor& m = ms[pick_i];
                mx = m.x, my = m.y, mw = m.width, mh = m.height;
                key = "monitor" + std::to_string(pick_i) + "@" + std::to_string(mw) + "x" + std::to_string(mh) + "+" +
                      std::to_string(mx) + "+" + std::to_string(my);
            }
            rr.free(ms);
        }
    }
    // The work area of the current desktop (EWMH), intersected with the monitor.
    int ax = mx, ay = my, aw = mw, ah = mh;
    auto card = [&](Atom prop, std::vector<long>& out) {
        Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (x_->XGetWindowProperty(dpy_, root_, prop, 0, 256, False, XA_CARDINAL, &type, &format, &n, &after, &data) ==
                Success &&
            data && format == 32) {
            const long* v = reinterpret_cast<const long*>(data);
            out.assign(v, v + n);
        }
        if (data) x_->XFree(data);
    };
    std::vector<long> wa, cur;
    card(atoms_.net_workarea, wa);
    card(atoms_.net_current_desktop, cur);
    const std::size_t d = cur.empty() ? 0 : static_cast<std::size_t>(cur[0]);
    if (wa.size() >= (d + 1) * 4) {
        const int wx = static_cast<int>(wa[d * 4]), wy = static_cast<int>(wa[d * 4 + 1]);
        const int ww = static_cast<int>(wa[d * 4 + 2]), wh = static_cast<int>(wa[d * 4 + 3]);
        const int l = std::max(mx, wx), t = std::max(my, wy);
        const int r = std::min(mx + mw, wx + ww), b = std::min(my + mh, wy + wh);
        if (r - l >= 200 && b - t >= 150) ax = l, ay = t, aw = r - l, ah = b - t;
    }
    ScreenArea s;
    s.scale = scale_;
    s.area = detail::to_dip(ax, ay, aw, ah, scale_);
    s.key = std::move(key);
    return s;
}

uint64_t X11Platform::foreground_token() const {
    Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    uint64_t w = 0;
    if (x_->XGetWindowProperty(dpy_, root_, atoms_.net_active_window, 0, 1, False, XA_WINDOW, &type, &format, &n, &after,
                               &data) == Success &&
        data && n == 1 && format == 32)
        w = static_cast<uint64_t>(*reinterpret_cast<const unsigned long*>(data));
    if (data) x_->XFree(data);
    return w;
}

void X11Platform::restore_foreground(uint64_t token) {
    if (!token) return;
    if (windows_.count(static_cast<Window>(token))) return;   // one of ours
    activate(static_cast<Window>(token), true);
}

void X11Platform::activate(Window w, bool as_pager) {
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atoms_.net_active_window;
    e.xclient.format = 32;
    e.xclient.data.l[0] = as_pager ? 2 : 1;   // source: 1 application, 2 pager
    e.xclient.data.l[1] = static_cast<long>(last_time_);
    x_->XSendEvent(dpy_, root_, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    x_->XFlush(dpy_);
}

Result<std::unique_ptr<IWindow>> X11Platform::create_window(const WindowDesc& d, IWindowListener& l) {
    return create_x11_window(*this, d, l);
}

Result<std::unique_ptr<IPlatform>> create_x11_platform(const PlatformOptions& opts) {
    std::string why;
    const Api* x = api(&why);
    if (!x) return make_error(Errc::unavailable, why);
    auto p = std::make_unique<X11Platform>(opts, x);
    if (auto r = p->init(); !r) return Unexpected(r.error());
    return std::unique_ptr<IPlatform>(std::move(p));
}

}  // namespace kt::platform::x11
