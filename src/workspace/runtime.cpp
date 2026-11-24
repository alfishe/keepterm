#include "workspace/runtime.h"

#include <chrono>
#include <thread>

#include "base/log.h"
#include "config/config.h"
#include "session/os.h"
#include "workspace/paths.h"

namespace kt::ws {

namespace {
WorkspaceOptions model_options(const config::Config& cfg, std::string home) {
    WorkspaceOptions o;
    o.home_dir = std::move(home);
    o.auto_tile = cfg.splits_auto_tile();
    o.close_last_pane_closes_tab = cfg.tabs_close_last_pane_closes_tab();
    return o;
}
}  // namespace

std::unique_ptr<WorkspaceRuntime> WorkspaceRuntime::create(RuntimeOptions opts) {
    std::unique_ptr<WorkspaceRuntime> rt(new WorkspaceRuntime());
    WorkspaceRuntime& r = *rt;
    r.opts_ = std::move(opts);
    r.config_ = std::make_shared<const config::Config>(r.opts_.config ? *r.opts_.config : config::Config());
    if (r.opts_.state_dir.empty()) {
        if (auto p = session::os::paths()) r.opts_.state_dir = p->state;
    }
    if (r.opts_.home_dir.empty()) r.opts_.home_dir = session::os::home_dir();
    r.opts_.home_dir = to_slashes(r.opts_.home_dir);

    r.fs_ = make_os_file_system();
    r.ws_ = std::make_unique<Workspace>(system_random(), system_clock(), model_options(*r.config_, r.opts_.home_dir));
    r.store_ = std::make_unique<StateStore>(*r.fs_, system_clock(), system_random(), r.opts_.state_dir);
    std::function<std::vector<LegacyPane>()> legacy;
    if (r.opts_.import_legacy) legacy = [] { return read_legacy_panes(); };
    r.outcome_ = r.store_->load(r.opts_.home_dir, legacy,
                                [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
    for (const auto& note : r.outcome_.repairs.notes) kt::log::warn("workspace", "state repaired: " + note);
    if (!r.outcome_.notice.empty()) kt::log::warn("workspace", r.outcome_.notice);
    r.ws_->load(r.outcome_.state);

    PersistOptions po;
    po.writer = r.opts_.writer;
    po.post = r.opts_.post;
    r.legacy_imported_ = r.outcome_.status == LoadStatus::ImportedLegacy;
    WorkspaceRuntime* self = &r;
    po.on_written = [self] {
        if (!self->legacy_imported_) return;
        self->legacy_imported_ = false;
        retire_legacy_panes();   // the import is in workspace.json now
    };
    r.persist_ = std::make_unique<PersistScheduler>(*r.ws_, *r.store_, system_clock(), std::move(po));
    r.persist_->set_read_only(r.outcome_.read_only, r.outcome_.status == LoadStatus::NewerSchema ? r.outcome_.file_schema : 0);
    if (r.outcome_.status != LoadStatus::Loaded || r.outcome_.repairs.any()) r.persist_->request_save();

    r.sm_ = r.opts_.session_manager ? std::move(r.opts_.session_manager)
                                    : make_host_session_manager(r.opts_.post, r.opts_.hosts);
    BinderOptions bo;
    bo.restart_dead_sessions = r.config_->sessions_restart_dead_sessions();
    bo.profile = r.config_->profile();
    bo.control_endpoint = r.opts_.control_endpoint;
    bo.history_enabled = [self](const Project* project) {
        const auto cfg = self->config_;
        if (!project) return cfg->session_history();
        return config::resolve_for_project(*cfg, config::ProjectRef{project->name, std::filesystem::path(
                                                     std::u8string(project->path.begin(), project->path.end()))})
            .session_history();
    };
    r.binder_ = std::make_unique<SessionBinder>(*r.ws_, *r.sm_, system_random(), std::move(bo));

    r.usage_ = std::make_unique<UsageStore>(*r.fs_, system_clock(), r.opts_.state_dir);
    if (auto corrupt = r.usage_->load())
        kt::log::warn("workspace", "palette usage file was damaged and was kept as " + *corrupt);
    return rt;
}

WorkspaceRuntime::~WorkspaceRuntime() {
    // Order: the binder and the scheduler unsubscribe before the model goes.
    binder_.reset();
    sm_.reset();
    persist_.reset();
    legacy_sub_.reset();
}

void WorkspaceRuntime::start() { binder_->start(); }

void WorkspaceRuntime::flush() {
    if (auto r = persist_->flush(); !r) kt::log::error("workspace", "saving the workspace failed: " + r.error().message);
    (void)usage_->flush();
}

void WorkspaceRuntime::tick() {
    persist_->poll();
    usage_->maybe_save();
}

void WorkspaceRuntime::apply_config(const config::Config& cfg) {
    config_ = std::make_shared<const config::Config>(cfg);
    WorkspaceOptions o = model_options(cfg, opts_.home_dir);
    ws_->set_options(std::move(o));
    binder_->set_restart_dead_sessions(cfg.sessions_restart_dead_sessions());
}

}  // namespace kt::ws
