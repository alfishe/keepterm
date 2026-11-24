// The GUI's pane state files (see gui_state.h).

#include "session/gui_state.h"

#include <algorithm>

#include "session/history.h"
#include "session/json.h"
#include "session/registry.h"

namespace kt::session {

std::string windows_dir(const os::Paths& p) { return os::join(p.state, "windows"); }

Result<void> write_pane_state(const os::Paths& p, const PaneState& s) {
    const std::string dir = windows_dir(p);
    if (auto r = os::make_private_dir(dir); !r) return r;
    auto v = json::Value::object();
    v.set("schema", json::Value::integer(1));
    v.set("session", json::Value::string(s.session.str()));
    v.set("cwd", json::Value::string(s.cwd));
    v.set("cwd_source", json::Value::string(s.cwd_source));
    v.set("title", json::Value::string(s.title));
    v.set("updated_unix_ms", json::Value::integer(static_cast<int64_t>(s.updated_unix_ms ? s.updated_unix_ms : os::unix_ms())));
    return os::write_file_atomic(os::join(dir, s.session.str() + ".json"), v.dump(true) + "\n");
}

void remove_pane_state(const os::Paths& p, const SessionId& id) {
    (void)os::remove_file(os::join(windows_dir(p), id.str() + ".json"));
}

std::vector<PaneState> read_pane_states(const os::Paths& p) {
    std::vector<PaneState> out;
    const std::string dir = windows_dir(p);
    for (const std::string& name : os::list_dir(dir)) {
        if (name.size() != SessionId::kLength + 5 || name.substr(SessionId::kLength) != ".json") continue;
        auto text = os::read_file(os::join(dir, name), 1 << 20);
        if (!text) continue;
        auto v = json::parse(*text);
        if (!v || v->int_or("schema", 0) != 1) continue;
        auto id = SessionId::parse(v->str_or("session"));
        if (!id || id->str() != name.substr(0, SessionId::kLength)) continue;
        PaneState s;
        s.session = *id;
        s.cwd = v->str_or("cwd");
        s.cwd_source = v->str_or("cwd_source");
        s.title = v->str_or("title");
        s.updated_unix_ms = v->uint_or("updated_unix_ms", 0);
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const PaneState& a, const PaneState& b) { return a.updated_unix_ms < b.updated_unix_ms; });
    return out;
}

PaneAction decide_pane(const PaneFacts& f) {
    if (f.host_live) return f.gui_clients > 0 ? PaneAction::Skip : PaneAction::Attach;
    if (f.host_unresponsive) return PaneAction::Skip;
    if (f.ended) return PaneAction::Drop;
    // Without a folder there is no history, but the pane and its cwd are still worth a shell
    // (history = false panes have only record.json, which also lives in the folder).
    return f.state_folder ? PaneAction::Restore : PaneAction::Drop;
}

std::vector<StartupItem> plan_startup(const std::vector<PaneState>& panes,
                                      const std::function<PaneFacts(const PaneState&)>& facts) {
    std::vector<StartupItem> out;
    for (const auto& p : panes) out.push_back({p, decide_pane(facts(p))});
    return out;
}

PaneFacts real_pane_facts(const os::Paths& paths, const PaneState& s) {
    PaneFacts f;
    Registry reg(paths);
    if (auto d = reg.find(s.session)) {
        f.host_live = d->state == HostState::Live;
        f.host_unresponsive = d->state == HostState::Unresponsive;
        if (d->hello) f.gui_clients = d->hello->gui_clients();
    }
    const std::string dir = os::join(paths.state_sessions, s.session.str());
    f.state_folder = os::is_dir(dir);
    if (auto rec = read_recovery_record(dir)) f.ended = rec->exit_code.has_value();
    return f;
}

}  // namespace kt::session
