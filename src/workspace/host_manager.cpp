// ISessionManager over the session registry and `keepterm --session-host` (kt_workspace_os).
//
// The host command line is built here (not with session::spawn_host) because the workspace
// chooses the session id itself. The pane's environment goes as
// `--env K=V` options. An older host that does not know the option answers "ERR unknown option
// --env"; it is started again without them (the variables are then missing in that shell) and
// later spawns skip them.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "base/log.h"
#include "session/client.h"
#include "session/os.h"
#include "session/registry.h"
#include "workspace/runtime.h"

namespace kt::ws {

namespace {

class HostSessionManager final : public ISessionManager {
public:
    HostSessionManager(Poster post, HostManagerOptions opts) : post_(std::move(post)), opts_(std::move(opts)) {}

    ~HostSessionManager() override {
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        alive_->store(false);
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    static constexpr int kConfirmProbes = 3;
    static constexpr int kConfirmTimeoutMs = 2000;

    void probe(const session::SessionId& id, std::function<void(ProbeResult, std::string)> done) override {
        const int timeout = opts_.probe_timeout_ms;
        submit([id, timeout, done = std::move(done)]() mutable -> std::function<void()> {
            ProbeResult r = ProbeResult::NotFound;
            std::string endpoint;
            auto paths = session::os::paths();
            if (paths) {
                session::Registry reg(*paths);
                auto d = reg.find(id, timeout);
                // A busy machine or a host stopped for a moment looks hung to one probe,
                // and Unreachable makes the binder give the pane a new session and leave
                // the old one: so it takes three more probes with a longer timeout to say so.
                for (int i = 0; i < kConfirmProbes && d && d->state == session::HostState::Unresponsive; ++i)
                    d = reg.find(id, kConfirmTimeoutMs);
                if (d) {
                    switch (d->state) {
                        case session::HostState::Live:
                            r = ProbeResult::Live;
                            endpoint = d->record.endpoint.empty() ? session::os::endpoint_name(*paths, id)
                                                                  : d->record.endpoint;
                            break;
                        case session::HostState::Unresponsive: r = ProbeResult::Unreachable; break;
                        case session::HostState::Incompatible: r = ProbeResult::VersionTooNew; break;
                        case session::HostState::Stale: r = ProbeResult::NotFound; break;
                    }
                }
            }
            return [r, endpoint = std::move(endpoint), done = std::move(done)]() mutable { done(r, std::move(endpoint)); };
        });
    }

    void spawn(const SpawnSpec& spec, std::function<void(WsResult<std::string>)> done) override {
        submit([this, spec, done = std::move(done)]() mutable -> std::function<void()> {
            auto r = spawn_blocking(spec);
            return [r = std::move(r), done = std::move(done)]() mutable { done(std::move(r)); };
        });
    }

    void kill(const session::SessionId& id, std::function<void(bool)> done) override {
        submit([id, done = std::move(done)]() mutable -> std::function<void()> {
            const bool ok = kill_blocking(id);
            return [ok, done = std::move(done)]() mutable { done(ok); };
        });
    }
    void end_keep_history(const session::SessionId& id, std::function<void(bool)> done) override {
        submit([id, done = std::move(done)]() mutable -> std::function<void()> {
            const bool ok = kill_blocking(id, session::proto::KillMode::Suspend);
            return [ok, done = std::move(done)]() mutable { done(ok); };
        });
    }

private:
    using Task = std::function<std::function<void()>()>;   // runs on a worker, returns the UI part

    void submit(Task task) {
        std::lock_guard lock(mu_);
        if (stop_) return;
        tasks_.push_back(std::move(task));
        if (idle_ == 0 && workers_.size() < kMaxWorkers) workers_.emplace_back([this] { worker(); });
        cv_.notify_one();
    }

    void worker() {
        std::unique_lock lock(mu_);
        for (;;) {
            ++idle_;
            cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
            --idle_;
            if (stop_) return;
            Task task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            std::function<void()> ui = task();
            if (ui && alive_->load()) {
                auto alive = alive_;
                post_([alive, ui = std::move(ui)] {
                    if (alive->load()) ui();
                });
            }
            lock.lock();
        }
    }

    WsResult<std::string> spawn_blocking(const SpawnSpec& spec) {
        auto paths = session::os::paths();
        if (!paths) return ws_error(WsErrc::Io, "the session folders are not available", paths.error().what);
        const std::string exe = opts_.exe.empty() ? session::os::self_exe() : opts_.exe;
        if (exe.empty()) return ws_error(WsErrc::Io, "cannot find the keepterm executable");
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool with_env = !env_unsupported_.load();
            std::vector<std::string> args = {exe, "--session-host", spec.id.str(), "--ready", "{ready}"};
            if (spec.restore_from) {
                args.push_back("--restore-from");
                args.push_back(spec.restore_from->str());
            }
            if (!spec.cwd.empty()) {
                args.push_back("--cwd");
                args.push_back(spec.cwd);
            }
            args.push_back("--size");
            args.push_back(std::to_string(spec.size.cols) + "x" + std::to_string(spec.size.rows));
            if (!spec.history) args.push_back("--no-history");
            for (const auto& a : opts_.host_args) args.push_back(a);
            if (with_env)
                for (const auto& [k, v] : spec.env) {
                    if (k == "KEEPTERM_SESSION") continue;   // the host sets it itself
                    args.push_back("--env");
                    args.push_back(k + "=" + v);
                }
            if (!spec.argv.empty()) {
                args.push_back("--");
                for (const auto& a : spec.argv) args.push_back(a);
            }
            auto sp = session::os::spawn_detached(args, std::chrono::milliseconds(opts_.spawn_timeout_ms));
            if (!sp) return ws_error(WsErrc::Io, "the session host did not start", sp.error().what);
            if (sp->ready_line.rfind("OK", 0) == 0) return session::os::endpoint_name(*paths, spec.id);
            if (with_env && sp->ready_line.find("unknown option --env") != std::string::npos) {
                if (!env_unsupported_.exchange(true))
                    kt::log::warn("workspace", "the session host has no --env option; KEEPTERM_PANE is not set in shells");
                continue;
            }
            return ws_error(WsErrc::Io, "the session host failed", sp->ready_line);
        }
        return ws_error(WsErrc::Io, "the session host failed");
    }

    // KillTree: the host deletes its history when it ends; Suspend: it saves and keeps it.
    static bool kill_blocking(const session::SessionId& id, session::proto::KillMode mode = session::proto::KillMode::KillTree) {
        auto paths = session::os::paths();
        if (!paths) return false;
        session::Registry reg(*paths);
        auto d = reg.find(id);
        if (!d) return true;   // nothing runs
        const auto& host = d->record.host;
        auto running = [&] { return host.pid && session::os::process_matches(host.pid, host.start_time); };
        if (d->state == session::HostState::Live) {
            session::proto::Kill k;
            k.mode = mode;
            auto r = session::request_kill(d->record.endpoint, k);
            if (r && r->ok)
                for (int i = 0; i < 40 && running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (running()) {
            if (auto r = session::os::kill_process(host.pid, host.start_time); !r) return false;
            for (int i = 0; i < 20 && running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (d->state != session::HostState::Unresponsive) reg.remove(id);
        return !running();
    }

    static constexpr std::size_t kMaxWorkers = 8;

    Poster post_;
    HostManagerOptions opts_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    std::size_t idle_ = 0;
    bool stop_ = false;
    std::atomic<bool> env_unsupported_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace

std::unique_ptr<ISessionManager> make_host_session_manager(Poster post, HostManagerOptions opts) {
    return std::make_unique<HostSessionManager>(std::move(post), std::move(opts));
}

}  // namespace kt::ws
