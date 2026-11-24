// The UI event loop.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "base/error.h"

namespace kt::platform {

using TimerId = uint64_t;            // 0 is never a valid id
using Clock   = std::chrono::steady_clock;

#if defined(_WIN32)
using Waitable = void*;   // HANDLE
#else
using Waitable = int;     // fd, readable == signalled
#endif

class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    // Runs until quit(). Dispatches OS events, posted tasks, timers, waitables, frames.
    virtual int  run() = 0;
    virtual void quit(int exit_code) = 0;

    // Any thread. Tasks run on the UI thread in FIFO order. Wake-ups are coalesced.
    virtual void post(std::function<void()> task) = 0;

    // UI thread.
    virtual TimerId add_timer(Clock::time_point deadline, std::function<void()> fn) = 0;
    virtual void    cancel_timer(TimerId) = 0;

    // A handle or fd that the loop waits on together with OS events. The callback runs on the
    // UI thread each time the handle is signalled (level-triggered for fds).
    virtual Result<void> add_waitable(Waitable, std::function<void()> on_signalled) = 0;
    virtual void         remove_waitable(Waitable) = 0;
};

}  // namespace kt::platform
