// ConfigService: one reload thread fed by the folder watcher.
#include "config/service.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

#include "base/log.h"
#include "platform/file_watcher.h"
#include "platform/paths.h"

namespace kt::config {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::uint64_t hash_files(std::span<const fs::path> files) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(p[i]);
            h *= 1099511628211ull;
        }
    };
    for (const auto& f : files) {
        const std::string name = platform::paths::to_utf8(f);
        mix(name.data(), name.size() + 1);
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            mix("\x00missing", 8);
            continue;
        }
        char buf[8192];
        while (in.read(buf, sizeof buf) || in.gcount() > 0) mix(buf, static_cast<std::size_t>(in.gcount()));
        mix("\x00end", 4);
    }
    return h;
}

namespace {

struct Sub {
    enum class Kind { Prefix, Class, Report } kind;
    std::string prefix;
    Apply cls = Apply::Live;
    ConfigService::Callback cb;
    std::function<void(const ReloadReport&)> report;
};

struct Registry {
    std::mutex mu;
    std::map<std::uint64_t, std::shared_ptr<Sub>> subs;
    std::uint64_t next = 1;
};

bool prefix_hit(const Sub& s, const ConfigDelta& d) {
    if (s.prefix.empty()) return !d.empty();
    if (d.keys_changed && std::string_view("keys.").starts_with(s.prefix)) return true;
    if (d.keys_changed && s.prefix.starts_with("keys.")) return true;
    // A theme file changed without a key: subscribers of theme.* and colors.* hear it.
    if (d.theme_changed && (std::string_view("theme.").starts_with(s.prefix) || s.prefix.starts_with("theme.") ||
                            std::string_view("colors.").starts_with(s.prefix) || s.prefix.starts_with("colors.")))
        return true;
    return std::any_of(d.changed.begin(), d.changed.end(),
                       [&](KeyId k) { return spec(k).path.starts_with(s.prefix); });
}

// File signature for polling: existence, size and modification time.
std::string signature(std::span<const fs::path> files) {
    std::string sig;
    for (const auto& f : files) {
        std::error_code ec;
        const auto st = fs::status(f, ec);
        if (ec || !fs::exists(st)) {
            sig += "-;";
            continue;
        }
        const auto sz = fs::file_size(f, ec);
        const auto mt = fs::last_write_time(f, ec);
        sig += std::to_string(static_cast<unsigned long long>(sz)) + ":" +
               std::to_string(static_cast<long long>(mt.time_since_epoch().count())) + ";";
    }
    return sig;
}

std::set<fs::path> existing(std::span<const fs::path> files) {
    std::set<fs::path> out;
    for (const auto& f : files) {
        std::error_code ec;
        if (fs::exists(f, ec)) out.insert(f);
    }
    return out;
}

}  // namespace

struct ConfigService::Impl {
    Options opts;
    std::shared_ptr<Registry> reg = std::make_shared<Registry>();

    mutable std::mutex mu;  // guards everything below
    std::condition_variable cv;
    Config current;
    std::uint64_t hash = 0;
    Debouncer debounce{std::chrono::milliseconds(150)};
    bool force = false;
    bool kicked = false;
    bool stop = false;
    std::vector<fs::path> files;          // what the current snapshot depends on
    std::set<fs::path> present;           // the subset of `files` that existed at the last load
    std::set<fs::path> watched;           // folders with an OS watch
    bool poll = false;                    // some folder could not be watched
    std::string poll_sig;
    Clock::time_point next_poll{};

    std::unique_ptr<platform::FileWatcher> watcher;
    std::thread thread;

    void on_events(std::span<const platform::FsEvent> evs) {
        std::lock_guard lock(mu);
        bool hit = false;
        for (const auto& e : evs) {
            if (e.kind == platform::FsEvent::Kind::overflow) {
                hit = true;
                break;
            }
            for (const auto& f : files) {
                if (f.parent_path() == e.dir && f.filename() == e.name) {
                    hit = true;
                    break;
                }
            }
            if (hit) break;
        }
        if (!hit) return;
        debounce.event(Clock::now());
        kicked = true;
        cv.notify_all();
    }

    // Called with `mu` held.
    void update_watches() {
        std::set<fs::path> want;
        for (const auto& f : files) want.insert(f.parent_path());
        poll = false;
        for (auto it = watched.begin(); it != watched.end();) {
            if (!want.contains(*it)) {
                if (watcher) watcher->remove_dir(*it);
                it = watched.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& d : want) {
            if (watched.contains(d)) continue;
            if (!watcher) {
                poll = true;
                continue;
            }
            std::error_code ec;
            if (!fs::is_directory(d, ec)) {
                poll = true;  // the folder appears later: polling notices it
                continue;
            }
            if (auto r = watcher->add_dir(d); r) {
                watched.insert(d);
            } else {
                poll = true;
                log::info("config", "folder watch unavailable, polling: " + r.error().what);
            }
        }
        poll_sig = signature(files);
        next_poll = Clock::now() + opts.poll_interval;
    }

    void run() {
        std::unique_lock lock(mu);
        while (!stop) {
            auto wake = Clock::now() + std::chrono::hours(1);
            if (auto d = debounce.deadline()) wake = std::min(wake, *d);
            if (poll) wake = std::min(wake, next_poll);
            cv.wait_until(lock, wake, [&] { return stop || force || kicked; });
            kicked = false;  // a new event moved the deadline: recompute the wake time
            if (stop) break;
            const auto now = Clock::now();
            if (poll && now >= next_poll) {
                next_poll = now + opts.poll_interval;
                const std::string sig = signature(files);
                if (sig != poll_sig) {
                    poll_sig = sig;
                    debounce.event(now);
                }
            }
            const bool due = debounce.take_due(now);
            if (!due && !force) continue;
            const bool forced = force;
            force = false;
            reload(lock, forced);
        }
    }

    // Called with `lock` held; releases it while reading and parsing.
    void reload(std::unique_lock<std::mutex>& lock, bool forced) {
        const std::vector<fs::path> old_files = files;
        const std::uint64_t old_hash = hash;
        const Config before = current;
        const std::set<fs::path> present_before = present;
        lock.unlock();

        const std::uint64_t h = hash_files(old_files);
        if (h == old_hash && !forced) {
            lock.lock();
            return;  // unchanged content (for example the Settings window's own write)
        }
        Config next = Config::load(opts.load);
        const std::uint64_t next_hash = hash_files(next.files());

        ReloadReport rep;
        rep.diagnostics.assign(next.diagnostics().begin(), next.diagnostics().end());
        const std::set<fs::path> now_present = existing(next.files());
        for (const auto& f : present_before) {
            if (!now_present.contains(f)) {
                // A deleted file is no input: defaults for its keys; say so once.
                Diagnostic dg;
                dg.level = Diagnostic::Level::Info;
                dg.file = display_path(f);
                dg.message = "the file was removed; its keys use their defaults";
                rep.diagnostics.push_back(std::move(dg));
            }
        }

        lock.lock();
        hash = next_hash;
        present = now_present;
        files.assign(next.files().begin(), next.files().end());
        if (next.file_rejected()) {
            // A syntax error during a session keeps the last good snapshot.
            rep.rejected = true;
            rep.generation = before.generation();
            update_watches();
            lock.unlock();
            publish(before, nullptr, rep);
            lock.lock();
            return;
        }
        current = next;
        rep.applied = true;
        rep.generation = next.generation();
        update_watches();
        lock.unlock();
        auto delta = std::make_shared<ConfigDelta>(diff(before, next));
        publish(next, delta, rep);
        lock.lock();
    }

    void publish(const Config& cfg, std::shared_ptr<ConfigDelta> delta, const ReloadReport& rep) {
        auto task = [reg = reg, cfg, delta, rep] {
            std::vector<std::shared_ptr<Sub>> subs;
            {
                std::lock_guard l(reg->mu);
                for (const auto& [id, s] : reg->subs) subs.push_back(s);
            }
            for (const auto& s : subs) {
                switch (s->kind) {
                    case Sub::Kind::Report: s->report(rep); break;
                    case Sub::Kind::Prefix:
                        if (delta && prefix_hit(*s, *delta)) s->cb(cfg, *delta);
                        break;
                    case Sub::Kind::Class:
                        if (delta && delta->any(s->cls)) s->cb(cfg, *delta);
                        break;
                }
            }
        };
        if (opts.post) opts.post(std::move(task));
        else task();
    }

    Subscription add(std::shared_ptr<Sub> s) {
        std::uint64_t id;
        {
            std::lock_guard l(reg->mu);
            id = reg->next++;
            reg->subs.emplace(id, std::move(s));
        }
        std::weak_ptr<Registry> weak = reg;
        return Subscription([weak, id] {
            if (auto r = weak.lock()) {
                std::lock_guard l(r->mu);
                r->subs.erase(id);
            }
        });
    }
};

ConfigService::ConfigService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ConfigService::~ConfigService() {
    {
        std::lock_guard lock(impl_->mu);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->watcher.reset();  // its thread ends before Impl goes away
}

Result<std::unique_ptr<ConfigService>> ConfigService::open(Options opts) {
    auto impl = std::make_unique<Impl>();
    impl->opts = std::move(opts);
    impl->debounce = Debouncer(impl->opts.debounce);
    impl->current = Config::load(impl->opts.load);
    impl->files.assign(impl->current.files().begin(), impl->current.files().end());
    impl->hash = hash_files(impl->files);
    impl->present = existing(impl->files);
    Impl* raw = impl.get();
    if (impl->opts.watch) {
        auto w = platform::FileWatcher::create([raw](std::span<const platform::FsEvent> evs) { raw->on_events(evs); });
        if (w) impl->watcher = std::move(*w);
        else log::info("config", "file watcher unavailable, polling: " + w.error().what);
        std::lock_guard lock(impl->mu);
        impl->update_watches();
    }
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<ConfigService>(new ConfigService(std::move(impl)));
}

Config ConfigService::current() const {
    std::lock_guard lock(impl_->mu);
    return impl_->current;
}

Subscription ConfigService::subscribe(std::string_view prefix, Callback cb) {
    auto s = std::make_shared<Sub>();
    s->kind = Sub::Kind::Prefix;
    s->prefix = std::string(prefix);
    s->cb = std::move(cb);
    return impl_->add(std::move(s));
}

Subscription ConfigService::subscribe(Apply cls, Callback cb) {
    auto s = std::make_shared<Sub>();
    s->kind = Sub::Kind::Class;
    s->cls = cls;
    s->cb = std::move(cb);
    return impl_->add(std::move(s));
}

Subscription ConfigService::on_reload(std::function<void(const ReloadReport&)> cb) {
    auto s = std::make_shared<Sub>();
    s->kind = Sub::Kind::Report;
    s->report = std::move(cb);
    return impl_->add(std::move(s));
}

void ConfigService::reload_now() {
    {
        std::lock_guard lock(impl_->mu);
        impl_->force = true;
    }
    impl_->cv.notify_all();
}

std::vector<fs::path> ConfigService::watched_dirs() const {
    std::lock_guard lock(impl_->mu);
    return {impl_->watched.begin(), impl_->watched.end()};
}

bool ConfigService::polling() const {
    std::lock_guard lock(impl_->mu);
    return impl_->poll;
}

}  // namespace kt::config
