#include "app/desktop.h"

#import <Cocoa/Cocoa.h>

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace kt::app::desktop {

bool open_url(const std::string& url) {
    if (!url_allowed(url)) return false;
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    return u && [[NSWorkspace sharedWorkspace] openURL:u];
}

bool open_folder(const std::string& path) {
    if (path.empty()) return false;
    NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()] isDirectory:YES];
    return u && [[NSWorkspace sharedWorkspace] openURL:u];
}

bool open_file(const std::string& path) {
    if (path.empty()) return false;
    NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    return u && [[NSWorkspace sharedWorkspace] openURL:u];
}

bool run_detached(const std::vector<std::string>& argv_in) {
    if (argv_in.empty()) return false;
    // Double fork: no zombie, no wait on the UI thread. The vector is built before fork().
    std::vector<std::string> argv = argv_in;
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (fork() != 0) _exit(0);
        execvp(args[0], args.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    return true;
}

bool pick_folder(void* native_view, const std::string& title, const std::string& initial,
                 std::function<void(std::string)> done, const Post&) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.message = [NSString stringWithUTF8String:title.c_str()];
    panel.prompt = @"Choose";
    if (!initial.empty())
        panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:initial.c_str()] isDirectory:YES];
    auto finish = [panel, done = std::move(done)](NSModalResponse r) {
        if (r != NSModalResponseOK || panel.URLs.count == 0) return;
        const char* p = panel.URLs.firstObject.path.UTF8String;
        if (p) done(std::string(p));
    };
    NSView* view = (__bridge NSView*)native_view;
    if (view.window) {
        [panel beginSheetModalForWindow:view.window completionHandler:^(NSModalResponse r) { finish(r); }];
    } else {
        [panel beginWithCompletionHandler:^(NSModalResponse r) { finish(r); }];
    }
    return true;
}

void set_badge(int count) {
    NSString* label = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
    [[NSApp dockTile] setBadgeLabel:label];
}

}  // namespace kt::app::desktop
