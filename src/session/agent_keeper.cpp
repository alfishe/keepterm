#include "session/agent_keeper.h"

#include "agents/context_os.h"

#include <thread>

#include "agents/capture.h"
#include "agents/os_access.h"
#include "agents/record_json.h"
#include "agents/resume_command.h"
#include "base/log.h"
#include "session/cwd.h"

namespace kt::session {

using namespace std::chrono_literals;

AgentKeeper::AgentKeeper(AgentSettings settings, AgentPane pane, const agents::IFileSystem* fs,
                         const agents::IProcessProbe* probe)
    : settings_(std::move(settings)), pane_(std::move(pane)) {
    if (!fs) {
        own_fs_ = std::make_unique<agents::StdFileSystem>();
        fs = own_fs_.get();
    }
    if (!probe) {
        own_probe_ = std::make_unique<agents::OsProcessProbe>();
        probe = own_probe_.get();
    }
    fs_ = fs;
    probe_ = probe;
    if (!pane_.now) pane_.now = [] { return std::chrono::steady_clock::now(); };
    if (!pane_.sleep) pane_.sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
    if (!pane_.cancelled) pane_.cancelled = [] { return false; };
    if (!pane_.last_input_ns) pane_.last_input_ns = [] { return uint64_t{0}; };
    rebuild_registry();
}

AgentKeeper::~AgentKeeper() = default;

void AgentKeeper::rebuild_registry() {
    auto reg = std::make_shared<agents::AgentRegistry>(agents::AgentRegistry::builtin());
    std::string dir;
    {
        std::lock_guard lk(mu_);
        dir = settings_.claude_config_dir;
    }
    const agents::SourcePaths paths = agents::default_source_paths(dir);
    if (!codex_index_) {
        const std::string sq = agents::environment_variable("CODEX_SQLITE_HOME");
        codex_index_ = std::make_unique<agents::CodexSqliteIndex>(sq.empty() ? paths.codex_home : sq);
    }
    agents::attach_builtin_sources(*reg, *fs_, *probe_, paths, codex_index_.get());
    std::lock_guard lk(mu_);
    registry_ = std::move(reg);
}

void AgentKeeper::set_settings(const AgentSettings& s) {
    bool dir_changed = false;
    {
        std::lock_guard lk(mu_);
        dir_changed = s.claude_config_dir != settings_.claude_config_dir;
        settings_ = s;
    }
    if (dir_changed) rebuild_registry();
}

AgentSettings AgentKeeper::settings() const {
    std::lock_guard lk(mu_);
    return settings_;
}

std::vector<agents::ProcessFacts> AgentKeeper::facts_of(const proto::ProcessInfo& pi) {
    std::vector<agents::ProcessFacts> out;
    for (const auto& e : pi.foreground_chain) {
        agents::ProcessFacts f;
        f.argv = e.argv;
        f.image = e.image;
        f.comm = e.comm;
        f.pid = e.pid;
        f.start_time = e.start_unix_ns;
        f.cwd = e.cwd;
        out.push_back(std::move(f));
    }
    return out;
}

bool AgentKeeper::idle_shell(const proto::ProcessInfo& pi) {
    return pi.foreground_pid != 0 && pi.foreground_is_shell && pi.foreground_chain.empty();
}

std::optional<agents::AgentResume> AgentKeeper::capture(const proto::ProcessInfo& pi,
                                                        const std::optional<agents::AgentResume>& current) const {
    if (pi.foreground_pid == 0) return current;   // no foreground known: the record stays
    std::shared_ptr<agents::AgentRegistry> reg;
    AgentSettings s;
    {
        std::lock_guard lk(mu_);
        reg = registry_;
        s = settings_;
    }
    const auto facts = facts_of(pi);
    auto rec = agents::build_record(facts, pi.cwd, *reg, current);
    // agents.restore_skip: that agent's sessions are not recorded.
    if (rec && !s.restores(rec->agent)) return std::nullopt;
    return rec;
}

bool AgentKeeper::wait_until(std::chrono::milliseconds timeout, std::chrono::milliseconds poll,
                             const std::function<bool()>& pred) {
    const auto deadline = pane_.now() + timeout;
    for (;;) {
        if (pane_.cancelled()) return false;
        if (pred()) return true;
        if (pane_.now() >= deadline) return false;
        pane_.sleep(poll);
    }
}

std::string AgentKeeper::restore(const agents::AgentResume& rec, std::string_view session_id) {
    std::shared_ptr<agents::AgentRegistry> reg;
    double scale = 1.0;
    bool skip = false;
    {
        std::lock_guard lk(mu_);
        reg = registry_;
        scale = settings_.restore_timeout_scale;
        skip = !settings_.restores(rec.agent);
    }
    const std::string prefix = "agent restore: " + rec.agent + " " + rec.session_id + " in " + std::string(session_id) + ": ";
    auto done = [&](std::string outcome, bool error = false) {
        if (error) log::error("agents", prefix + outcome);
        else log::info("agents", prefix + outcome);
        return outcome;
    };
    if (skip) return done("restore is off for " + rec.agent + " (agents.restore_skip)");
    const agents::AgentDescriptor* d = reg->find(rec.agent);
    if (!d || !d->resumable()) return done("agent " + rec.agent + " is not resumable by this build");

    // Step 3: a PROCESS_INFO with a foreground.
    std::optional<proto::ProcessInfo> pi;
    auto refresh = [&] {
        pi = pane_.process_info();
        return pi.has_value();
    };
    if (!wait_until(timings_.foreground, timings_.poll, [&] { return refresh() && pi->foreground_pid != 0; })) {
        return done(pi ? "the pane has no foreground process" : "the terminal is not live");
    }
    const uint64_t input_mark = pane_.last_input_ns();
    // Step 4: the same agent runs already.
    {
        const auto facts = facts_of(*pi);
        if (auto m = agents::recognize_chain(facts, *reg); m && m->agent->id == rec.agent) return done("already running");
    }
    // Step 5: an idle interactive shell.
    if (!wait_until(timings_.idle, timings_.poll, [&] { return refresh() && idle_shell(*pi); })) {
        return done(pi ? "the pane runs another program" : "the terminal is not live");
    }
    // Step 6: settle, then idle again (rc files and the integration hook may still run).
    pane_.sleep(timings_.settle);
    if (pane_.cancelled()) return done("the terminal is not live");
    if (!refresh()) return done("the terminal is not live");
    if (!idle_shell(*pi)) return done("the shell is not idle");
    // Step 7: the user typed since step 3.
    if (pane_.last_input_ns() != input_mark) return done("the user took over");
    // Step 8: a live process has the session.
    if (const agents::ISessionSource* src = reg->source(rec.agent); src && src->session_is_running(rec.session_id)) {
        return done("the session runs somewhere else");
    }
    // Step 9: the directory.
    if (rec.directory.empty() || !directory_exists(rec.directory)) return done("directory " + rec.directory + " is gone", true);
    // Step 10: the shell and the quoting.
    const agents::ShellKind kind = agents::shell_kind(pi->shell_image);
    if (kind == agents::ShellKind::Unsupported) return done("shell " + pi->shell_image + " is not supported");
#ifdef _WIN32
    constexpr agents::PathStyle style = agents::PathStyle::Windows;
#else
    constexpr agents::PathStyle style = agents::PathStyle::Posix;
#endif
    auto cmd = agents::resume_command(rec, *d, kind, style);
    if (!cmd) return done("cannot quote " + cmd.error().what);
    // Step 11 (memory pressure) has no platform source yet: the ready wait is the throttle.
    // Step 12: type the line and Enter as plain input.
    if (!pane_.type(*cmd + "\r")) return done("the terminal is not live");
    {
        std::vector<std::string> shown = agents::redact_for_log(rec.arguments);
        std::string args;
        for (const auto& a : shown) args += " " + a;
        log::info("agents", prefix + "started (arguments:" + args + ")");
    }
    // Step 13: ready.
    const auto ready_timeout = std::chrono::milliseconds(static_cast<int64_t>(
        static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(d->ready_timeout).count()) * scale));
    const bool ready = wait_until(ready_timeout, timings_.ready_poll, [&] {
        if (!refresh()) return false;
        const auto facts = facts_of(*pi);
        return agents::agent_is_ready(facts, rec.agent, pi->cwd, *reg);
    });
    if (!ready) return done(pane_.cancelled() ? "the terminal is not live" : "not ready in time");
    // Step 14: settle after ready (Qoder).
    if (d->post_ready_settle.count() > 0) pane_.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(d->post_ready_settle));
    return done("ready");
}

std::optional<agents::AgentResume> agent_record_from_json(const json::Value& v) {
    if (v.is_null()) return std::nullopt;
    return agents::agent_resume_from_json(std::string_view(v.dump(false)));
}

json::Value agent_record_to_json(const std::optional<agents::AgentResume>& r) {
    if (!r) return json::Value::null();
    auto v = json::parse(agents::to_json(*r));
    return v ? *v : json::Value::null();
}

}  // namespace kt::session
