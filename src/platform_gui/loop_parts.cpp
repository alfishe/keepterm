#include "platform_gui/loop_parts.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace kt::platform::detail {

bool TaskQueue::push(std::function<void()> task) {
    {
        std::lock_guard lock(mu_);
        tasks_.push_back(std::move(task));
    }
    return !wake_pending_.exchange(true, std::memory_order_acq_rel);
}

size_t TaskQueue::drain() {
    // Clear the flag before taking the batch: a push that lands after the swap wakes again.
    wake_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(mu_);
        running_.swap(tasks_);
    }
    const size_t n = running_.size();
    for (auto& t : running_) {
        if (t) t();
    }
    running_.clear();
    return n;
}

bool TaskQueue::empty() const {
    std::lock_guard lock(mu_);
    return tasks_.empty();
}

namespace {
bool later(const auto& a, const auto& b) {
    if (a.deadline != b.deadline) return a.deadline > b.deadline;
    return a.id > b.id;
}
}  // namespace

TimerId TimerHeap::add(Clock::time_point deadline, std::function<void()> fn) {
    const TimerId id = next_id_++;
    fns_.emplace(id, std::move(fn));
    heap_.push_back(Entry{deadline, id});
    std::push_heap(heap_.begin(), heap_.end(), [](const Entry& a, const Entry& b) { return later(a, b); });
    return id;
}

void TimerHeap::cancel(TimerId id) { fns_.erase(id); }

void TimerHeap::prune() const {
    while (!heap_.empty() && !fns_.contains(heap_.front().id)) {
        std::pop_heap(heap_.begin(), heap_.end(), [](const Entry& a, const Entry& b) { return later(a, b); });
        heap_.pop_back();
    }
}

std::optional<Clock::time_point> TimerHeap::next_deadline() const {
    prune();
    if (heap_.empty()) return std::nullopt;
    return heap_.front().deadline;
}

size_t TimerHeap::run_due(Clock::time_point now) {
    size_t n = 0;
    for (;;) {
        prune();
        if (heap_.empty() || heap_.front().deadline > now) break;
        const TimerId id = heap_.front().id;
        std::pop_heap(heap_.begin(), heap_.end(), [](const Entry& a, const Entry& b) { return later(a, b); });
        heap_.pop_back();
        auto it = fns_.find(id);
        auto fn = std::move(it->second);
        fns_.erase(it);
        ++n;
        if (fn) fn();
    }
    return n;
}

int32_t wait_timeout_ms(std::optional<Clock::time_point> deadline, Clock::time_point now, int32_t max_ms) {
    if (!deadline) return max_ms;
    if (*deadline <= now) return 0;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(*deadline - now).count();
    const auto ms = (us + 999) / 1000;
    return static_cast<int32_t>(std::min<long long>(ms, max_ms));
}

Clock::time_point FramePacer::due(Clock::time_point now) const {
    if (!last_ || now - *last_ >= interval_) return now;
    return *last_ + interval_;
}

Clock::time_point FramePacer::delivered(Clock::time_point now) {
    last_ = now;
    return now + interval_;
}

uint8_t ClickCounter::press(MouseButton button, uint32_t time_ms, DipPoint pos) {
    const uint32_t dt = time_ms - last_time_;   // unsigned: correct across wrap-around
    const bool near = std::fabs(pos.x - last_pos_.x) <= slop_ && std::fabs(pos.y - last_pos_.y) <= slop_;
    if (count_ > 0 && button == last_button_ && dt <= interval_ms_ && near) {
        if (count_ < 255) ++count_;
    } else {
        count_ = 1;
    }
    last_button_ = button;
    last_time_ = time_ms;
    last_pos_ = pos;
    return count_;
}

}  // namespace kt::platform::detail
