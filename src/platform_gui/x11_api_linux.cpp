#include "platform_gui/x11_api_linux.h"

#include <dlfcn.h>


namespace kt::platform::x11 {

namespace {

struct Loaded {
    Api api;
    bool ok = false;
    std::string why;
};

Loaded load() {
    Loaded l;
    // Soname only, never a path. Kept loaded for the life of the process.
    void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        l.why = "libX11.so.6 is not available";
        return l;
    }
#define KT_X11_LOAD(name, required)                                                     \
    l.api.name = reinterpret_cast<decltype(&::name)>(dlsym(lib, #name));                \
    if ((required) && !l.api.name) {                                                    \
        l.why = "libX11.so.6 lacks " #name;                                             \
        return l;                                                                       \
    }
    KT_X11_FUNCS(KT_X11_LOAD)
#undef KT_X11_LOAD
    l.api.XkbSetDetectableAutoRepeat =
        reinterpret_cast<Bool (*)(Display*, Bool, Bool*)>(dlsym(lib, "XkbSetDetectableAutoRepeat"));
    l.ok = true;
    return l;
}

}  // namespace

const Api* api(std::string* why) {
    static const Loaded l = load();
    if (!l.ok) {
        if (why) *why = l.why;
        return nullptr;
    }
    return &l.api;
}

}  // namespace kt::platform::x11
