// Internal declarations shared by the Cocoa files (Objective-C++, ARC).
#pragma once

#import <AppKit/AppKit.h>

#include <memory>
#include <unordered_map>

#include "platform_gui/loop_parts.h"
#include "platform_gui/platform.h"

namespace kt::platform::mac {

class MacLoop final : public IEventLoop {
public:
    MacLoop();
    ~MacLoop() override;

    int  run() override;
    void quit(int exit_code) override;
    void post(std::function<void()> task) override;
    TimerId add_timer(Clock::time_point deadline, std::function<void()> fn) override;
    void    cancel_timer(TimerId) override;
    Result<void> add_waitable(Waitable, std::function<void()> on_signalled) override;
    void         remove_waitable(Waitable) override;

    void on_source();    // CFRunLoopSource perform: posted tasks
    void on_timer();     // CFRunLoopTimer: due timers

private:
    void reschedule();

    detail::TaskQueue tasks_;
    detail::TimerHeap timers_;
    CFRunLoopRef main_ = nullptr;
    CFRunLoopSourceRef source_ = nullptr;
    CFRunLoopTimerRef timer_ = nullptr;
    std::unordered_map<int, dispatch_source_t> waitables_;
    int exit_code_ = 0;
    bool quit_requested_ = false;
};

struct MacConfig {
    OptionAsAlt option_as_alt = OptionAsAlt::none;
};

Result<std::unique_ptr<IWindow>> create_mac_window(MacLoop& loop, const MacConfig& cfg, const WindowDesc& desc,
                                                   IWindowListener& listener);

}  // namespace kt::platform::mac
