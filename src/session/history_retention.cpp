// Retention of the session history per project.
#include <algorithm>
#include <map>
#include <memory>

#include "platform/paths.h"
#include "session/history_index.h"

namespace kt::session {

namespace {

config::ProjectRef project_ref(const json::Value& project) {
    config::ProjectRef r;
    if (project.kind() != json::Value::Kind::Object) return r;
    r.name = project.str_or("name");
    const std::string path = project.str_or("path");
    if (!path.empty()) r.folder = platform::paths::from_utf8(expand_tilde(path, os::home_dir()));
    return r;
}

// The keep_closed of a series: the project's override, else `global` (the config's or the one
// Settings asks about).
uint64_t keep_closed_of(const config::Config& cfg, const json::Value& project, std::optional<int64_t> global) {
    const config::ProjectRef ref = project_ref(project);
    if (!ref.name.empty() || !ref.folder.empty()) {
        const config::Config pc = config::resolve_for_project(cfg, ref);
        if (pc.origin(config::KeyId::storage_keep_closed).layer == config::Layer::Project) {
            return history_policy(cfg, project).keep_closed_ms;
        }
    }
    if (global) return *global > 0 ? static_cast<uint64_t>(*global) : store::kOff;
    return store::policy_from_config(cfg).keep_closed_ms;
}

}  // namespace

store::Policy history_policy(const config::Config& cfg, const json::Value& project) {
    const config::ProjectRef ref = project_ref(project);
    if (ref.name.empty() && ref.folder.empty()) return store::policy_from_config(cfg);
    return store::policy_from_config(config::resolve_for_project(cfg, ref));
}

store::Janitor::PolicyFor history_policy_for(const config::Config& cfg, const os::Paths& p) {
    if (cfg.project_overrides().empty()) return {};
    auto projects = std::make_shared<std::map<std::string, json::Value>>();
    for (auto& e : list_history(p)) (*projects)[e.id] = std::move(e.project);
    auto cache = std::make_shared<std::map<std::string, store::Policy>>();
    const store::Policy base = store::policy_from_config(cfg);
    return [cfg, projects, cache, base](const store::SessionRow& row) {
        if (auto it = cache->find(row.id); it != cache->end()) return it->second;
        const auto pr = projects->find(row.id);
        const store::Policy pol = pr == projects->end() ? base : history_policy(cfg, pr->second);
        cache->emplace(row.id, pol);
        return pol;
    };
}

RetentionPreview retention_preview(const os::Paths& p, const config::Config& cfg, uint64_t now_ms,
                                   std::optional<int64_t> keep_closed_ms) {
    RetentionPreview out;
    for (auto& e : list_history(p)) {
        if (e.state != HistoryState::closed || !e.session.empty() || !e.ended_ms || e.pinned) continue;
        const uint64_t keep = keep_closed_of(cfg, e.project, keep_closed_ms);
        if (store::add_sat(*e.ended_ms, keep) >= now_ms) continue;  // as the janitor; kOff: for good
        out.stored_bytes += e.stats.stored_bytes;
        out.due.push_back(std::move(e));
    }
    std::sort(out.due.begin(), out.due.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return *a.ended_ms < *b.ended_ms; });
    return out;
}

}  // namespace kt::session
