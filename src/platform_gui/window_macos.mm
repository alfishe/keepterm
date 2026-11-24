// Cocoa windows: NSWindow, a layer-hosting NSView with NSTextInputClient, backing scale,
// render layers.
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/QuartzCore.h>

#include <string>
#include <utility>

#include "platform_gui/dpi.h"
#include "platform_gui/keyboard.h"
#include "platform_gui/macos_internal.h"
#include "platform_gui/utf.h"

namespace kt::platform::mac {
class MacWindow;
}

using kt::platform::mac::MacWindow;

// ---------------------------------------------------------------- Objective-C classes

@interface KtView : NSView <NSTextInputClient>
@property(nonatomic, assign) MacWindow* owner;
@end

// The quick terminal: a borderless panel that takes keys without activating the app, so the
// app in front stays in front and gets the keys back when the panel hides.
@interface KtQuickPanel : NSPanel
@end
@implementation KtQuickPanel
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@interface KtWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) MacWindow* owner;
@end

namespace kt::platform::mac {

namespace {

std::u8string to_u8(NSString* s) {
    if (!s) return {};
    const char* p = s.UTF8String;
    return p ? std::u8string(reinterpret_cast<const char8_t*>(p)) : std::u8string();
}

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

NSString* to_ns(std::u8string_view s) {
    NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return r ? r : @"";
}

// First code point of an NSString (joins a surrogate pair).
char32_t first_char(NSString* s) {
    if (!s || s.length == 0) return 0;
    const unichar c0 = [s characterAtIndex:0];
    if (c0 >= 0xD800 && c0 <= 0xDBFF && s.length > 1) {
        const unichar c1 = [s characterAtIndex:1];
        if (c1 >= 0xDC00 && c1 <= 0xDFFF) return 0x10000 + ((char32_t(c0) - 0xD800) << 10) + (char32_t(c1) - 0xDC00);
    }
    return c0;
}

// The character of `keycode` with no modifiers in the current ASCII-capable layout
// (ctrl+c keeps working on a Russian layout). 0 when unavailable.
char32_t ascii_capable_char(uint16_t keycode) {
    TISInputSourceRef src = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    if (!src) return 0;
    char32_t result = 0;
    auto data = static_cast<CFDataRef>(TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData));
    if (data) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 dead = 0;
        UniChar chars[4];
        UniCharCount len = 0;
        const OSStatus st = UCKeyTranslate(layout, keycode, kUCKeyActionDisplay, 0, LMGetKbdType(),
                                           kUCKeyTranslateNoDeadKeysBit, &dead, 4, &len, chars);
        if (st == noErr && len == 1) result = chars[0];
    }
    CFRelease(src);
    return result;
}

// The window frame cursors the system shows at a window's edges (macOS 15); before 15, the old
// edge arrows, and the diagonal ones fall back to the horizontal arrow.
NSCursor* frame_cursor(CursorShape s) {
    if (@available(macOS 15.0, *)) {
        NSCursorFrameResizePosition p = NSCursorFrameResizePositionRight;
        switch (s) {
            case CursorShape::frame_n: p = NSCursorFrameResizePositionTop; break;
            case CursorShape::frame_s: p = NSCursorFrameResizePositionBottom; break;
            case CursorShape::frame_e: p = NSCursorFrameResizePositionRight; break;
            case CursorShape::frame_w: p = NSCursorFrameResizePositionLeft; break;
            case CursorShape::frame_ne: p = NSCursorFrameResizePositionTopRight; break;
            case CursorShape::frame_nw: p = NSCursorFrameResizePositionTopLeft; break;
            case CursorShape::frame_se: p = NSCursorFrameResizePositionBottomRight; break;
            case CursorShape::frame_sw: p = NSCursorFrameResizePositionBottomLeft; break;
            default: break;
        }
        return [NSCursor frameResizeCursorFromPosition:p inDirections:NSCursorFrameResizeDirectionsAll];
    }
    return s == CursorShape::frame_n || s == CursorShape::frame_s ? [NSCursor resizeUpDownCursor] : [NSCursor resizeLeftRightCursor];
}

NSCursor* ns_cursor(CursorShape s) {
    switch (s) {
        case CursorShape::arrow: return [NSCursor arrowCursor];
        case CursorShape::ibeam: return [NSCursor IBeamCursor];
        case CursorShape::hand: return [NSCursor pointingHandCursor];
        case CursorShape::crosshair: return [NSCursor crosshairCursor];
        case CursorShape::wait: return [NSCursor arrowCursor];   // no public busy cursor
        case CursorShape::move: return [NSCursor openHandCursor];
        case CursorShape::resize_ew: return [NSCursor resizeLeftRightCursor];
        case CursorShape::resize_ns: return [NSCursor resizeUpDownCursor];
        case CursorShape::resize_nesw:
        case CursorShape::resize_nwse: return [NSCursor crosshairCursor];   // public diagonal cursors need macOS 15
        case CursorShape::hidden: return [NSCursor arrowCursor];
        case CursorShape::frame_n:
        case CursorShape::frame_s:
        case CursorShape::frame_e:
        case CursorShape::frame_w:
        case CursorShape::frame_ne:
        case CursorShape::frame_nw:
        case CursorShape::frame_se:
        case CursorShape::frame_sw: return frame_cursor(s);
    }
    return [NSCursor arrowCursor];
}

NSDictionary* no_actions() {
    static NSDictionary* d = @{
        @"contents" : [NSNull null],
        @"bounds" : [NSNull null],
        @"position" : [NSNull null],
        @"frame" : [NSNull null],
        @"contentsScale" : [NSNull null],
        @"sublayers" : [NSNull null],
        @"onOrderIn" : [NSNull null],
        @"onOrderOut" : [NSNull null],
    };
    return d;
}

}  // namespace

class MacWindow final : public IWindow {
public:
    MacWindow(MacLoop& loop, const MacConfig& cfg, IWindowListener& listener)
        : loop_(loop), cfg_(cfg), listener_(listener) {}

    ~MacWindow() override { teardown(false); }

    Result<void> init(const WindowDesc& desc) {
        const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        const float w = desc.placement.rect.w > 0 ? desc.placement.rect.w : 800;
        const float h = desc.placement.rect.h > 0 ? desc.placement.rect.h : 500;
        NSRect content = NSMakeRect(0, 0, w, h);
        quick_ = desc.kind == WindowKind::quick_terminal;
        if (quick_) {
            KtQuickPanel* panel = [[KtQuickPanel alloc]
                initWithContentRect:content
                          styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                            backing:NSBackingStoreBuffered
                              defer:NO];
            panel.floatingPanel = YES;
            panel.hidesOnDeactivate = NO;
            panel.becomesKeyOnlyIfNeeded = NO;
            window_ = panel;
        } else {
            window_ = [[NSWindow alloc] initWithContentRect:content
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        }
        if (!window_) return make_error(Errc::os_error, "NSWindow init failed");
        window_.releasedWhenClosed = NO;
        window_.contentMinSize = NSMakeSize(desc.min_size.w, desc.min_size.h);
        if (quick_) {
            window_.level = NSFloatingWindowLevel;
            window_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                         NSWindowCollectionBehaviorFullScreenAuxiliary |
                                         NSWindowCollectionBehaviorIgnoresCycle;
        } else {
            window_.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        }
        window_.title = to_ns(desc.title);
        window_.tabbingMode = NSWindowTabbingModeDisallowed;

        // The owners are connected at the end: no listener callbacks from inside create_window.
        delegate_ = [[KtWindowDelegate alloc] init];
        window_.delegate = delegate_;

        view_ = [[KtView alloc] initWithFrame:content];
        // Layer-hosting view: set the layer first, then wantsLayer.
        root_ = [CALayer layer];
        root_.actions = no_actions();
        root_.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        view_.layer = root_;
        view_.wantsLayer = YES;
        view_.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        window_.contentView = view_;
        [window_ makeFirstResponder:view_];
        [view_ registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];

        if (quick_ || desc.placement.rect.x != 0 || desc.placement.rect.y != 0) {
            set_placement(desc.placement);
        } else {
            [window_ center];
        }
        scale_ = static_cast<float>(window_.backingScaleFactor);
        root_.contentsScale = scale_;
        update_sizes(false);
        update_refresh_interval();
        if (desc.placement.fullscreen) want_fullscreen_ = true;
        view_.owner = this;
        delegate_.owner = this;
        return {};
    }

    // ------------------------------------------------------------ IWindow
    void show(bool activate) override {
        if (!window_) return;
        if (quick_) {
            [window_ makeKeyAndOrderFront:nil];   // the panel does not activate the app
        } else if (activate) {
            [NSApp activateIgnoringOtherApps:YES];
            [window_ makeKeyAndOrderFront:nil];
        } else {
            [window_ orderFront:nil];
        }
        if (want_fullscreen_) {
            want_fullscreen_ = false;
            set_fullscreen(true);
        }
    }

    void hide() override {
        if (window_) [window_ orderOut:nil];
    }

    void close() override {
        if (window_) [window_ close];   // windowWillClose: -> teardown(true)
    }

    void set_title(std::u8string_view t) override {
        if (window_) window_.title = to_ns(t);
    }

    void set_placement(const WindowPlacement& p) override {
        if (!window_) return;
        NSScreen* screen = NSScreen.screens.firstObject;
        const CGFloat top = screen ? NSMaxY(screen.frame) : 0;
        const NSRect content = NSMakeRect(p.rect.x, top - p.rect.y - p.rect.h, p.rect.w, p.rect.h);
        [window_ setFrame:[window_ frameRectForContentRect:content] display:YES];
        if (quick_) return;
        if (p.maximized != static_cast<bool>(window_.zoomed)) [window_ zoom:nil];
        set_fullscreen(p.fullscreen);
    }

    WindowPlacement placement() const override {
        WindowPlacement p;
        if (!window_) return p;
        const NSRect content = [window_ contentRectForFrameRect:window_.frame];
        NSScreen* screen = NSScreen.screens.firstObject;
        const CGFloat top = screen ? NSMaxY(screen.frame) : 0;
        p.rect = DipRect{static_cast<float>(content.origin.x), static_cast<float>(top - NSMaxY(content)),
                         static_cast<float>(content.size.width), static_cast<float>(content.size.height)};
        p.maximized = window_.zoomed;
        p.fullscreen = is_fullscreen();
        return p;
    }

    void set_fullscreen(bool on) override {
        if (!window_) return;
        if (is_fullscreen() != on) [window_ toggleFullScreen:nil];
    }

    void minimize() override {
        if (window_) [window_ miniaturize:nil];
    }

    void toggle_maximize() override {
        if (window_) [window_ zoom:nil];
    }

    void set_cursor(CursorShape s) override {
        cursor_ = s;
        if (window_) [window_ invalidateCursorRectsForView:view_];
        if (s == CursorShape::hidden) [NSCursor setHiddenUntilMouseMoves:YES];
    }

    void set_hit_test(std::function<HitArea(DipPoint)> fn) override { hit_test_ = std::move(fn); }

    float scale() const override { return scale_; }
    PxSize client_size_px() const override { return px_; }
    DipSize client_size() const override { return dip_; }
    bool focused() const override { return focused_; }
    void request_attention() override {
        if (![NSApp isActive]) [NSApp requestUserAttention:NSInformationalRequest];
    }

    Result<NativeSurface> create_surface(const SurfaceRequest& req) override {
        if (!window_) return make_error(Errc::unavailable, "window is closed");
        if (req.kind != SurfaceRequest::Kind::software_blit) {
            return make_error(Errc::not_supported, "only software_blit surfaces exist so far");
        }
        destroy_surface();
        surface_ = [CALayer layer];
        surface_.actions = no_actions();
        surface_.anchorPoint = CGPointZero;
        surface_.frame = view_.bounds;
        surface_.contentsGravity = kCAGravityTopLeft;
        surface_.contentsScale = scale_;
        surface_.opaque = YES;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [root_ addSublayer:surface_];
        [CATransaction commit];
        NativeSurface s;
        s.ws = WindowSystem::cocoa;
        s.handle = (__bridge void*)view_;
        s.layer = (__bridge void*)surface_;
        return s;
    }

    void destroy_surface() override {
        if (!surface_) return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [surface_ removeFromSuperlayer];
        [CATransaction commit];
        surface_ = nil;
    }

    void request_frame() override {
        if (!window_ || frame_pending_) return;
        frame_pending_ = true;
        schedule_frame();
    }

    void set_ime_enabled(bool on) override {
        ime_enabled_ = on;
        if (!on) cancel_composition();
    }

    void set_ime_cursor_rect(DipRect r) override { caret_ = r; }

    void set_secure_input(bool on) override {
        if (on == secure_input_) return;
        secure_input_ = on;
        if (on) EnableSecureEventInput();
        else DisableSecureEventInput();
    }

    void consume_key() override { consumed_ = true; }

    // ------------------------------------------------------------ called by the ObjC classes
    IWindowListener& listener() { return listener_; }
    KtView* view() const { return view_; }
    CursorShape cursor() const { return cursor_; }
    DipRect caret() const { return caret_; }
    bool ime_enabled() const { return ime_enabled_; }
    bool begin_key() {
        consumed_ = false;
        return true;
    }
    bool key_consumed() const { return consumed_; }
    bool option_as_meta(uint64_t flags) const {
        const auto sides = detail::mac_option_sides(flags);
        switch (cfg_.option_as_alt) {
            case OptionAsAlt::none: return false;
            case OptionAsAlt::left: return sides.left;
            case OptionAsAlt::right: return sides.right;
            case OptionAsAlt::both: return sides.left || sides.right;
        }
        return false;
    }

    void on_will_close() { teardown(true); }

    void on_focus(bool f) {
        focused_ = f;
        if (!f) cancel_composition();
        listener_.on_focus(f);
    }

    void on_occlusion(bool occluded) {
        if (occluded == occluded_) return;
        occluded_ = occluded;
        listener_.on_occluded(occluded);
        if (!occluded && frame_pending_ && !frame_timer_) schedule_frame();
    }

    void on_screen_changed() { update_refresh_interval(); }

    void on_backing_changed() {
        if (!window_) return;
        const float s = static_cast<float>(window_.backingScaleFactor);
        if (s != scale_ && s > 0) {
            scale_ = s;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            root_.contentsScale = s;
            if (surface_) surface_.contentsScale = s;
            [CATransaction commit];
            const NSRect f = [window_ contentRectForFrameRect:window_.frame];
            listener_.on_scale_changed(s, DipRect{static_cast<float>(f.origin.x), static_cast<float>(f.origin.y),
                                                  static_cast<float>(f.size.width), static_cast<float>(f.size.height)});
        }
        update_sizes(true);
    }

    void on_view_resized() {
        if (surface_) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            surface_.frame = view_.bounds;
            [CATransaction commit];
        }
        update_sizes(true);
    }

    // Marked text (IME preedit) state of the view.
    NSString* marked() const { return marked_; }
    NSRange marked_selection() const { return marked_sel_; }

    void set_marked(NSString* s, NSRange sel) {
        if (!ime_enabled_) return;
        if (s.length == 0) {
            const bool had = marked_.length > 0;
            marked_ = nil;
            marked_sel_ = NSMakeRange(0, 0);
            if (had) listener_.on_composition(Composition{});
            return;
        }
        marked_ = [s copy];
        marked_sel_ = sel;
        Composition c;
        c.text = to_u8(marked_);
        std::u16string s16(marked_.length, u'\0');
        [marked_ getCharacters:reinterpret_cast<unichar*>(s16.data()) range:NSMakeRange(0, marked_.length)];
        const size_t loc = std::min<size_t>(sel.location == NSNotFound ? s16.size() : sel.location, s16.size());
        const size_t end = std::min<size_t>(loc + sel.length, s16.size());
        const uint32_t b0 = detail::utf8_offset_of_utf16_index(s16, loc);
        const uint32_t b1 = detail::utf8_offset_of_utf16_index(s16, end);
        const auto total = static_cast<uint32_t>(c.text.size());
        c.caret = b0;
        if (b0 > 0) c.segments.push_back({0, b0, false});
        if (b1 > b0) c.segments.push_back({b0, b1, true});
        if (total > b1) c.segments.push_back({b1, total, false});
        listener_.on_composition(c);
    }

    void commit_text(NSString* s) {
        const bool had = marked_.length > 0;
        marked_ = nil;
        marked_sel_ = NSMakeRange(0, 0);
        if (had) listener_.on_composition(Composition{});
        const std::u8string t = filter_text(to_u8(s));
        if (!t.empty()) listener_.on_text(t);
    }

    void cancel_composition() {
        if (marked_.length == 0) return;
        marked_ = nil;
        marked_sel_ = NSMakeRange(0, 0);
        [view_.inputContext discardMarkedText];
        listener_.on_composition(Composition{});
    }

    // ------------------------------------------------------------ input translation
    KeyEvent make_key(NSEvent* e, KeyAction action) const {
        KeyEvent k;
        k.key = detail::key_from_mac_keycode(e.keyCode);
        k.native_code = e.keyCode;
        k.mods = detail::mods_from_mac_flags(e.modifierFlags);
        k.action = action;
        if (action != KeyAction::release || e.type == NSEventTypeKeyUp) {
            NSString* base = [e charactersByApplyingModifiers:0];
            char32_t c = first_char(base);
            if (c >= 0x80 && detail::is_text_char(c)) {
                const char32_t a = ascii_capable_char(e.keyCode);
                if (a >= 0x20 && a < 0x7F) c = a;
            }
            k.logical = detail::is_text_char(c) ? detail::ascii_lower(c) : 0;
        }
        return k;
    }

    DipPoint mouse_pos(NSEvent* e) const {
        const NSPoint p = [view_ convertPoint:e.locationInWindow fromView:nil];
        return DipPoint{static_cast<float>(p.x), static_cast<float>(view_.bounds.size.height - p.y)};
    }

    // A borderless window moves and resizes by itself where the app's hit test says so
    // (the quick terminal's title bar and edges).
    bool frame_drag(NSEvent* e) {
        if (!hit_test_ || e.buttonNumber != 0) return false;
        const HitArea a = hit_test_(mouse_pos(e));
        if (a == HitArea::client || a == HitArea::minimize || a == HitArea::maximize || a == HitArea::close) return false;
        const bool move = a == HitArea::caption;
        const bool west = a == HitArea::resize_w || a == HitArea::resize_nw || a == HitArea::resize_sw;
        const bool east = a == HitArea::resize_e || a == HitArea::resize_ne || a == HitArea::resize_se;
        const bool north = a == HitArea::resize_n || a == HitArea::resize_nw || a == HitArea::resize_ne;
        const bool south = a == HitArea::resize_s || a == HitArea::resize_sw || a == HitArea::resize_se;
        const NSRect start = window_.frame;
        const NSPoint p0 = NSEvent.mouseLocation;
        const NSSize min = window_.minSize;
        listener_.on_move_size(true);
        for (;;) {
            NSEvent* ev = [window_ nextEventMatchingMask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp];
            if (!ev || ev.type == NSEventTypeLeftMouseUp) break;
            const NSPoint p = NSEvent.mouseLocation;
            const CGFloat dx = p.x - p0.x, dy = p.y - p0.y;   // Cocoa: y grows upwards
            NSRect f = start;
            if (move) f.origin = NSMakePoint(start.origin.x + dx, start.origin.y + dy);
            if (east) f.size.width = std::max(min.width, start.size.width + dx);
            if (west) {
                f.size.width = std::max(min.width, start.size.width - dx);
                f.origin.x = start.origin.x + start.size.width - f.size.width;
            }
            if (north) f.size.height = std::max(min.height, start.size.height + dy);
            if (south) {
                f.size.height = std::max(min.height, start.size.height - dy);
                f.origin.y = start.origin.y + start.size.height - f.size.height;
            }
            [window_ setFrame:f display:YES];
        }
        // A non-activating panel can lose key status to the active app on the way.
        if (quick_ && !window_.isKeyWindow) [window_ makeKeyWindow];
        listener_.on_move_size(false);
        return true;
    }

    void mouse(NSEvent* e, MouseEvent::Type type) {
        if (type == MouseEvent::Type::press && frame_drag(e)) return;
        MouseEvent m;
        m.type = type;
        m.mods = detail::mods_from_mac_flags(e.modifierFlags);
        m.pos = mouse_pos(e);
        if (type == MouseEvent::Type::press || type == MouseEvent::Type::release) {
            switch (e.buttonNumber) {
                case 0: m.button = MouseButton::left; break;
                case 1: m.button = MouseButton::right; break;
                case 2: m.button = MouseButton::middle; break;
                case 3: m.button = MouseButton::back; break;
                case 4: m.button = MouseButton::forward; break;
                default: return;
            }
            if (type == MouseEvent::Type::press) m.click_count = static_cast<uint8_t>(std::min<NSInteger>(e.clickCount, 255));
        }
        listener_.on_mouse(m);
    }

    // Drag and drop: files arrive as paths, anything else as text.
    DipPoint drag_pos(id<NSDraggingInfo> info) {
        const NSPoint p = [view_ convertPoint:info.draggingLocation fromView:nil];
        return DipPoint{static_cast<float>(p.x), static_cast<float>(view_.bounds.size.height - p.y)};
    }
    static std::vector<std::filesystem::path> drag_paths(NSPasteboard* pb) {
        std::vector<std::filesystem::path> out;
        NSArray* urls = [pb readObjectsForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        for (NSURL* u in urls)
            if (u.fileURL && u.path) out.emplace_back(to_u8(u.path));
        return out;
    }
    NSDragOperation drag_over(id<NSDraggingInfo> info) {
        NSPasteboard* pb = info.draggingPasteboard;
        DragOverEvent d;
        d.pos = drag_pos(info);
        d.has_paths = [pb canReadObjectForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        d.has_text = !d.has_paths && [pb availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil;
        d.mods = detail::mods_from_mac_flags(NSEvent.modifierFlags);
        if (!d.has_paths && !d.has_text) return NSDragOperationNone;
        return listener_.on_drag_over(d) == DropEffect::copy ? NSDragOperationCopy : NSDragOperationNone;
    }
    bool drop(id<NSDraggingInfo> info) {
        NSPasteboard* pb = info.draggingPasteboard;
        DropEvent e;
        e.pos = drag_pos(info);
        e.paths = drag_paths(pb);
        if (e.paths.empty()) e.text = to_u8([pb stringForType:NSPasteboardTypeString]);
        if (e.paths.empty() && e.text.empty()) return false;
        listener_.on_drop(e);
        return true;
    }

    void scroll(NSEvent* e) {
        ScrollEvent s;
        s.precise = e.hasPreciseScrollingDeltas;
        s.dx = static_cast<float>(e.scrollingDeltaX);
        s.dy = static_cast<float>(e.scrollingDeltaY);
        s.mods = detail::mods_from_mac_flags(e.modifierFlags);
        s.pos = mouse_pos(e);
        const NSEventPhase ph = e.phase, mph = e.momentumPhase;
        if (mph != NSEventPhaseNone) {
            s.phase = (mph & (NSEventPhaseEnded | NSEventPhaseCancelled)) ? ScrollEvent::Phase::end
                                                                          : ScrollEvent::Phase::momentum;
        } else if (ph & NSEventPhaseBegan) {
            s.phase = ScrollEvent::Phase::begin;
        } else if (ph & (NSEventPhaseEnded | NSEventPhaseCancelled)) {
            s.phase = ScrollEvent::Phase::end;
        } else if (ph & NSEventPhaseChanged) {
            s.phase = ScrollEvent::Phase::update;
        }
        if (!s.precise && s.dx == 0 && s.dy == 0) return;
        listener_.on_scroll(s);
    }

private:
    bool is_fullscreen() const {
        return window_ && (window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
    }

    void update_sizes(bool notify) {
        const NSRect b = view_.bounds;
        const NSRect px = [view_ convertRectToBacking:b];
        const DipSize dip{static_cast<float>(b.size.width), static_cast<float>(b.size.height)};
        const PxSize p{static_cast<int32_t>(std::lround(px.size.width)), static_cast<int32_t>(std::lround(px.size.height))};
        if (p == px_ && dip.w == dip_.w && dip.h == dip_.h) return;
        px_ = p;
        dip_ = dip;
        if (!notify) return;
        listener_.on_resized(px_, dip_);
        // Live resize: draw synchronously so that the content follows the frame.
        if (view_.inLiveResize && frame_pending_) deliver_frame();
    }

    void update_refresh_interval() {
        NSScreen* s = window_.screen ? window_.screen : NSScreen.mainScreen;
        NSInteger fps = s ? s.maximumFramesPerSecond : 60;
        if (fps <= 0) fps = 60;
        pacer_.set_interval(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / static_cast<double>(fps))));
    }

    void schedule_frame() {
        if (occluded_) return;   // delivered when the window becomes visible again
        const auto now = Clock::now();
        const auto due = pacer_.due(now);
        frame_timer_ = loop_.add_timer(due, [this] {
            frame_timer_ = 0;
            if (frame_pending_ && !occluded_) deliver_frame();
        });
    }

    void deliver_frame() {
        if (frame_timer_) {
            loop_.cancel_timer(frame_timer_);
            frame_timer_ = 0;
        }
        frame_pending_ = false;
        const auto target = pacer_.delivered(Clock::now());
        listener_.on_frame(target);
    }

    void teardown(bool notify) {
        if (!window_) return;
        if (frame_timer_) {
            loop_.cancel_timer(frame_timer_);
            frame_timer_ = 0;
        }
        if (secure_input_) set_secure_input(false);
        NSWindow* w = window_;
        window_ = nil;
        view_.owner = nullptr;
        delegate_.owner = nullptr;
        w.delegate = nil;
        if (!notify) [w close];
        surface_ = nil;
        view_ = nil;
        delegate_ = nil;
        root_ = nil;
        if (notify) listener_.on_destroyed();   // last: the listener may delete this window
    }

    MacLoop& loop_;
    MacConfig cfg_;
    IWindowListener& listener_;
    NSWindow* window_ = nil;
    KtView* view_ = nil;
    KtWindowDelegate* delegate_ = nil;
    CALayer* root_ = nil;
    CALayer* surface_ = nil;
    float scale_ = 1;
    PxSize px_;
    DipSize dip_;
    bool focused_ = false;
    bool quick_ = false;   // WindowKind::quick_terminal
    bool occluded_ = false;
    bool want_fullscreen_ = false;
    bool frame_pending_ = false;
    TimerId frame_timer_ = 0;
    detail::FramePacer pacer_;
    CursorShape cursor_ = CursorShape::arrow;
    std::function<HitArea(DipPoint)> hit_test_;
    bool ime_enabled_ = true;
    DipRect caret_;
    bool secure_input_ = false;
    bool consumed_ = false;
    NSString* marked_ = nil;
    NSRange marked_sel_ = NSMakeRange(0, 0);
};

Result<std::unique_ptr<IWindow>> create_mac_window(MacLoop& loop, const MacConfig& cfg, const WindowDesc& desc,
                                                   IWindowListener& listener) {
    auto w = std::make_unique<MacWindow>(loop, cfg, listener);
    if (auto r = w->init(desc); !r) return Unexpected(r.error());
    return std::unique_ptr<IWindow>(std::move(w));
}

}  // namespace kt::platform::mac

using kt::platform::KeyAction;
using kt::platform::MouseEvent;

// ---------------------------------------------------------------- KtWindowDelegate

@implementation KtWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.owner) self.owner->listener().on_close_requested();
    return NO;   // the app calls IWindow::close()
}
- (void)windowWillClose:(NSNotification*)n {
    (void)n;
    if (self.owner) self.owner->on_will_close();
}
- (void)windowDidBecomeKey:(NSNotification*)n {
    (void)n;
    if (self.owner) self.owner->on_focus(true);
}
- (void)windowDidResignKey:(NSNotification*)n {
    (void)n;
    if (self.owner) self.owner->on_focus(false);
}
- (void)windowDidChangeOcclusionState:(NSNotification*)n {
    NSWindow* w = n.object;
    if (self.owner) self.owner->on_occlusion((w.occlusionState & NSWindowOcclusionStateVisible) == 0);
}
- (void)windowDidChangeScreen:(NSNotification*)n {
    (void)n;
    if (self.owner) self.owner->on_screen_changed();
}
@end

// ---------------------------------------------------------------- KtView

@implementation KtView {
    NSTrackingArea* _tracking;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
    (void)e;
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (self.owner) self.owner->on_view_resized();
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.owner) self.owner->on_backing_changed();
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    if (self.owner) self.owner->request_frame();
}

- (void)updateTrackingAreas {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect | NSTrackingCursorUpdate
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (void)resetCursorRects {
    if (self.owner) [self addCursorRect:self.bounds cursor:kt::platform::mac::ns_cursor(self.owner->cursor())];
}

- (void)cursorUpdate:(NSEvent*)e {
    (void)e;
    if (self.owner) [kt::platform::mac::ns_cursor(self.owner->cursor()) set];
}

// ------------------------------------------------------------ keyboard

- (void)keyDown:(NSEvent*)e {
    MacWindow* o = self.owner;
    if (!o) return;
    kt::platform::KeyEvent k = o->make_key(e, e.isARepeat ? KeyAction::repeat : KeyAction::press);
    const bool meta = o->option_as_meta(e.modifierFlags);
    const bool marked = [self hasMarkedText];
    if (meta) {
        k.text = kt::platform::mac::filter_text(kt::platform::mac::to_u8(e.charactersIgnoringModifiers));
    } else if (!marked) {
        k.text = kt::platform::mac::filter_text(kt::platform::mac::to_u8(e.characters));
    }
    // A dead key produces no characters; marked text means an IME composition is running.
    k.composing = marked || (e.characters.length == 0 && !kt::platform::is_modifier_key(k.key));
    o->begin_key();
    o->listener().on_key(k);
    if (!self.owner || o->key_consumed()) return;   // the keymap took it: no text
    if (meta) return;                                // Option as Meta: no text system
    if (!o->ime_enabled()) {
        if (!k.text.empty()) o->listener().on_text(k.text);
        return;
    }
    [self interpretKeyEvents:@[ e ]];
}

- (void)keyUp:(NSEvent*)e {
    MacWindow* o = self.owner;
    if (!o) return;
    o->listener().on_key(o->make_key(e, KeyAction::release));
}

- (void)flagsChanged:(NSEvent*)e {
    MacWindow* o = self.owner;
    if (!o) return;
    kt::platform::KeyEvent k = o->make_key(e, KeyAction::press);
    k.logical = 0;
    const uint16_t side = kt::platform::detail::side_mod_of(k.key);
    bool down = false;
    if (side) {
        down = (k.mods & side) != 0;
    } else if (k.key == kt::platform::Key::caps_lock) {
        down = (k.mods & kt::platform::mod_caps) != 0;
    } else {
        return;   // Fn and other flags without a Key
    }
    k.action = down ? KeyAction::press : KeyAction::release;
    o->listener().on_key(k);
}

// ------------------------------------------------------------ NSTextInputClient

- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    if (!self.owner) return;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    self.owner->commit_text(s);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range {
    (void)range;
    if (!self.owner) return;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    self.owner->set_marked(s, sel);
}

- (void)unmarkText {
    if (!self.owner) return;
    NSString* m = self.owner->marked();
    if (m.length > 0) self.owner->commit_text(m);   // accept the marked text as it is
}

- (BOOL)hasMarkedText {
    return self.owner && self.owner->marked().length > 0;
}

- (NSRange)markedRange {
    if (![self hasMarkedText]) return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(0, self.owner->marked().length);
}

- (NSRange)selectedRange {
    if (![self hasMarkedText]) return NSMakeRange(0, 0);
    return self.owner->marked_selection();
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range;
    (void)actual;
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range;
    if (actual) *actual = NSMakeRange(NSNotFound, 0);
    if (!self.owner || !self.window) return NSZeroRect;
    const kt::platform::DipRect c = self.owner->caret();
    const NSRect in_view = NSMakeRect(c.x, self.bounds.size.height - c.y - c.h, c.w, c.h);
    const NSRect in_window = [self convertRect:in_view toView:nil];
    return [self.window convertRectToScreen:in_window];
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;   // keys were already reported through on_key; no beep
}

// ------------------------------------------------------------ mouse

- (void)mouseDown:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::press);
}
- (void)mouseUp:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::release);
}
- (void)rightMouseDown:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::press);
}
- (void)rightMouseUp:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::release);
}
- (void)otherMouseDown:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::press);
}
- (void)otherMouseUp:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::release);
}
- (void)mouseMoved:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::move);
}
- (void)mouseDragged:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::move);
}
- (void)rightMouseDragged:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::move);
}
- (void)otherMouseDragged:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::move);
}
- (void)mouseEntered:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::enter);
}
- (void)mouseExited:(NSEvent*)e {
    if (self.owner) self.owner->mouse(e, MouseEvent::Type::leave);
}
- (void)scrollWheel:(NSEvent*)e {
    if (self.owner) self.owner->scroll(e);
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return self.owner ? self.owner->drag_over(info) : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    return self.owner ? self.owner->drag_over(info) : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    return self.owner && self.owner->drop(info);
}

@end
