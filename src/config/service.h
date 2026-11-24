// Live reload: folder watcher, debounce, content hash, diff, subscriptions.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "config/config.h"

namespace kt::config {

// Debounce state with an explicit clock, so tests can drive it.
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;
    explicit Debouncer(Clock::duration delay) : delay_(delay) {}
    void event(Clock::time_point now) { deadline_ = now + delay_; }
    std::optional<Clock::time_point> deadline() const { return deadline_; }
    // True once when the quiet period after the last event has passed.
    bool take_due(Clock::time_point now) {
        if (!deadline_ || now < *deadline_) return false;
        deadline_.reset();
        return true;
    }

private:
    Clock::duration delay_;
    std::optional<Clock::time_point> deadline_;
};

// 64-bit FNV-1a over the existence and contents of `files`, in order.
std::uint64_t hash_files(std::span<const std::filesystem::path> files);

// Unsubscribes on destruction. Move-only.
class Subscription {
public:
    Subscription() = default;
    Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}  // NOLINT
    Subscription(Subscription&& o) noexcept : cancel_(std::move(o.cancel_)) { o.cancel_ = nullptr; }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            reset();
            cancel_ = std::move(o.cancel_);
            o.cancel_ = nullptr;
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }
    void reset() {
        if (cancel_) cancel_();
        cancel_ = nullptr;
    }

private:
    std::function<void()> cancel_;
};

struct ReloadReport {
    bool applied = false;   // a new snapshot was published
    bool rejected = false;  // a file did not parse; the last good snapshot stays
    std::uint64_t generation = 0;
    std::vector<Diagnostic> diagnostics;
};

class ConfigService {
public:
    using Executor = std::function<void(std::function<void()>)>;
    using Callback = std::function<void(const Config&, const ConfigDelta&)>;

    struct Options {
        LoadOptions load;
        std::chrono::milliseconds debounce{150};
        std::chrono::milliseconds poll_interval{5000};  // for folders the watcher cannot watch
        bool watch = true;
        // Where subscribers run (the UI thread's post). Empty: on the reload thread.
        Executor post;
    };

    static Result<std::unique_ptr<ConfigService>> open(Options opts);
    ~ConfigService();
    ConfigService(const ConfigService&) = delete;
    ConfigService& operator=(const ConfigService&) = delete;

    Config current() const;

    // Called when a key under `prefix` changes ("font.", "terminal.cursor_", "" for any change;
    // "keys." also fires for [keys] binding changes).
    Subscription subscribe(std::string_view prefix, Callback cb);
    // Called when the change set holds at least one key of this reload class.
    Subscription subscribe(Apply cls, Callback cb);
    // Called after every reload attempt that read changed content (and after reload_now).
    Subscription on_reload(std::function<void(const ReloadReport&)> cb);

    // Forces a reload, skipping the content hash ("Reload Config" command). Asynchronous.
    void reload_now();

    // Folders watched by events, and files checked by polling (diagnostics, tests).
    std::vector<std::filesystem::path> watched_dirs() const;
    bool polling() const;

    struct Impl;

private:
    explicit ConfigService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace kt::config
