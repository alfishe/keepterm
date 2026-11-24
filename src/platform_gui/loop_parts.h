// OS-independent parts of the event loops: the posted-task queue,
// the timer heap, frame pacing and click counting. Used by every OS loop and by unit tests.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "platform_gui/event_loop.h"
#include "platform_gui/geometry.h"
#include "platform_gui/input.h"

namespace kt::platform::detail {

// Multi-producer task queue with a coalesced wake-up flag. push() is callable from any thread
// and returns true when the caller must wake the UI thread (the first push after a drain).
class TaskQueue {
public:
    bool push(std::function<void()> task);
    // UI thread. Runs every task queued before the call, in FIFO order. Tasks posted while
    // draining run on the next drain. Returns the number of tasks run.
    size_t drain();
    bool empty() const;

private:
    mutable std::mutex mu_;
    std::vector<std::function<void()>> tasks_;
    std::vector<std::function<void()>> running_;
    std::atomic<bool> wake_pending_{false};
};

// Min-heap of one-shot timers. UI thread only. Callbacks may add and cancel timers.
class TimerHeap {
public:
    TimerId add(Clock::time_point deadline, std::function<void()> fn);
    void cancel(TimerId);
    std::optional<Clock::time_point> next_deadline() const;
    // Runs every timer whose deadline is <= now, earliest first (ties in add order).
    size_t run_due(Clock::time_point now);
    size_t size() const { return fns_.size(); }

private:
    struct Entry {
        Clock::time_point deadline;
        TimerId id;
    };
    void prune() const;
    mutable std::vector<Entry> heap_;
    std::unordered_map<TimerId, std::function<void()>> fns_;
    TimerId next_id_ = 1;
};

// Milliseconds until `deadline`, rounded up, clamped to [0, max_ms]. No deadline = max_ms.
int32_t wait_timeout_ms(std::optional<Clock::time_point> deadline, Clock::time_point now, int32_t max_ms);

// Frame pacing of the software path: one on_frame per refresh interval; a
// request when the last frame is older than one interval is served at once.
class FramePacer {
public:
    explicit FramePacer(Clock::duration interval = std::chrono::microseconds(16667)) : interval_(interval) {}
    void set_interval(Clock::duration d) { interval_ = d; }
    Clock::duration interval() const { return interval_; }
    // When the requested frame should run. Does not change state.
    Clock::time_point due(Clock::time_point now) const;
    // Records a delivered frame and returns its target present time.
    Clock::time_point delivered(Clock::time_point now);

private:
    Clock::duration interval_;
    std::optional<Clock::time_point> last_;
};

// Multi-click detection for window systems without a native click count (X11, Wayland, Win32
// message order differs; macOS reports clickCount itself).
class ClickCounter {
public:
    ClickCounter(uint32_t interval_ms = 500, float slop = 4.0f) : interval_ms_(interval_ms), slop_(slop) {}
    void set_interval(uint32_t ms) { interval_ms_ = ms; }
    void set_slop(float px) { slop_ = px; }
    // A press of `button` at `time_ms` (OS time stamp, wraps) and `pos`; returns 1, 2, 3 ...
    uint8_t press(MouseButton button, uint32_t time_ms, DipPoint pos);
    void reset() { count_ = 0; }

private:
    uint32_t interval_ms_;
    float slop_;
    MouseButton last_button_ = MouseButton::none;
    uint32_t last_time_ = 0;
    DipPoint last_pos_;
    uint8_t count_ = 0;
};

}  // namespace kt::platform::detail
