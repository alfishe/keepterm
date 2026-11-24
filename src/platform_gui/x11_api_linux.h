// libX11 loaded with dlopen. One X-macro list drives the declarations,
// loading and the required/optional check. Only kt_platform_gui includes this header.
#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>

// X.h's XQueryBestSize class names collide with kt::platform::CursorShape.
#undef CursorShape

#include <string>

namespace kt::platform::x11 {

// X(name, required)
#define KT_X11_FUNCS(X)                                                                      \
    X(XOpenDisplay, true) X(XCloseDisplay, true) X(XDefaultScreen, true) X(XRootWindow, true) \
    X(XConnectionNumber, true) X(XCreateWindow, true) X(XDestroyWindow, true)                \
    X(XMapWindow, true) X(XUnmapWindow, true) X(XMoveResizeWindow, true)                     \
    X(XResizeWindow, true) X(XRaiseWindow, true) X(XIconifyWindow, true)                     \
    X(XInternAtom, true) X(XChangeProperty, true) X(XDeleteProperty, true)                   \
    X(XGetWindowProperty, true) X(XFree, true) X(XSetWMProtocols, true) X(XSelectInput, true) \
    X(XNextEvent, true) X(XPending, true) X(XEventsQueued, true) X(XPeekEvent, true)         \
    X(XFlush, true) X(XSync, true) X(XSendEvent, true) X(XFilterEvent, true)                 \
    X(XLookupString, true) X(XLookupKeysym, true) X(XSetSelectionOwner, true)                \
    X(XGetSelectionOwner, true) X(XConvertSelection, true) X(XCreateFontCursor, true)        \
    X(XDefineCursor, true) X(XFreeCursor, true) X(XCreatePixmap, true) X(XFreePixmap, true)  \
    X(XCreatePixmapCursor, true) X(XSetErrorHandler, true) X(XSetIOErrorHandler, true)       \
    X(XGetWindowAttributes, true) X(XTranslateCoordinates, true) X(XSetClassHint, true)      \
    X(XSetWMNormalHints, true) X(XGetErrorText, true) X(XMaxRequestSize, true)               \
    X(XExtendedMaxRequestSize, true) X(XGetWMHints, false) X(XSetWMHints, false)             \
    X(XOpenIM, false) X(XCloseIM, false) X(XCreateIC, false) X(XDestroyIC, false)            \
    X(XSetICFocus, false) X(XUnsetICFocus, false) X(XSetICValues, false)                     \
    X(XGetICValues, false) X(XGetIMValues, false) X(Xutf8LookupString, false)                \
    X(XVaCreateNestedList, false) X(XSetLocaleModifiers, false) X(XSupportsLocale, false)   \
    X(XGrabKey, false) X(XUngrabKey, false) X(XQueryPointer, false) X(XSetInputFocus, false)

struct Api {
#define KT_X11_DECL(name, required) decltype(&::name) name = nullptr;
    KT_X11_FUNCS(KT_X11_DECL)
#undef KT_X11_DECL
    // XKBlib.h is not vendored: the one Xkb function is declared here.
    Bool (*XkbSetDetectableAutoRepeat)(Display*, Bool, Bool*) = nullptr;

    bool has_xim() const {
        return XOpenIM && XCloseIM && XCreateIC && XDestroyIC && XSetICFocus && XUnsetICFocus && XSetICValues &&
               XGetICValues && XGetIMValues && Xutf8LookupString && XVaCreateNestedList;
    }
};

// Loads libX11.so.6 once. Null when the library or a required symbol is missing; `why` gets
// the reason.
const Api* api(std::string* why = nullptr);

}  // namespace kt::platform::x11
