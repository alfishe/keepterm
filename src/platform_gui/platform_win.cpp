// Win32 platform: DPI awareness, the message loop, the clipboard.
// Uses only functions of build 14393; newer ones go through fns().
#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include "base/log.h"
#include "platform_gui/clipboard_image_win.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/utf.h"
#include "platform_gui/win_internal.h"

namespace kt::platform {

namespace win {

namespace {

template <typename T>
void resolve(HMODULE m, const char* name, T& out) {
    if (!m) return;
    // Through void*: function pointer casts between unrelated types warn on some compilers.
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

Fns load_fns() {
    Fns f;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    resolve(user32, "SetProcessDpiAwarenessContext", f.SetProcessDpiAwarenessContext);
    resolve(user32, "GetDpiForWindow", f.GetDpiForWindow);
    resolve(user32, "AdjustWindowRectExForDpi", f.AdjustWindowRectExForDpi);
    resolve(user32, "EnableNonClientDpiScaling", f.EnableNonClientDpiScaling);
    // System32 only: never from the application directory or the current directory.
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    resolve(shcore, "SetProcessDpiAwareness", f.SetProcessDpiAwareness);
    resolve(shcore, "GetDpiForMonitor", f.GetDpiForMonitor);
    return f;
}

constexpr int kClipboardAttempts = 5;   // 5 tries, 10 ms apart
constexpr auto kClipboardRetry = std::chrono::milliseconds(10);

bool open_clipboard(HWND owner) {
    for (int i = 0; i < kClipboardAttempts; ++i) {
        if (OpenClipboard(owner)) return true;
        // Writes are synchronous (write_text returns a Result), so they wait here briefly;
        // reads retry from the timer heap instead.
        Sleep(10);
    }
    return false;
}

void set_dword_format(const wchar_t* name, DWORD value) {
    const UINT fmt = RegisterClipboardFormatW(name);
    if (!fmt) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!h) return;
    if (void* p = GlobalLock(h)) {
        std::memcpy(p, &value, sizeof value);
        GlobalUnlock(h);
        if (SetClipboardData(fmt, h)) return;
    }
    GlobalFree(h);
}

uint32_t read_build_number() {
    wchar_t buf[32] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return 0;
    }
    uint32_t n = 0;
    for (const wchar_t* p = buf; *p >= L'0' && *p <= L'9'; ++p) n = n * 10 + static_cast<uint32_t>(*p - L'0');
    return n;
}

bool process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

}  // namespace

const Fns& fns() {
    static const Fns f = load_fns();
    return f;
}

// ---------------------------------------------------------------- WinLoop

WinLoop::~WinLoop() {
    if (wake_) DestroyWindow(wake_);
}

bool WinLoop::init(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &WinLoop::wake_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"keepterm.wake";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    wake_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!wake_) return false;
    SetWindowLongPtrW(wake_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

LRESULT CALLBACK WinLoop::wake_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<WinLoop*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == WM_HOTKEY) {
        // A copy: the callback may unregister its own hotkey.
        if (auto it = self->hotkeys.find(static_cast<int>(w)); it != self->hotkeys.end()) {
            auto fn = it->second;
            if (fn) fn();
        }
        return 0;
    }
    if (self && (msg == kWmAppWake || (msg == WM_TIMER && w == kModalTimerId))) {
        // Also reached from inside modal loops, where run() is not running.
        self->turn();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void WinLoop::turn() {
    tasks_.drain();
    timers_.run_due(Clock::now());
}

int WinLoop::run() {
    while (!quit_) {
        MSG m;
        while (!quit_ && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                exit_code_ = static_cast<int>(m.wParam);
                quit_ = true;
                break;
            }
            // No TranslateMessage here: the window procedure calls it for WM_KEYDOWN and then
            // collects the WM_CHAR it produced.
            DispatchMessageW(&m);
        }
        if (quit_) break;
        turn();
        if (quit_) break;

        DWORD timeout = static_cast<DWORD>(detail::wait_timeout_ms(timers_.next_deadline(), Clock::now(), 60'000));
        if (!tasks_.empty()) timeout = 0;
        const auto n = static_cast<DWORD>(handles_.size());
        const DWORD r = MsgWaitForMultipleObjectsEx(n, handles_.empty() ? nullptr : handles_.data(), timeout,
                                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
        if (r == WAIT_FAILED) {
            log::error("win", "MsgWaitForMultipleObjectsEx failed: " + std::to_string(GetLastError()));
            return 1;
        }
        DWORD idx = n;
        if (r < WAIT_OBJECT_0 + n) idx = r - WAIT_OBJECT_0;
        else if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + n) idx = r - WAIT_ABANDONED_0;
        if (idx < n) {
            auto fn = fns_[idx];   // copy: the callback may remove itself
            (*fn)();
        }
    }
    quit_ = false;
    return exit_code_;
}

void WinLoop::quit(int exit_code) {
    exit_code_ = exit_code;
    quit_ = true;   // seen by run() when a modal loop, if any, ends
}

void WinLoop::post(std::function<void()> task) {
    if (tasks_.push(std::move(task))) PostMessageW(wake_, kWmAppWake, 0, 0);
}

TimerId WinLoop::add_timer(Clock::time_point deadline, std::function<void()> fn) {
    return timers_.add(deadline, std::move(fn));
}

void WinLoop::cancel_timer(TimerId id) { timers_.cancel(id); }

Result<void> WinLoop::add_waitable(Waitable h, std::function<void()> on_signalled) {
    if (!h || h == INVALID_HANDLE_VALUE) return make_error(Errc::invalid_argument, "invalid handle");
    for (HANDLE x : handles_) {
        if (x == h) return make_error(Errc::busy, "handle already registered");
    }
    // A helper thread with RegisterWaitForSingleObject for more handles is a later step.
    if (handles_.size() >= MAXIMUM_WAIT_OBJECTS - 1) return make_error(Errc::not_supported, "too many waitable handles");
    handles_.push_back(h);
    fns_.push_back(std::make_shared<std::function<void()>>(std::move(on_signalled)));
    return {};
}

void WinLoop::remove_waitable(Waitable h) {
    for (size_t i = 0; i < handles_.size(); ++i) {
        if (handles_[i] == h) {
            handles_.erase(handles_.begin() + static_cast<ptrdiff_t>(i));
            fns_.erase(fns_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void WinLoop::enter_modal() {
    if (modal_depth_++ == 0) SetTimer(wake_, kModalTimerId, 8, nullptr);
}

void WinLoop::exit_modal() {
    if (modal_depth_ > 0 && --modal_depth_ == 0) KillTimer(wake_, kModalTimerId);
}

// ---------------------------------------------------------------- WinClipboard

void WinClipboard::read_text(Selection s, std::function<void(Result<std::u8string>)> cb) {
    if (s != Selection::clipboard) {
        loop_.post([cb = std::move(cb)] { cb(make_error(Errc::not_supported, "Windows has no primary selection")); });
        return;
    }
    loop_.post([this, cb = std::move(cb)]() mutable { try_read(std::move(cb), kClipboardAttempts); });
}

void WinClipboard::try_read(std::function<void(Result<std::u8string>)> cb, int attempts_left) {
    if (!OpenClipboard(loop_.wake_window())) {
        if (attempts_left <= 1) {
            cb(make_error(Errc::busy, "another process holds the clipboard"));
            return;
        }
        loop_.add_timer(Clock::now() + kClipboardRetry, [this, cb = std::move(cb), attempts_left]() mutable {
            try_read(std::move(cb), attempts_left - 1);
        });
        return;
    }
    Result<std::u8string> result = std::u8string();
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const char16_t*>(GlobalLock(h))) {
            const size_t cap = GlobalSize(h) / sizeof(char16_t);
            size_t n = 0;
            while (n < cap && p[n]) ++n;
            if (n * 3 > kMaxBytes) {
                result = make_error(Errc::invalid_argument, "clipboard text exceeds the size limit");
            } else {
                result = detail::utf16_to_utf8(std::u16string_view(p, n));
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    cb(std::move(result));
}

namespace {
// A packed DIB (CF_DIB) as a .bmp file in memory, for the WIC decoder.
std::vector<uint8_t> dib_to_bmp(const uint8_t* p, size_t n) {
    if (n < sizeof(BITMAPINFOHEADER)) return {};
    BITMAPINFOHEADER bi;
    std::memcpy(&bi, p, sizeof bi);
    if (bi.biSize < sizeof bi || bi.biSize > n) return {};
    size_t colors = bi.biClrUsed;
    if (!colors && bi.biBitCount <= 8) colors = size_t{1} << bi.biBitCount;
    size_t off = bi.biSize + colors * 4;
    if (bi.biSize == sizeof bi && (bi.biCompression == BI_BITFIELDS || bi.biCompression == 6 /*BI_ALPHABITFIELDS*/))
        off += bi.biCompression == BI_BITFIELDS ? 12 : 16;
    if (off > n) return {};
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4d42;   // "BM"
    fh.bfSize = static_cast<DWORD>(sizeof fh + n);
    fh.bfOffBits = static_cast<DWORD>(sizeof fh + off);
    std::vector<uint8_t> out(sizeof fh + n);
    std::memcpy(out.data(), &fh, sizeof fh);
    std::memcpy(out.data() + sizeof fh, p, n);
    return out;
}

// Re-encodes an image WIC can read (BMP here) as PNG.
std::vector<uint8_t> to_png(const std::vector<uint8_t>& src) {
    std::vector<uint8_t> out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IWICImagingFactory* f = nullptr;
    IStream* in = nullptr;
    IStream* os = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
        in = SHCreateMemStream(src.data(), static_cast<UINT>(src.size()));
        if (!in || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &os))) break;
        if (FAILED(f->CreateDecoderFromStream(in, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(os, WICBitmapEncoderNoCache)))
            break;
        if (FAILED(enc->CreateNewFrame(&fe, nullptr)) || FAILED(fe->Initialize(nullptr))) break;
        if (FAILED(fe->WriteSource(frame, nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit())) break;
        STATSTG st{};
        if (FAILED(os->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart > IClipboard::kMaxBytes) break;
        out.resize(static_cast<size_t>(st.cbSize.QuadPart));
        LARGE_INTEGER zero{};
        ULONG got = 0;
        if (FAILED(os->Seek(zero, STREAM_SEEK_SET, nullptr)) || FAILED(os->Read(out.data(), static_cast<ULONG>(out.size()), &got)) ||
            got != out.size())
            out.clear();
    } while (false);
    for (IUnknown* u : std::initializer_list<IUnknown*>{fe, enc, frame, dec, os, in, f})
        if (u) u->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}
}  // namespace

std::vector<uint8_t> dib_as_png(const uint8_t* dib, size_t size) {
    const std::vector<uint8_t> bmp = dib_to_bmp(dib, size);
    return bmp.empty() ? std::vector<uint8_t>{} : to_png(bmp);
}

void WinClipboard::read_image(std::function<void(Result<std::vector<uint8_t>>)> cb) {
    loop_.post([this, cb = std::move(cb)]() mutable {
        if (!OpenClipboard(loop_.wake_window())) {
            cb(make_error(Errc::busy, "another process holds the clipboard"));
            return;
        }
        // Browsers and Office offer "PNG" as is; screenshots offer a DIB.
        std::vector<uint8_t> png, dib;
        auto bytes = [](UINT fmt, std::vector<uint8_t>& dst) {
            HANDLE h = GetClipboardData(fmt);
            if (!h) return;
            const size_t n = GlobalSize(h);
            if (!n || n > IClipboard::kMaxBytes) return;
            if (const auto* p = static_cast<const uint8_t*>(GlobalLock(h))) {
                dst.assign(p, p + n);
                GlobalUnlock(h);
            }
        };
        const UINT cf_png = RegisterClipboardFormatW(L"PNG");
        if (cf_png && IsClipboardFormatAvailable(cf_png)) bytes(cf_png, png);
        if (png.empty() && IsClipboardFormatAvailable(CF_DIB)) bytes(CF_DIB, dib);
        CloseClipboard();   // the conversion runs on a copy, with the clipboard released
        if (png.empty() && !dib.empty()) png = dib_as_png(dib.data(), dib.size());
        cb(std::move(png));
    });
}

Result<void> WinClipboard::write_text(Selection s, std::u8string_view text, bool sensitive) {
    if (s != Selection::clipboard) return make_error(Errc::not_supported, "Windows has no primary selection");
    // Windows text uses CRLF line ends; lone LFs are expanded.
    std::u16string u = detail::utf8_to_utf16(text);
    std::u16string crlf;
    crlf.reserve(u.size() + 16);
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == u'\n' && (i == 0 || u[i - 1] != u'\r')) crlf.push_back(u'\r');
        crlf.push_back(u[i]);
    }
    const size_t bytes = (crlf.size() + 1) * sizeof(char16_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return last_os_error("GlobalAlloc");
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return last_os_error("GlobalLock");
    }
    std::memcpy(p, crlf.c_str(), bytes);
    GlobalUnlock(h);
    if (!open_clipboard(loop_.wake_window())) {
        GlobalFree(h);
        return make_error(Errc::busy, "another process holds the clipboard");
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        auto err = last_os_error("SetClipboardData");
        GlobalFree(h);
        CloseClipboard();
        return err;
    }
    if (sensitive) {
        // Clipboard history and cloud sync skip these (1809+); harmless before.
        set_dword_format(L"ExcludeClipboardContentFromMonitorProcessing", 0);
        set_dword_format(L"CanIncludeInClipboardHistory", 0);
        set_dword_format(L"CanUploadToCloudClipboard", 0);
    }
    CloseClipboard();
    return {};
}

// ---------------------------------------------------------------- WinPlatform

WinPlatform::WinPlatform(const PlatformOptions&) {
    info_.ws = WindowSystem::win32;
    info_.os_build = read_build_number();
    info_.os_version = "10.0." + std::to_string(info_.os_build);
    info_.elevated = process_elevated();
}

WinPlatform::~WinPlatform() {
    if (classes_) {
        UnregisterClassW(kWindowClass, inst_);
        UnregisterClassW(kSurfaceClass, inst_);
    }
}

Result<void> WinPlatform::init() {
    inst_ = GetModuleHandleW(nullptr);
    // Per-monitor v2 on 1703+, v1 on 1607. A manifest that already set the
    // awareness makes both calls fail harmlessly.
    const Fns& f = fns();
    const auto per_monitor_v2 = reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<intptr_t>(-4));
    bool aware = f.SetProcessDpiAwarenessContext && f.SetProcessDpiAwarenessContext(per_monitor_v2);
    if (!aware && f.SetProcessDpiAwareness) {
        constexpr int kProcessPerMonitorDpiAware = 2;
        f.SetProcessDpiAwareness(kProcessPerMonitorDpiAware);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // The exe's icon resource (app/keepterm.rc.in), if any: the large one for Alt+Tab and the
    // taskbar, the small one for the title bar, each from its own entry rather than scaled.
    wc.hIcon = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return last_os_error("RegisterClassExW");
    WNDCLASSEXW sc{};
    sc.cbSize = sizeof sc;
    sc.lpfnWndProc = &surface_proc;
    sc.hInstance = inst_;
    sc.lpszClassName = kSurfaceClass;
    if (!RegisterClassExW(&sc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return last_os_error("RegisterClassExW");
    classes_ = true;
    if (!loop_.init(inst_)) return last_os_error("cannot create the wake window");
    return {};
}

// ---------------------------------------------------------------- global hotkeys, screens

Result<HotkeyId> WinPlatform::register_global_hotkey(GlobalHotkey hk, std::function<void()> fn) {
    // The physical key's scan code, then the virtual key the current layout gives it: the key
    // left of 1 stays that key on every layout (VK_OEM_3 is Ö on a German layout).
    UINT sc = 0;
    for (UINT c = 1; c < 0x80 && !sc; ++c)
        if (detail::key_from_win_scancode(c, false) == hk.key) sc = c;
    const UINT vk = sc ? MapVirtualKeyW(sc, 3 /* MAPVK_VSC_TO_VK_EX */) : 0;
    if (!vk) return make_error(Errc::invalid_argument, "this key cannot be a global hotkey");
    UINT mods = 0x4000;   // MOD_NOREPEAT
    if (hk.mods & mod_ctrl) mods |= MOD_CONTROL;
    if (hk.mods & mod_shift) mods |= MOD_SHIFT;
    if (hk.mods & mod_alt) mods |= MOD_ALT;
    if (hk.mods & mod_super) mods |= MOD_WIN;
    const int id = next_hotkey_++;
    if (!RegisterHotKey(loop_.wake_window(), id, mods, vk)) {
        const DWORD e = GetLastError();
        if (e == ERROR_HOTKEY_ALREADY_REGISTERED) return make_error(Errc::busy, "the shortcut is used by another app");
        return make_error(Errc::unavailable, "RegisterHotKey failed (" + std::to_string(e) + ")");
    }
    loop_.hotkeys[id] = std::move(fn);
    return static_cast<HotkeyId>(id);
}

void WinPlatform::unregister_global_hotkey(HotkeyId id) {
    UnregisterHotKey(loop_.wake_window(), static_cast<int>(id));
    loop_.hotkeys.erase(static_cast<int>(id));
}

std::optional<ScreenArea> WinPlatform::screen_area(ScreenPick pick) const {
    HMONITOR mon = nullptr;
    if (pick == ScreenPick::mouse) {
        POINT pt{};
        if (GetCursorPos(&pt)) mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else if (pick == ScreenPick::active) {
        if (HWND fg = GetForegroundWindow()) mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, &mi)) return std::nullopt;
    UINT dx = 96, dy = 96;
    if (!fns().GetDpiForMonitor || FAILED(fns().GetDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy))) {
        HDC dc = GetDC(nullptr);
        dx = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
    }
    ScreenArea s;
    s.scale = static_cast<float>(dx) / 96.0f;
    const RECT& r = mi.rcWork;
    s.area = DipRect{static_cast<float>(r.left) / s.scale, static_cast<float>(r.top) / s.scale,
                     static_cast<float>(r.right - r.left) / s.scale, static_cast<float>(r.bottom - r.top) / s.scale};
    const RECT& m = mi.rcMonitor;
    const std::u8string dev = detail::utf16_to_utf8(std::u16string_view(reinterpret_cast<const char16_t*>(mi.szDevice)));
    s.key = std::string(dev.begin(), dev.end()) + "@" +
            std::to_string(m.right - m.left) + "x" + std::to_string(m.bottom - m.top);
    return s;
}

uint64_t WinPlatform::foreground_token() const {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(GetForegroundWindow()));
}

void WinPlatform::restore_foreground(uint64_t token) {
    HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(token));
    if (h && IsWindow(h) && IsWindowVisible(h)) SetForegroundWindow(h);
}

}  // namespace win

bool display_available() {
    // An interactive window station (WinSta0) is visible; services and some CI agents run on
    // a non-interactive one.
    HWINSTA ws = GetProcessWindowStation();
    if (!ws) return false;
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(ws, UOI_FLAGS, &flags, sizeof flags, &needed)) return false;
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

Result<std::unique_ptr<IPlatform>> create_platform(const PlatformOptions& opts) {
    if (opts.force_ws && *opts.force_ws != WindowSystem::win32) {
        return make_error(Errc::invalid_argument, "only win32 exists on Windows");
    }
    if (!display_available()) return make_error(Errc::unavailable, "no interactive window station");
    auto p = std::make_unique<win::WinPlatform>(opts);
    if (auto r = p->init(); !r) return Unexpected(r.error());
    return std::unique_ptr<IPlatform>(std::move(p));
}

}  // namespace kt::platform

namespace kt::platform::detail {
std::vector<uint8_t> dib_to_png(const uint8_t* dib, size_t size) { return win::dib_as_png(dib, size); }
}  // namespace kt::platform::detail
