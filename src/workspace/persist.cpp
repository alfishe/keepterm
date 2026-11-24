#include "workspace/persist.h"

#include <algorithm>
#include <chrono>

namespace kt::ws {

PersistScheduler::PersistScheduler(Workspace& ws, StateStore& store, IClock& clock, PersistOptions opts)
    : ws_(ws), store_(store), clock_(clock), opts_(std::move(opts)) {
    sub_ = ws_.subscribe([this](std::span<const Change> c) { on_changes(c); });
    if (opts_.mode == PersistOptions::Mode::Thread) worker_ = std::thread([this] { writer_loop(); });
    if (opts_.post) timer_ = std::thread([this] { timer_loop(); });
}

PersistScheduler::~PersistScheduler() {
    alive_->store(false);
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    timer_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (timer_.joinable()) timer_.join();
}

void PersistScheduler::on_changes(std::span<const Change> changes) {
    bool any = false, immediate = false;
    for (const Change& c : changes) {
        if (!c.persistent) continue;
        any = true;
        if (c.kind == ChangeKind::PaneChanged && (c.fields & field::kSession)) immediate = true;
    }
    if (any) mark_dirty(immediate);
}

void PersistScheduler::mark_dirty(bool immediate) {
    const std::int64_t now = clock_.steady_ms();
    ++gen_;
    if (!dirty_) first_dirty_ = now;
    last_change_ = now;
    dirty_ = true;
    immediate_ = immediate_ || immediate;
    kick_timer();
}

std::optional<std::int64_t> PersistScheduler::next_deadline() const {
    if (read_only_ || !dirty_) return std::nullopt;
    if (failed_) return retry_at_;
    if (immediate_) return last_change_;
    return std::min(last_change_ + opts_.idle_ms, first_dirty_ + opts_.max_delay_ms);
}

void PersistScheduler::kick_timer() {
    if (!opts_.post) return;
    const auto d = next_deadline();
    {
        std::lock_guard lock(mu_);
        timer_deadline_ = d ? *d : -1;
    }
    timer_cv_.notify_all();
}

void PersistScheduler::timer_loop() {
    std::unique_lock lock(mu_);
    while (!stop_) {
        if (timer_deadline_ < 0 || poll_posted_.load()) {
            timer_cv_.wait_for(lock, std::chrono::milliseconds(1000));
            continue;
        }
        const std::int64_t wait = timer_deadline_ - clock_.steady_ms();
        if (wait > 0) {
            timer_cv_.wait_for(lock, std::chrono::milliseconds(std::min<std::int64_t>(wait, 1000)));
            continue;
        }
        timer_deadline_ = -1;
        poll_posted_.store(true);
        lock.unlock();
        auto alive = alive_;
        opts_.post([this, alive] {
            if (!alive->load()) return;
            poll_posted_.store(false);
            poll();
        });
        lock.lock();
    }
}

void PersistScheduler::collect() {
    std::optional<WriteResult> r;
    {
        std::lock_guard lock(mu_);
        r = std::move(result_);
        result_.reset();
    }
    if (r) finish_write(*r);
}

void PersistScheduler::finish_write(const WriteResult& r) {
    if (r.ok) {
        written_gen_.store(std::max(written_gen_.load(), r.gen));
        last_hash_ = r.hash;
        have_hash_ = true;
        failed_ = false;
        retry_delay_ = 0;
        last_error_.clear();
        if (opts_.on_written) opts_.on_written();
    } else {
        failed_ = true;
        last_error_ = r.error;
        retry_delay_ = retry_delay_ == 0 ? opts_.retry_min_ms : std::min(retry_delay_ * 2, opts_.retry_max_ms);
        retry_at_ = clock_.steady_ms() + retry_delay_;
        dirty_ = true;   // the same content is due again
        have_hash_ = false;
    }
    kick_timer();
}

void PersistScheduler::poll() {
    collect();
    const auto d = next_deadline();
    if (!d || clock_.steady_ms() < *d) return;
    {
        std::lock_guard lock(mu_);
        if (writing_ || pending_) return;   // one at a time; the next poll writes the rest
    }
    (void)write_now(false);
}

WsResult<void> PersistScheduler::write_now(bool sync) {
    const std::string content = serialize_state(ws_.state(), opts_.writer, -1);
    const std::size_t hash = std::hash<std::string>{}(content);
    const std::uint64_t gen = gen_;
    dirty_ = false;
    immediate_ = false;
    if (have_hash_ && hash == last_hash_ && !failed_) {
        written_gen_.store(std::max(written_gen_.load(), gen));   // identical content: nothing to write
        kick_timer();
        return {};
    }
    std::string bytes = serialize_state(ws_.state(), opts_.writer, clock_.unix_ms());
    if (sync || opts_.mode == PersistOptions::Mode::Inline) {
        WriteResult r{gen, false, {}, hash};
        WsResult<void> res = ws_error(WsErrc::Io, "a write is still running");
        {
            std::unique_lock wl(write_mu_, std::defer_lock);
            if (wl.try_lock_for(std::chrono::milliseconds(1000))) {
                res = store_.write(bytes);
                writes_.fetch_add(1);
            }
        }
        r.ok = res.has_value();
        if (!r.ok) r.error = res.error().message + (res.error().detail.empty() ? "" : ": " + res.error().detail);
        finish_write(r);
        return res;
    }
    {
        std::lock_guard lock(mu_);
        pending_ = Job{gen, std::move(bytes), hash};
    }
    cv_.notify_all();
    kick_timer();
    return {};
}

void PersistScheduler::writer_loop() {
    std::unique_lock lock(mu_);
    for (;;) {
        cv_.wait(lock, [&] { return stop_ || pending_.has_value(); });
        if (stop_) return;
        Job job = std::move(*pending_);
        pending_.reset();
        writing_ = true;
        lock.unlock();
        WriteResult r{job.gen, false, {}, job.hash};
        {
            std::lock_guard wl(write_mu_);
            auto res = store_.write(job.bytes);
            writes_.fetch_add(1);
            r.ok = res.has_value();
            if (!r.ok) r.error = res.error().message + (res.error().detail.empty() ? "" : ": " + res.error().detail);
        }
        lock.lock();
        writing_ = false;
        result_ = std::move(r);
        if (opts_.post && !stop_) {
            lock.unlock();
            auto alive = alive_;
            opts_.post([this, alive] {
                if (alive->load()) collect();
            });
            lock.lock();
        }
    }
}

WsResult<void> PersistScheduler::flush() {
    if (read_only_) return {};
    bool queued = false;
    {
        std::lock_guard lock(mu_);
        if (pending_) {
            pending_.reset();   // superseded by the write below
            queued = true;
        }
    }
    // Wait for a running write so its result is in before deciding.
    {
        std::unique_lock wl(write_mu_, std::defer_lock);
        if (wl.try_lock_for(std::chrono::milliseconds(1000))) wl.unlock();
    }
    collect();
    if (!dirty_ && !queued && !failed_) return {};
    if (queued) have_hash_ = false;   // the dropped job never landed
    return write_now(true);
}

void PersistScheduler::set_read_only(bool ro, int file_schema) {
    read_only_ = ro;
    ro_schema_ = file_schema;
    kick_timer();
}

WsResult<void> PersistScheduler::save_anyway() {
    if (ro_schema_ > 0) {
        if (auto r = store_.backup_newer(ro_schema_); !r) return r;
    }
    read_only_ = false;
    ro_schema_ = 0;
    have_hash_ = false;
    dirty_ = true;
    ++gen_;
    return write_now(true);
}

PersistState PersistScheduler::state() const {
    {
        std::lock_guard lock(mu_);
        if (writing_ || pending_) return PersistState::Writing;
    }
    if (failed_) return PersistState::Failed;
    if (dirty_) return PersistState::Dirty;
    return PersistState::Clean;
}

std::string PersistScheduler::last_error() const { return last_error_; }

}  // namespace kt::ws
