// OS appearance on Windows (platform_gui/appearance.h).
#include "platform_gui/appearance.h"

#include <windows.h>

namespace kt::platform {

namespace {

constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

Appearance read_appearance(HKEY key) {
    DWORD value = 1, size = sizeof(value), type = 0;
    if (::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD)
        return Appearance::light;
    return value == 0 ? Appearance::dark : Appearance::light;
}

class WinAppearanceWatch final : public AppearanceWatch {
public:
    WinAppearanceWatch(IEventLoop& loop, std::function<void(Appearance)> cb) : loop_(loop), cb_(std::move(cb)) {
        current_ = system_appearance();
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ | KEY_NOTIFY, &key_) != ERROR_SUCCESS) {
            key_ = nullptr;
            return;
        }
        event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_ || !arm()) return;
        if (loop_.add_waitable(event_, [this] { changed(); })) registered_ = true;
    }
    ~WinAppearanceWatch() override {
        if (registered_) loop_.remove_waitable(event_);
        if (key_) ::RegCloseKey(key_);   // ends the pending notification
        if (event_) ::CloseHandle(event_);
    }
    Appearance current() const override { return current_; }

private:
    bool arm() {
        return ::RegNotifyChangeKeyValue(key_, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event_, TRUE) == ERROR_SUCCESS;
    }
    void changed() {
        (void)arm();   // one notification per call
        const Appearance a = read_appearance(key_);
        if (a == current_) return;
        current_ = a;
        if (cb_) cb_(a);
    }

    IEventLoop& loop_;
    std::function<void(Appearance)> cb_;
    HKEY key_ = nullptr;
    HANDLE event_ = nullptr;
    bool registered_ = false;
    Appearance current_ = Appearance::light;
};

}  // namespace

Appearance system_appearance() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return Appearance::light;
    const Appearance a = read_appearance(key);
    ::RegCloseKey(key);
    return a;
}

std::unique_ptr<AppearanceWatch> watch_appearance(IEventLoop& loop, std::function<void(Appearance)> on_change) {
    return std::make_unique<WinAppearanceWatch>(loop, std::move(on_change));
}

}  // namespace kt::platform
