#include "app/mac_window.h"

#import <Cocoa/Cocoa.h>

#include <utility>

// The menu items' target: forwards the command id to the C++ side.
@interface KTMenuTarget : NSObject <NSMenuItemValidation>
@property(nonatomic) std::function<void(const std::string&)> invoke;
@property(nonatomic) std::function<bool(const std::string&)> enabled;
@end

@implementation KTMenuTarget
- (void)run:(NSMenuItem*)item {
    NSString* cmd = item.representedObject;
    if (cmd && self.invoke) self.invoke(std::string(cmd.UTF8String));
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    NSString* cmd = item.representedObject;
    if (!cmd || !self.enabled) return YES;
    return self.enabled(std::string(cmd.UTF8String)) ? YES : NO;
}
@end

namespace kt::app::mac {

namespace {

KTMenuTarget* g_target = nil;

NSWindow* window_of(void* ns_view) {
    NSView* v = (__bridge NSView*)ns_view;
    return v ? v.window : nil;
}

NSString* str(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

NSMenu* build(const std::string& title, const std::vector<MenuEntry>& items) {
    NSMenu* m = [[NSMenu alloc] initWithTitle:str(title)];
    for (const MenuEntry& e : items) {
        if (e.separator) {
            [m addItem:[NSMenuItem separatorItem]];
            continue;
        }
        if (!e.submenu.empty()) {
            NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:str(e.title) action:nil keyEquivalent:@""];
            it.submenu = build(e.title, e.submenu);
            [m addItem:it];
            continue;
        }
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:str(e.title) action:@selector(run:) keyEquivalent:str(e.key)];
        it.target = g_target;
        it.representedObject = str(e.command);
        NSEventModifierFlags mask = 0;
        if (e.cmd) mask |= NSEventModifierFlagCommand;
        if (e.shift) mask |= NSEventModifierFlagShift;
        if (e.alt) mask |= NSEventModifierFlagOption;
        if (e.ctrl) mask |= NSEventModifierFlagControl;
        it.keyEquivalentModifierMask = mask;
        [m addItem:it];
    }
    return m;
}

}  // namespace

void use_unified_title_bar(void* ns_view) {
    NSWindow* w = window_of(ns_view);
    if (!w) return;
    w.styleMask |= NSWindowStyleMaskFullSizeContentView;
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility = NSWindowTitleHidden;
    w.movableByWindowBackground = NO;
}

void drag_window(void* ns_view) {
    NSWindow* w = window_of(ns_view);
    NSEvent* e = NSApp.currentEvent;
    if (w && e && e.type == NSEventTypeLeftMouseDown) [w performWindowDragWithEvent:e];
}

void title_bar_double_click(void* ns_view) {
    NSWindow* w = window_of(ns_view);
    if (!w) return;
    NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"None"]) return;
    if ([action isEqualToString:@"Minimize"]) [w performMiniaturize:nil];
    else [w performZoom:nil];
}

void install_menu_bar(const std::string& app_name, const std::vector<MenuEntry>& menus,
                      std::function<void(const std::string&)> invoke, std::function<bool(const std::string&)> enabled) {
    if (!g_target) g_target = [[KTMenuTarget alloc] init];
    g_target.invoke = std::move(invoke);
    g_target.enabled = std::move(enabled);
    NSString* name = str(app_name);

    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] initWithTitle:name];
    [app_menu addItemWithTitle:[@"About " stringByAppendingString:name]
                        action:@selector(orderFrontStandardAboutPanel:)
                 keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    {
        NSMenuItem* s = [[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(run:) keyEquivalent:@","];
        s.target = g_target;
        s.representedObject = @"app.settings";
        [app_menu addItem:s];
    }
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* others = [app_menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app_menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    {
        NSMenuItem* q = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                   action:@selector(run:)
                                            keyEquivalent:@"q"];
        q.target = g_target;
        q.representedObject = @"app.quit";
        [app_menu addItem:q];
        // Option shows the alternate: end the sessions too (the running programs are listed first).
        NSMenuItem* qe = [[NSMenuItem alloc] initWithTitle:@"Quit and End All Sessions…" action:@selector(run:) keyEquivalent:@"q"];
        qe.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
        qe.alternate = YES;
        qe.target = g_target;
        qe.representedObject = @"app.quit_end_sessions";
        [app_menu addItem:qe];
    }
    app_item.submenu = app_menu;

    for (const MenuEntry& top : menus) {
        NSMenuItem* item = [[NSMenuItem alloc] init];
        item.submenu = build(top.title, top.submenu);
        [bar addItem:item];
        if (top.title == "Window") {
            NSMenu* wm = item.submenu;
            [wm insertItem:[NSMenuItem separatorItem] atIndex:0];
            NSMenuItem* fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
            [wm insertItem:fs atIndex:0];
            [wm insertItem:[[NSMenuItem alloc] initWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""] atIndex:0];
            [wm insertItem:[[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"]
                   atIndex:0];
            NSApp.windowsMenu = wm;
        }
    }
    NSApp.mainMenu = bar;
}

}  // namespace kt::app::mac
