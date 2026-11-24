// OS appearance on macOS (platform_gui/appearance.h). Compiled with ARC.
#include "platform_gui/appearance.h"

#import <AppKit/AppKit.h>

#include <chrono>

using kt::platform::Appearance;

@interface KtAppearanceObserver : NSObject
@property(nonatomic, copy) void (^onChange)(void);
@end

@implementation KtAppearanceObserver
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    (void)keyPath;
    (void)object;
    (void)change;
    (void)context;
    if (self.onChange) self.onChange();
}
@end

namespace kt::platform {

namespace {

Appearance from_defaults() {
    NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    return (style && [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame) ? Appearance::dark : Appearance::light;
}

class MacAppearanceWatch final : public AppearanceWatch {
public:
    MacAppearanceWatch(IEventLoop& loop, std::function<void(Appearance)> cb) : loop_(loop), cb_(std::move(cb)) {
        current_ = system_appearance();
        if (NSApp) {
            observer_ = [[KtAppearanceObserver alloc] init];
            MacAppearanceWatch* self = this;
            observer_.onChange = ^{
                self->check();
            };
            [NSApp addObserver:observer_ forKeyPath:@"effectiveAppearance" options:NSKeyValueObservingOptionNew context:nullptr];
        } else {
            schedule_poll();
        }
    }
    ~MacAppearanceWatch() override {
        if (observer_) [NSApp removeObserver:observer_ forKeyPath:@"effectiveAppearance"];
        if (timer_) loop_.cancel_timer(timer_);
    }
    Appearance current() const override { return current_; }

private:
    void check() {
        const Appearance a = system_appearance();
        if (a == current_) return;
        current_ = a;
        if (cb_) cb_(a);
    }
    void schedule_poll() {
        timer_ = loop_.add_timer(Clock::now() + std::chrono::seconds(2), [this] {
            timer_ = 0;
            check();
            schedule_poll();
        });
    }

    IEventLoop& loop_;
    std::function<void(Appearance)> cb_;
    KtAppearanceObserver* observer_ = nil;
    TimerId timer_ = 0;
    Appearance current_ = Appearance::light;
};

}  // namespace

Appearance system_appearance() {
    if (!NSApp) return from_defaults();
    NSAppearanceName best = [NSApp.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [best isEqualToString:NSAppearanceNameDarkAqua] ? Appearance::dark : Appearance::light;
}

std::unique_ptr<AppearanceWatch> watch_appearance(IEventLoop& loop, std::function<void(Appearance)> on_change) {
    return std::make_unique<MacAppearanceWatch>(loop, std::move(on_change));
}

}  // namespace kt::platform
