// Cocoa platform: NSApplication without a nib, the event loop on the main run loop, the
// pasteboard.
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <functional>
#include <map>
#include <string>
#include <utility>

#include "base/log.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/macos_internal.h"

using kt::platform::mac::MacLoop;

namespace {

#include "platform_gui/app_icon_png.inc"

// The Dock and app switcher icon. keepterm runs as a plain executable as well as from a bundle;
// a bundle with its own icon (CFBundleIconFile / CFBundleIconName) keeps it.
void set_app_icon() {
    NSBundle* b = [NSBundle mainBundle];
    if ([b objectForInfoDictionaryKey:@"CFBundleIconFile"] || [b objectForInfoDictionaryKey:@"CFBundleIconName"]) return;
    NSData* png = [NSData dataWithBytesNoCopy:const_cast<unsigned char*>(kIconPng1024) length:sizeof kIconPng1024 freeWhenDone:NO];
    NSImage* image = [[NSImage alloc] initWithData:png];
    if (image) NSApp.applicationIconImage = image;
}

}  // namespace

namespace {
std::function<void()> g_quit_handler;   // the app delegate forwards terminate: here
bool g_platform_alive = false;          // one IPlatform per process

// Global hotkeys (Carbon; no Accessibility permission). The handler runs on the main thread.
struct CarbonHotkey {
    EventHotKeyRef ref = nullptr;
    std::function<void()> fn;
};
std::map<uint32_t, CarbonHotkey> g_hotkeys;
EventHandlerRef g_hotkey_handler = nullptr;

OSStatus on_hotkey(EventHandlerCallRef, EventRef ev, void*) {
    EventHotKeyID hid{};
    if (GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof hid, nullptr, &hid) != noErr)
        return eventNotHandledErr;
    auto it = g_hotkeys.find(hid.id);
    if (it == g_hotkeys.end()) return eventNotHandledErr;
    auto fn = it->second.fn;   // the callback may unregister it
    if (fn) fn();
    return noErr;
}

// Top-left global coordinates (WindowPlacement on macOS): y grows down from the top of the
// first screen.
kt::platform::DipRect top_left(NSRect r) {
    NSScreen* first = NSScreen.screens.firstObject;
    const CGFloat top = first ? NSMaxY(first.frame) : 0;
    return kt::platform::DipRect{static_cast<float>(r.origin.x), static_cast<float>(top - NSMaxY(r)),
                                 static_cast<float>(r.size.width), static_cast<float>(r.size.height)};
}
}

@interface KtAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation KtAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;   // sessions keep running; the app decides when to quit
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    // Cmd+Q and the Dock "Quit" end the loop through IEventLoop::quit, so that run() returns
    // and destructors run, instead of exit() inside AppKit.
    if (g_quit_handler) g_quit_handler();
    return NSTerminateCancel;
}
@end

namespace kt::platform::mac {

namespace {

void source_perform(void* info) { static_cast<MacLoop*>(info)->on_source(); }

void timer_fire(CFRunLoopTimerRef, void* info) { static_cast<MacLoop*>(info)->on_timer(); }

constexpr CFTimeInterval kFarFuture = 1.0e10;

}  // namespace

MacLoop::MacLoop() {
    main_ = CFRunLoopGetMain();
    CFRunLoopSourceContext sctx{};
    sctx.info = this;
    sctx.perform = &source_perform;
    source_ = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &sctx);
    // Common modes: tasks and timers also run during live resize and menu tracking.
    CFRunLoopAddSource(main_, source_, kCFRunLoopCommonModes);

    CFRunLoopTimerContext tctx{};
    tctx.info = this;
    timer_ = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kFarFuture, kFarFuture, 0, 0,
                                  &timer_fire, &tctx);
    CFRunLoopAddTimer(main_, timer_, kCFRunLoopCommonModes);
}

MacLoop::~MacLoop() {
    for (auto& [fd, src] : waitables_) dispatch_source_cancel(src);
    waitables_.clear();
    if (timer_) {
        CFRunLoopTimerInvalidate(timer_);
        CFRelease(timer_);
    }
    if (source_) {
        CFRunLoopSourceInvalidate(source_);
        CFRelease(source_);
    }
}

int MacLoop::run() {
    if (quit_requested_) {
        quit_requested_ = false;
        return exit_code_;
    }
    [NSApp run];
    quit_requested_ = false;
    return exit_code_;
}

void MacLoop::quit(int exit_code) {
    exit_code_ = exit_code;
    quit_requested_ = true;
    [NSApp stop:nil];
    // -stop: takes effect after the next event; post one so that -run returns now.
    NSEvent* ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0];
    [NSApp postEvent:ev atStart:YES];
}

void MacLoop::post(std::function<void()> task) {
    if (tasks_.push(std::move(task))) {
        CFRunLoopSourceSignal(source_);
        CFRunLoopWakeUp(main_);
    }
}

void MacLoop::on_source() {
    tasks_.drain();
    if (!tasks_.empty()) {
        CFRunLoopSourceSignal(source_);
    }
    reschedule();
}

TimerId MacLoop::add_timer(Clock::time_point deadline, std::function<void()> fn) {
    const TimerId id = timers_.add(deadline, std::move(fn));
    reschedule();
    return id;
}

void MacLoop::cancel_timer(TimerId id) {
    timers_.cancel(id);
    reschedule();
}

void MacLoop::on_timer() {
    timers_.run_due(Clock::now());
    reschedule();
}

void MacLoop::reschedule() {
    const auto next = timers_.next_deadline();
    CFAbsoluteTime when = CFAbsoluteTimeGetCurrent() + kFarFuture;
    if (next) {
        const auto now = Clock::now();
        const double secs = *next <= now ? 0.0 : std::chrono::duration<double>(*next - now).count();
        when = CFAbsoluteTimeGetCurrent() + secs;
    }
    CFRunLoopTimerSetNextFireDate(timer_, when);
}

Result<void> MacLoop::add_waitable(Waitable fd, std::function<void()> on_signalled) {
    if (fd < 0) return make_error(Errc::invalid_argument, "negative fd");
    if (waitables_.contains(fd)) return make_error(Errc::busy, "fd already registered");
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(fd), 0,
                                                   dispatch_get_main_queue());
    if (!src) return make_error(Errc::os_error, "dispatch_source_create failed");
    auto fn = std::make_shared<std::function<void()>>(std::move(on_signalled));
    dispatch_source_set_event_handler(src, ^{
      (*fn)();
    });
    dispatch_resume(src);
    waitables_.emplace(fd, src);
    return {};
}

void MacLoop::remove_waitable(Waitable fd) {
    auto it = waitables_.find(fd);
    if (it == waitables_.end()) return;
    dispatch_source_cancel(it->second);
    waitables_.erase(it);
}

namespace {

// Pasteboard access is synchronous; read callbacks run on the next loop turn so that the
// behavior matches the other OSes.
class MacClipboard final : public IClipboard {
public:
    explicit MacClipboard(MacLoop& loop) : loop_(loop) {}

    void read_text(Selection sel, std::function<void(Result<std::u8string>)> cb) override {
        if (sel == Selection::primary) {
            loop_.post([cb = std::move(cb)] { cb(make_error(Errc::not_supported, "no primary selection on macOS")); });
            return;
        }
        Result<std::u8string> r = std::u8string();
        @autoreleasepool {
            NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
            if (s) {
                const char* utf8 = s.UTF8String;
                std::u8string text(reinterpret_cast<const char8_t*>(utf8 ? utf8 : ""));
                if (text.size() > kMaxBytes) {
                    r = make_error(Errc::invalid_argument, "clipboard text exceeds the size limit");
                } else {
                    r = std::move(text);
                }
            }
        }
        loop_.post([cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
    }

    void read_image(std::function<void(Result<std::vector<uint8_t>>)> cb) override {
        Result<std::vector<uint8_t>> r = std::vector<uint8_t>{};
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            NSData* png = [pb dataForType:NSPasteboardTypePNG];
            if (!png) {
                // Screenshots and most apps also offer TIFF; re-encode it as PNG.
                NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF];
                NSBitmapImageRep* rep = tiff ? [NSBitmapImageRep imageRepWithData:tiff] : nil;
                if (rep) png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            }
            if (png.length > kMaxBytes) {
                r = make_error(Errc::invalid_argument, "clipboard image exceeds the size limit");
            } else if (png.length) {
                const auto* b = static_cast<const uint8_t*>(png.bytes);
                r = std::vector<uint8_t>(b, b + png.length);
            }
        }
        loop_.post([cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
    }

    Result<void> write_text(Selection sel, std::u8string_view text, bool sensitive) override {
        if (sel == Selection::primary) return make_error(Errc::not_supported, "no primary selection on macOS");
        @autoreleasepool {
            NSString* s = [[NSString alloc] initWithBytes:text.data()
                                                   length:text.size()
                                                 encoding:NSUTF8StringEncoding];
            if (!s) return make_error(Errc::invalid_argument, "text is not valid UTF-8");
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            [pb clearContents];
            if (![pb setString:s forType:NSPasteboardTypeString]) {
                return make_error(Errc::os_error, "NSPasteboard setString failed");
            }
            if (sensitive) {
                // nspasteboard.org marker that clipboard managers respect.
                [pb setString:@"" forType:@"org.nspasteboard.ConcealedType"];
            }
        }
        return {};
    }

    bool has_primary() const override { return false; }

private:
    MacLoop& loop_;
};

void build_main_menu(NSString* app_name) {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:app_name]
                        action:@selector(hide:)
                 keyEquivalent:@"h"];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:app_name]
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    app_item.submenu = app_menu;

    NSMenuItem* win_item = [[NSMenuItem alloc] init];
    [bar addItem:win_item];
    NSMenu* win_menu = [[NSMenu alloc] initWithTitle:@"Window"];
    [win_menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    NSMenuItem* fs = [win_menu addItemWithTitle:@"Enter Full Screen"
                                         action:@selector(toggleFullScreen:)
                                  keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    win_item.submenu = win_menu;
    NSApp.windowsMenu = win_menu;
    NSApp.mainMenu = bar;
}

class MacPlatform final : public IPlatform {
public:
    explicit MacPlatform(const PlatformOptions& opts) : clipboard_(loop_) {
        cfg_.option_as_alt = opts.option_as_alt;
        info_.ws = WindowSystem::cocoa;
        const NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
        info_.os_version = std::to_string(v.majorVersion) + "." + std::to_string(v.minorVersion) + "." +
                           std::to_string(v.patchVersion);
        info_.elevated = geteuid() == 0;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        set_app_icon();
        delegate_ = [[KtAppDelegate alloc] init];
        NSApp.delegate = delegate_;
        build_main_menu([NSProcessInfo processInfo].processName);
        g_quit_handler = [this] { loop_.quit(0); };
        g_platform_alive = true;
        [NSApp finishLaunching];
    }

    ~MacPlatform() override {
        for (auto& [id, h] : g_hotkeys)
            if (h.ref) UnregisterEventHotKey(h.ref);
        g_hotkeys.clear();
        if (g_hotkey_handler) RemoveEventHandler(g_hotkey_handler);
        g_hotkey_handler = nullptr;
        g_quit_handler = nullptr;
        g_platform_alive = false;
        NSApp.delegate = nil;
    }

    const PlatformInfo& info() const override { return info_; }
    IEventLoop& loop() override { return loop_; }

    Result<std::unique_ptr<IWindow>> create_window(const WindowDesc& desc, IWindowListener& listener) override {
        return create_mac_window(loop_, cfg_, desc, listener);
    }

    IClipboard& clipboard() override { return clipboard_; }

    Result<HotkeyId> register_global_hotkey(GlobalHotkey hk, std::function<void()> fn) override {
        int code = -1;
        for (int k = 0; k < 128 && code < 0; ++k)
            if (detail::key_from_mac_keycode(static_cast<uint16_t>(k)) == hk.key) code = k;
        // The key left of 1: on ISO keyboards that is the section key, not the grave key.
        if (hk.key == Key::grave && KBGetLayoutType(LMGetKbdType()) == kKeyboardISO) code = kVK_ISO_Section;
        if (code < 0) return make_error(Errc::invalid_argument, "this key cannot be a global hotkey");
        UInt32 mods = 0;
        if (hk.mods & mod_ctrl) mods |= controlKey;
        if (hk.mods & mod_shift) mods |= shiftKey;
        if (hk.mods & mod_alt) mods |= optionKey;
        if (hk.mods & mod_super) mods |= cmdKey;
        if (!g_hotkey_handler) {
            const EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
            if (InstallEventHandler(GetApplicationEventTarget(), &on_hotkey, 1, &spec, nullptr, &g_hotkey_handler) != noErr)
                return make_error(Errc::unavailable, "the hotkey handler could not be installed");
        }
        const uint32_t id = next_hotkey_++;
        EventHotKeyRef ref = nullptr;
        const OSStatus st = RegisterEventHotKey(static_cast<UInt32>(code), mods, EventHotKeyID{'kptm', id},
                                                GetApplicationEventTarget(), 0, &ref);
        if (st == eventHotKeyExistsErr) return make_error(Errc::busy, "the shortcut is used by another app");
        if (st != noErr) return make_error(Errc::unavailable, "RegisterEventHotKey failed (" + std::to_string(st) + ")");
        g_hotkeys[id] = CarbonHotkey{ref, std::move(fn)};
        return id;
    }

    void unregister_global_hotkey(HotkeyId id) override {
        auto it = g_hotkeys.find(id);
        if (it == g_hotkeys.end()) return;
        if (it->second.ref) UnregisterEventHotKey(it->second.ref);
        g_hotkeys.erase(it);
    }

    std::optional<ScreenArea> screen_area(ScreenPick pick) const override {
        NSScreen* s = nil;
        if (pick == ScreenPick::mouse) {
            const NSPoint m = NSEvent.mouseLocation;
            for (NSScreen* c in NSScreen.screens)
                if (NSPointInRect(m, c.frame)) s = c;
        } else if (pick == ScreenPick::active) {
            s = NSScreen.mainScreen;
        }
        if (!s) s = NSScreen.screens.firstObject;
        if (!s) return std::nullopt;
        ScreenArea a;
        a.area = top_left(s.visibleFrame);
        a.scale = static_cast<float>(s.backingScaleFactor);
        NSNumber* num = s.deviceDescription[@"NSScreenNumber"];
        a.key = "display" + std::to_string(num ? num.unsignedIntValue : 0) + "@" +
                std::to_string(static_cast<int>(s.frame.size.width)) + "x" +
                std::to_string(static_cast<int>(s.frame.size.height));
        return a;
    }

    uint64_t foreground_token() const override {
        NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
        return app ? static_cast<uint64_t>(app.processIdentifier) : 0;
    }

    void restore_foreground(uint64_t token) override {
        if (!token || static_cast<pid_t>(token) == getpid() || !NSApp.isActive) return;
        NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(token)];
        if (app) [app activateWithOptions:0];
    }

private:
    uint32_t next_hotkey_ = 1;
    PlatformInfo info_;
    MacConfig cfg_;
    MacLoop loop_;
    MacClipboard clipboard_;
    KtAppDelegate* delegate_ = nil;
};

}  // namespace

}  // namespace kt::platform::mac

namespace kt::platform {

bool display_available() {
    // A GUI login session of this security session; nil under ssh or in a daemon context.
    CFDictionaryRef d = CGSessionCopyCurrentDictionary();
    if (!d) return false;
    CFRelease(d);
    return true;
}

Result<std::unique_ptr<IPlatform>> create_platform(const PlatformOptions& opts) {
    if (opts.force_ws && *opts.force_ws != WindowSystem::cocoa) {
        return make_error(Errc::not_supported, "only Cocoa exists on macOS");
    }
    if (![NSThread isMainThread]) return make_error(Errc::invalid_argument, "create_platform must run on the main thread");
    if (!display_available()) return make_error(Errc::unavailable, "no window server session");
    if (g_platform_alive) return make_error(Errc::busy, "one platform per process");
    auto p = std::make_unique<mac::MacPlatform>(opts);
    return std::unique_ptr<IPlatform>(std::move(p));
}

}  // namespace kt::platform
