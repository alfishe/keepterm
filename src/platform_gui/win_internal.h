// Internal declarations shared by the Win32 files.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "platform_gui/loop_parts.h"
#include "platform_gui/platform.h"

namespace kt::platform::win {

// Functions newer than or new in build 14393, resolved at run time from DLLs that are loaded
// anyway. Every pointer may be null; call sites check.
struct Fns {
    BOOL(WINAPI* SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT) = nullptr;   // 1703
    UINT(WINAPI* GetDpiForWindow)(HWND) = nullptr;                                    // 1607
    BOOL(WINAPI* AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT) = nullptr;
    BOOL(WINAPI* EnableNonClientDpiScaling)(HWND) = nullptr;
    HRESULT(WINAPI* SetProcessDpiAwareness)(int) = nullptr;                          // shcore, 8.1
    HRESULT(WINAPI* GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*) = nullptr;         // shcore, 8.1
};
const Fns& fns();

constexpr UINT kWmAppWake = WM_APP + 1;
constexpr UINT kWmSyncFocus = WM_APP + 7;   // re-reads focus once an activation sequence is over
constexpr UINT kWmGetDpiScaledSize = 0x02E4;   // 1703+, not in the 14393 headers
constexpr UINT_PTR kModalTimerId = 1;

// The loop: MsgWaitForMultipleObjectsEx over the message queue and waitable handles; a hidden
// message-only window receives wake-ups and the modal-loop timer.
class WinLoop final : public IEventLoop {
public:
    WinLoop() = default;
    ~WinLoop() override;
    bool init(HINSTANCE);

    int  run() override;
    void quit(int exit_code) override;
    void post(std::function<void()> task) override;
    TimerId add_timer(Clock::time_point deadline, std::function<void()> fn) override;
    void    cancel_timer(TimerId) override;
    Result<void> add_waitable(Waitable, std::function<void()> on_signalled) override;
    void         remove_waitable(Waitable) override;

    // DefWindowProc runs its own loop while moving, resizing and in menus: a WM_TIMER keeps
    // tasks, timers and frames running.
    void enter_modal();
    void exit_modal();
    HWND wake_window() const { return wake_; }
    // WM_HOTKEY on the wake window (RegisterHotKey with it as the owner).
    std::map<int, std::function<void()>> hotkeys;

private:
    static LRESULT CALLBACK wake_proc(HWND, UINT, WPARAM, LPARAM);
    void turn();   // drain posted tasks and due timers

    HWND wake_ = nullptr;
    detail::TaskQueue tasks_;
    detail::TimerHeap timers_;
    std::vector<HANDLE> handles_;
    std::vector<std::shared_ptr<std::function<void()>>> fns_;
    int modal_depth_ = 0;
    bool quit_ = false;
    int exit_code_ = 0;
};

class WinClipboard final : public IClipboard {
public:
    explicit WinClipboard(WinLoop& loop) : loop_(loop) {}
    void read_text(Selection, std::function<void(Result<std::u8string>)>) override;
    Result<void> write_text(Selection, std::u8string_view text, bool sensitive) override;
    bool has_primary() const override { return false; }
    void read_image(std::function<void(Result<std::vector<uint8_t>>)>) override;

private:
    void try_read(std::function<void(Result<std::u8string>)> cb, int attempts_left);
    WinLoop& loop_;
};

class WinPlatform;
Result<std::unique_ptr<IWindow>> create_win_window(WinPlatform&, const WindowDesc&, IWindowListener&);

class WinPlatform final : public IPlatform {
public:
    explicit WinPlatform(const PlatformOptions&);
    ~WinPlatform() override;
    Result<void> init();

    const PlatformInfo& info() const override { return info_; }
    IEventLoop& loop() override { return loop_; }
    Result<std::unique_ptr<IWindow>> create_window(const WindowDesc& d, IWindowListener& l) override {
        return create_win_window(*this, d, l);
    }
    IClipboard& clipboard() override { return clipboard_; }
    Result<HotkeyId> register_global_hotkey(GlobalHotkey, std::function<void()>) override;
    void unregister_global_hotkey(HotkeyId) override;
    std::optional<ScreenArea> screen_area(ScreenPick) const override;
    uint64_t foreground_token() const override;
    void restore_foreground(uint64_t) override;

    HINSTANCE instance() const { return inst_; }
    WinLoop& win_loop() { return loop_; }

private:
    PlatformInfo info_;
    HINSTANCE inst_ = nullptr;
    WinLoop loop_;
    WinClipboard clipboard_{loop_};
    bool classes_ = false;
    int next_hotkey_ = 1;
};

// Window classes registered by WinPlatform::init.
inline constexpr wchar_t kWindowClass[] = L"keepterm.window";
inline constexpr wchar_t kSurfaceClass[] = L"keepterm.surface";
LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK surface_proc(HWND, UINT, WPARAM, LPARAM);

}  // namespace kt::platform::win
