// OS appearance on Linux (platform_gui/appearance.h): the settings portal through libdbus-1,
// loaded with dlopen (never linked), else the GTK settings.
#include "platform_gui/appearance.h"

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace kt::platform {

namespace {

// ---------------------------------------------------------------- libdbus-1 (subset)

struct DBusConnection;
struct DBusMessage;
struct DBusError {   // layout of dbus/dbus-errors.h
    const char* name;
    const char* message;
    unsigned int dummy;
    void* padding1;
};
struct DBusMessageIter {   // opaque, stack allocated; larger than dbus/dbus-message.h needs
    alignas(void*) unsigned char storage[128];
};
using dbus_bool_t = uint32_t;
constexpr int kBusSession = 0;
constexpr int kTypeInvalid = 0;
constexpr int kTypeString = 's';
constexpr int kTypeVariant = 'v';
constexpr int kTypeUint32 = 'u';

struct DBus {
    void* lib = nullptr;
    DBusConnection* (*bus_get_private)(int, DBusError*) = nullptr;
    void (*connection_set_exit_on_disconnect)(DBusConnection*, dbus_bool_t) = nullptr;
    void (*connection_close)(DBusConnection*) = nullptr;
    void (*connection_unref)(DBusConnection*) = nullptr;
    DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    dbus_bool_t (*message_append_args)(DBusMessage*, int, ...) = nullptr;
    DBusMessage* (*send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*) = nullptr;
    void (*message_unref)(DBusMessage*) = nullptr;
    dbus_bool_t (*iter_init)(DBusMessage*, DBusMessageIter*) = nullptr;
    int (*iter_get_arg_type)(DBusMessageIter*) = nullptr;
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    void (*iter_get_basic)(DBusMessageIter*, void*) = nullptr;
    void (*error_init)(DBusError*) = nullptr;
    void (*error_free)(DBusError*) = nullptr;

    template <class F>
    bool sym(F& f, const char* name) {
        f = reinterpret_cast<F>(::dlsym(lib, name));
        return f != nullptr;
    }
    bool load() {
        lib = ::dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        const bool ok = sym(bus_get_private, "dbus_bus_get_private") &&
                        sym(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
                        sym(connection_close, "dbus_connection_close") && sym(connection_unref, "dbus_connection_unref") &&
                        sym(message_new_method_call, "dbus_message_new_method_call") &&
                        sym(message_append_args, "dbus_message_append_args") &&
                        sym(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
                        sym(message_unref, "dbus_message_unref") && sym(iter_init, "dbus_message_iter_init") &&
                        sym(iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
                        sym(iter_recurse, "dbus_message_iter_recurse") && sym(iter_get_basic, "dbus_message_iter_get_basic") &&
                        sym(error_init, "dbus_error_init") && sym(error_free, "dbus_error_free");
        if (!ok) {
            ::dlclose(lib);
            lib = nullptr;
        }
        return ok;
    }
};

DBus* dbus() {
    static DBus* d = [] {
        static DBus inst;
        return inst.load() ? &inst : nullptr;
    }();
    return d;
}

// A private session bus connection for the portal.
class Portal {
public:
    Portal() {
        DBus* d = dbus();
        if (!d) return;
        DBusError err;
        d->error_init(&err);
        conn_ = d->bus_get_private(kBusSession, &err);
        d->error_free(&err);
        if (conn_) d->connection_set_exit_on_disconnect(conn_, 0);
    }
    ~Portal() {
        if (!conn_) return;
        dbus()->connection_close(conn_);
        dbus()->connection_unref(conn_);
    }
    Portal(const Portal&) = delete;
    Portal& operator=(const Portal&) = delete;
    bool ok() const { return conn_ != nullptr; }

    // org.freedesktop.appearance color-scheme: 0 no preference, 1 dark, 2 light. Nothing when
    // the portal does not answer.
    std::optional<uint32_t> color_scheme() {
        DBus* d = dbus();
        if (!conn_ || !d) return std::nullopt;
        DBusMessage* msg = d->message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Settings", "Read");
        if (!msg) return std::nullopt;
        const char* ns = "org.freedesktop.appearance";
        const char* key = "color-scheme";
        d->message_append_args(msg, kTypeString, &ns, kTypeString, &key, kTypeInvalid);
        DBusError err;
        d->error_init(&err);
        DBusMessage* reply = d->send_with_reply_and_block(conn_, msg, 200, &err);
        d->message_unref(msg);
        d->error_free(&err);
        if (!reply) return std::nullopt;
        std::optional<uint32_t> out;
        DBusMessageIter it{};
        if (d->iter_init(reply, &it)) {
            // Read wraps the value in one or two variants (older portals); ReadOne in one.
            for (int depth = 0; depth < 3 && d->iter_get_arg_type(&it) == kTypeVariant; ++depth) {
                DBusMessageIter sub{};
                d->iter_recurse(&it, &sub);
                it = sub;
            }
            if (d->iter_get_arg_type(&it) == kTypeUint32) {
                uint32_t v = 0;
                d->iter_get_basic(&it, &v);
                out = v;
            }
        }
        d->message_unref(reply);
        return out;
    }

private:
    DBusConnection* conn_ = nullptr;
};

// ---------------------------------------------------------------- GTK settings

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool ends_with_dark(std::string_view s) {
    if (s.size() < 4) return false;
    std::string tail(s.substr(s.size() - 4));
    for (char& c : tail) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return tail == "dark" && (s.size() == 4 || s[s.size() - 5] == '-' || s[s.size() - 5] == ':' || s[s.size() - 5] == '_');
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

Appearance gtk_appearance() {
    if (const std::string t = env("GTK_THEME"); !t.empty()) return ends_with_dark(t) ? Appearance::dark : Appearance::light;
    std::string cfg = env("XDG_CONFIG_HOME");
    if (cfg.empty()) {
        const std::string home = env("HOME");
        if (home.empty()) return Appearance::light;
        cfg = home + "/.config";
    }
    for (const char* sub : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
        std::ifstream in(cfg + sub);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            const std::string_view l = trim(line);
            const auto eq = l.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string_view k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
            if (k == "gtk-application-prefer-dark-theme" && (v == "1" || v == "true")) return Appearance::dark;
            if (k == "gtk-theme-name" && ends_with_dark(v)) return Appearance::dark;
        }
    }
    return Appearance::light;
}

Appearance resolve(Portal* portal) {
    if (portal && portal->ok()) {
        if (auto v = portal->color_scheme(); v && *v != 0) return *v == 1 ? Appearance::dark : Appearance::light;
    }
    return gtk_appearance();
}

class LinuxAppearanceWatch final : public AppearanceWatch {
public:
    LinuxAppearanceWatch(IEventLoop& loop, std::function<void(Appearance)> cb) : loop_(loop), cb_(std::move(cb)) {
        portal_ = std::make_unique<Portal>();
        current_ = resolve(portal_.get());
        schedule();
    }
    ~LinuxAppearanceWatch() override {
        if (timer_) loop_.cancel_timer(timer_);
    }
    Appearance current() const override { return current_; }

private:
    void schedule() {
        timer_ = loop_.add_timer(Clock::now() + std::chrono::seconds(2), [this] {
            timer_ = 0;
            const Appearance a = resolve(portal_.get());
            if (a != current_) {
                current_ = a;
                if (cb_) cb_(a);
            }
            schedule();
        });
    }

    IEventLoop& loop_;
    std::function<void(Appearance)> cb_;
    std::unique_ptr<Portal> portal_;
    TimerId timer_ = 0;
    Appearance current_ = Appearance::light;
};

}  // namespace

Appearance system_appearance() {
    Portal p;
    return resolve(&p);
}

std::unique_ptr<AppearanceWatch> watch_appearance(IEventLoop& loop, std::function<void(Appearance)> on_change) {
    return std::make_unique<LinuxAppearanceWatch>(loop, std::move(on_change));
}

}  // namespace kt::platform
